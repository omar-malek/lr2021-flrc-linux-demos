/* lr2021_flrc.c - FLRC packet TX / RX test for LR2021 on Luckfox (Linux spidev + sysfs GPIO).
 * Commands from LR2021 datasheet Rev 2.1 (sections 6, 7, 8.1, 18.4).
 *
 *   lr2021_flrc tx [-n count] [-l len] [-i interval_ms] [-P dBm] ...
 *   lr2021_flrc rx [-t seconds] ...
 *
 * Common options: -d /dev/spidev0.0 -s 8000000 -b 54 -r 55 -f 2440000000 -R rate(0..7) -c cr(0..3)
 * HF PA power is limited to 0..10 dBm (conducted). EN 300 328 non-adaptive limit is 10 dBm EIRP:
 * subtract the TX antenna gain. Default is 0 dBm for bench tests.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/spi/spidev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

#define BUSY_TIMEOUT_US  100000
#define NO_BUSY_DELAY_US 1000

#define IRQ_RXDONE  (1u << 18)
#define IRQ_TXDONE  (1u << 19)
#define IRQ_TIMEOUT (1u << 21)
#define IRQ_CRCERR  (1u << 22)
#define IRQ_LENERR  (1u << 23)
#define IRQ_ERROR   (1u << 16)
#define IRQ_CMDERR  (1u << 17)

static int spi_fd = -1, busy_fd = -1;
static uint32_t spi_hz = 1000000;

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

/* --- GPIO through sysfs (the Luckfox SDK documents this interface) --- */
static int sysfs_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

static int gpio_setup(int gpio, const char *dir)
{
    char path[64], num[16];
    snprintf(num, sizeof num, "%d", gpio);
    snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/direction", gpio);
    if (access(path, F_OK) != 0) {
        sysfs_write("/sys/class/gpio/export", num);
        usleep(50000);
    }
    return sysfs_write(path, dir);
}

static int gpio_set(int gpio, int v)
{
    char path[64];
    snprintf(path, sizeof path, "/sys/class/gpio/gpio%d/value", gpio);
    return sysfs_write(path, v ? "1" : "0");
}

static int busy_read(void)
{
    char c = '0';
    if (pread(busy_fd, &c, 1, 0) != 1) return -1;
    return c == '1';
}

/* Returns 0 when the radio is ready, -1 on timeout. */
static int wait_ready(void)
{
    if (busy_fd < 0) {
        usleep(NO_BUSY_DELAY_US);
        return 0;
    }
    uint64_t end = now_ns() + (uint64_t)BUSY_TIMEOUT_US * 1000;
    int k = 0;
    do {
        if (busy_read() == 0) return 0;
        if (++k > 4) { struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL); }   /* au-delà de quelques lectures : céder le processeur */
    } while (now_ns() < end);
    return -1;
}

/* One SPI frame (one NSS assertion), then wait for BUSY to fall. */
static int frame(const uint8_t *tx, uint8_t *rx, size_t len)
{
    struct spi_ioc_transfer t;
    memset(&t, 0, sizeof t);
    t.tx_buf = (unsigned long)tx;
    t.rx_buf = (unsigned long)rx;
    t.len = (uint32_t)len;
    t.speed_hz = spi_hz;
    t.bits_per_word = 8;
    if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0) {
        perror("SPI transfer");
        return -1;
    }
    if (wait_ready() != 0) {
        fprintf(stderr, "BUSY still high after %d us\n", BUSY_TIMEOUT_US);
        return -1;
    }
    return 0;
}

static int cmd(uint16_t op, uint16_t *stat)
{
    uint8_t tx[2] = { (uint8_t)(op >> 8), (uint8_t)op }, rx[2];
    if (frame(tx, rx, 2)) return -1;
    if (stat) *stat = (uint16_t)(rx[0] << 8 | rx[1]);
    return 0;
}

/* Read command: request frame, then a second frame returns Stat and data. */
static int cmd_read(uint16_t op, uint8_t *out, size_t n, uint16_t *stat)
{
    uint8_t tx[34] = {0}, rx[34];
    if (n + 2 > sizeof tx || cmd(op, NULL)) return -1;
    if (frame(tx, rx, n + 2)) return -1;
    *stat = (uint16_t)(rx[0] << 8 | rx[1]);
    memcpy(out, rx + 2, n);
    return 0;
}


static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

/* Write-only command with parameters. */
static int wr(const uint8_t *b, size_t n)
{
    uint8_t rx[300];
    return frame(b, rx, n);
}
#define W(...) do { const uint8_t _b[] = { __VA_ARGS__ }; if (wr(_b, sizeof _b)) return -1; } while (0)

static int last_cmd_ok(const char *what)
{
    uint16_t s;
    if (cmd(0x0100, &s)) return -1;
    int c = (s >> 9) & 7;
    if (c == 0 || c == 1) {
        fprintf(stderr, "%s: refused by chip (Stat=0x%04X, %s)\n", what, s, c ? "CMD_PERR" : "CMD_FAIL");
        return -1;
    }
    return 0;
}
#define CHK(what, ...) do { W(__VA_ARGS__); if (last_cmd_ok(what)) return -1; } while (0)

static int irq_get_clear(uint32_t *irq)
{
    uint8_t d[4]; uint16_t s;
    if (cmd_read(0x0117, d, 4, &s)) return -1;
    *irq = (uint32_t)d[0] << 24 | d[1] << 16 | d[2] << 8 | d[3];
    return 0;
}

/* Datasheet Table 7-18 (2445 MHz reference design): target dBm -> {tx_power dBm*2, hf duty cycle}. */
static const struct { int8_t half_db; uint8_t duty; } pa_tab[11] = {
    {4,30},{6,30},{7,28},{8,25},{10,25},{15,31},{16,30},{18,30},{21,31},{22,30},{24,30} };

static int radio_setup(uint32_t freq, int rate, int cr, int pwr, int len)
{
    CHK("SetStandby XOSC", 0x01, 0x28, 0x01);
    CHK("Calibrate", 0x01, 0x22, 0x6F);
    CHK("SetPacketType FLRC", 0x02, 0x07, 0x05);
    CHK("SetRfFrequency", 0x02, 0x00, freq >> 24, freq >> 16, freq >> 8, freq);
    CHK("SetRxPath HF boost4", 0x02, 0x01, 0x01, 0x04);
    CHK("CalibFE", 0x01, 0x23);
    /* cr in high nibble, Gaussian BT 0.5 in low nibble */
    CHK("SetFlrcModulationParams", 0x02, 0x48, (uint8_t)rate, (uint8_t)(cr << 4 | 0x5));
    /* byte2: agc_pbl_len=7 (32 bits) <<2 | sync_len=2 (32 bits)
       byte3: sync_tx=1 <<6 | sync_match=1 <<3 | dynamic(0) <<2 | crc=1 (16 bit) */
    CHK("SetFlrcPacketParams", 0x02, 0x49, (7 << 2) | 2, (1 << 6) | (1 << 3) | 1, len >> 8, len);
    CHK("SetFlrcSyncword", 0x02, 0x4C, 0x01, 0x2D, 0xD4, 0x4A, 0x7E);
    /* 0..10 dBm: datasheet Table 7-18. Below 0 dBm: no table, duty 30 and setting = target + 2 dB (APPROXIMATE) */
    int8_t hd = pwr >= 0 ? pa_tab[pwr].half_db : (int8_t)(2 * (pwr + 2)); uint8_t du = pwr >= 0 ? pa_tab[pwr].duty : 30;
    CHK("SetPaConfig HF", 0x02, 0x02, 0x80, 0x67, du);
    CHK("SetTxParams", 0x02, 0x03, (uint8_t)hd, 0x02);
    CHK("SetRxTxFallbackMode XOSC", 0x02, 0x06, 0x02);
    CHK("ClearErrors", 0x01, 0x11);
    uint32_t irq; irq_get_clear(&irq);
    return 0;
}

static int do_tx(int count, int len, int interval_ms)
{
    uint8_t b[2 + 256];
    int ok = 0, fail = 0;
    double toa_sum = 0, toa_max = 0;
    for (int k = 0; k < count && !stop; k++) {
        W(0x01, 0x1F);                                   /* ClearTxFifo */
        b[0] = 0x00; b[1] = 0x02;                         /* WriteRadioTxFifo */
        b[2] = k >> 24; b[3] = k >> 16; b[4] = k >> 8; b[5] = k;
        for (int i = 4; i < len; i++) b[2 + i] = (uint8_t)(i * 7 + k);
        if (wr(b, 2 + len)) return -1;
        uint32_t irq; irq_get_clear(&irq);
        uint64_t t0 = now_ns();
        W(0x02, 0x0D, 0x00, 0x0C, 0x80);                 /* SetTx, timeout 3200/32768 s ~ 100 ms */
        for (;;) {
            if (irq_get_clear(&irq)) return -1;
            if (irq & (IRQ_TXDONE | IRQ_TIMEOUT | IRQ_CMDERR)) break;
            if (now_ns() - t0 > 500000000ull) { irq = 0; break; }
        }
        double us = (now_ns() - t0) / 1e3;
        if (irq & IRQ_TXDONE) { ok++; toa_sum += us; if (us > toa_max) toa_max = us; }
        else { fail++; fprintf(stderr, "pkt %d: no TxDone (irq=0x%08X)\n", k, irq); }
        if ((k + 1) % 100 == 0 || k + 1 == count)
            printf("tx %d/%d  ok %d  fail %d  TxDone avg %.0f us max %.0f us\n",
                   k + 1, count, ok, fail, ok ? toa_sum / ok : 0, toa_max);
        fflush(stdout);
        if (interval_ms) usleep(interval_ms * 1000);
    }
    return fail ? 1 : 0;
}

static int do_rx(int seconds)
{
    W(0x02, 0x0A);                                       /* ResetRxStats */
    W(0x02, 0x0C, 0xFF, 0xFF, 0xFF);                     /* SetRx continuous */
    uint64_t end = now_ns() + (uint64_t)seconds * 1000000000ull, t_first = 0, t_last = 0;
    long good = 0, crc = 0, lost = 0, bytes = 0; long prev = -1;
    while (!stop && (seconds == 0 || now_ns() < end)) {
        uint32_t irq;
        if (irq_get_clear(&irq)) return -1;
        if (!(irq & (IRQ_RXDONE | IRQ_CRCERR | IRQ_LENERR))) { usleep(200); continue; }
        if (irq & (IRQ_CRCERR | IRQ_LENERR)) {
            crc++; W(0x01, 0x1E);                         /* ClearRxFifo */
            continue;
        }
        uint8_t st[5]; uint16_t s;
        if (cmd_read(0x024B, st, 5, &s)) return -1;      /* GetFlrcPacketStatus */
        int n = st[0] << 8 | st[1];
        double rssi = -((st[2] << 1) | (st[4] & 1)) / 2.0;
        if (n > 256) n = 256;
        uint8_t tx[2 + 256] = {0x00, 0x01}, rx[2 + 256];
        struct spi_ioc_transfer t; memset(&t, 0, sizeof t);
        t.tx_buf = (unsigned long)tx; t.rx_buf = (unsigned long)rx; t.len = 2 + n;
        t.speed_hz = spi_hz; t.bits_per_word = 8;
        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 || wait_ready()) return -1;
        long seq = (long)rx[2] << 24 | rx[3] << 16 | rx[4] << 8 | rx[5];
        if (prev >= 0 && seq > prev + 1) lost += seq - prev - 1;
        prev = seq; good++; bytes += n;
        uint64_t now = now_ns(); if (!t_first) t_first = now; t_last = now;
        if (good % 100 == 1)
            printf("rx seq %ld len %d RSSI %.1f dBm | good %ld crc_err %ld lost %ld\n", seq, n, rssi, good, crc, lost);
        fflush(stdout);
    }
    double dt = (t_last - t_first) / 1e9;
    printf("RX end: good %ld crc_err %ld lost %ld  PER %.2f %%  useful rate %.1f kbit/s\n",
           good, crc, lost, good + crc + lost ? 100.0 * (crc + lost) / (good + crc + lost) : 0,
           dt > 0 ? bytes * 8 / dt / 1e3 : 0);
    W(0x01, 0x28, 0x01);
    return 0;
}


/* ===================== video over FLRC ===================== */
/* Air frame, always 255 bytes: [0xA5 seq_hi seq_lo len] + up to 251 bytes of H.265 Annex-B, zero padded.
   Fixed length keeps the RX FIFO aligned even if two packets are queued. */
#define VMAX 251
#define VSYNC 0xA5
static uint8_t vbuf[VMAX]; static int vlen; static uint16_t vseq;
static const char *vdump_path = "dump.h265";
static long v_pkts, v_bytes, v_fail; static int v_gap_us = 1500; static FILE *vdump;

static int send_air(const uint8_t *d, int n)
{
    uint8_t b[2 + 255];
    W(0x01, 0x1F);
    b[0] = 0x00; b[1] = 0x02; b[2] = VSYNC; b[3] = vseq >> 8; b[4] = (uint8_t)vseq; b[5] = (uint8_t)n;
    memcpy(b + 6, d, n); memset(b + 6 + n, 0, VMAX - n);
    if (wr(b, 2 + 255)) return -1;
    uint32_t irq; irq_get_clear(&irq);
    W(0x02, 0x0D, 0x00, 0x0C, 0x80);
    uint64_t t0 = now_ns();
    do { if (irq_get_clear(&irq)) return -1; }
    while (!(irq & (IRQ_TXDONE | IRQ_TIMEOUT | IRQ_CMDERR)) && now_ns() - t0 < 200000000ull);
    if (!(irq & IRQ_TXDONE)) v_fail++;
    vseq++; v_pkts++; v_bytes += n;
    usleep(v_gap_us);                                     /* let the receiver empty its 1-packet FIFO */
    return 0;
}
static int vput(const uint8_t *d, int n)
{
    if (vdump) fwrite(d, 1, n, vdump);
    while (n > 0) {
        int k = VMAX - vlen < n ? VMAX - vlen : n;
        memcpy(vbuf + vlen, d, k); vlen += k; d += k; n -= k;
        if (vlen == VMAX) { if (send_air(vbuf, vlen)) return -1; vlen = 0; }
    }
    return 0;
}
static int vflush(void) { int r = vlen ? send_air(vbuf, vlen) : 0; vlen = 0; return r; }

static const uint8_t SC[4] = {0, 0, 0, 1};
/* RTP H.265 (RFC 7798) -> Annex-B */
static int rtp_h265(const uint8_t *p, int n)
{
    if (n < 12 || (p[0] >> 6) != 2) return 0;
    int marker = p[1] & 0x80, h = 12 + 4 * (p[0] & 0x0F);
    if (p[0] & 0x10) { if (h + 4 > n) return 0; h += 4 + 4 * (p[h + 2] << 8 | p[h + 3]); }
    if (p[0] & 0x20) n -= p[n - 1];
    p += h; n -= h;
    if (n < 3) return 0;
    int type = (p[0] >> 1) & 0x3F;
    if (type == 48) {                                     /* aggregation packet */
        int i = 2;
        while (i + 2 <= n) {
            int sz = p[i] << 8 | p[i + 1]; i += 2;
            if (i + sz > n) break;
            if (vput(SC, 4) || vput(p + i, sz)) return -1;
            i += sz;
        }
    } else if (type == 49) {                              /* fragmentation unit */
        uint8_t fu = p[2];
        if (fu & 0x80) {
            uint8_t hdr[2] = { (uint8_t)((p[0] & 0x81) | ((fu & 0x3F) << 1)), p[1] };
            if (vput(SC, 4) || vput(hdr, 2)) return -1;
        }
        if (vput(p + 3, n - 3)) return -1;
    } else {
        if (vput(SC, 4) || vput(p, n)) return -1;
    }
    if (marker && vflush()) return -1;                    /* end of picture: send now for low latency */
    return 0;
}

static int rtsp_req(int fd, const char *req, char *resp, int max)
{
    if (write(fd, req, strlen(req)) < 0) return -1;
    int n = 0;
    while (n < max - 1) {
        int r = read(fd, resp + n, max - 1 - n);
        if (r <= 0) return -1;
        n += r; resp[n] = 0;
        char *e = strstr(resp, "\r\n\r\n");
        if (e) {
            char *cl = strcasestr(resp, "Content-Length:");
            int body = cl ? atoi(cl + 15) : 0;
            if (n >= (e - resp) + 4 + body) return n;
        }
    }
    return n;
}

static int do_vtx(const char *url_path, int seconds)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(554) };
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (void *)&a, sizeof a)) { perror("rtsp connect 127.0.0.1:554"); return 2; }
    struct timeval tv = { 3, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);   /* never hang if rkipc stalls */
    char rq[512], rs[4096], ctl[256] = "", sess[128] = "";
    snprintf(rq, sizeof rq, "DESCRIBE rtsp://127.0.0.1%s RTSP/1.0\r\nCSeq: 1\r\nAccept: application/sdp\r\n\r\n", url_path);
    if (rtsp_req(fd, rq, rs, sizeof rs) < 0 || !strstr(rs, " 200")) { fprintf(stderr, "DESCRIBE failed:\n%s\n", rs); return 1; }
    char *m = strstr(rs, "m=video"), *c = m ? strstr(m, "a=control:") : NULL;
    if (!c) { fprintf(stderr, "no video track in SDP\n"); return 1; }
    sscanf(c + 10, "%255[^\r\n]", ctl);
    if (!strstr(rs, "H265") && !strstr(rs, "h265")) fprintf(stderr, "warning: SDP is not H265\n");
    if (strncmp(ctl, "rtsp://", 7)) { char t[256]; snprintf(t, sizeof t, "rtsp://127.0.0.1%s/%s", url_path, ctl); strcpy(ctl, t); }
    snprintf(rq, sizeof rq, "SETUP %s RTSP/1.0\r\nCSeq: 2\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n", ctl);
    if (rtsp_req(fd, rq, rs, sizeof rs) < 0 || !strstr(rs, " 200")) { fprintf(stderr, "SETUP failed:\n%s\n", rs); return 1; }
    char *ss = strcasestr(rs, "Session:"); if (ss) sscanf(ss + 8, " %127[^;\r\n]", sess);
    snprintf(rq, sizeof rq, "PLAY rtsp://127.0.0.1%s RTSP/1.0\r\nCSeq: 3\r\nSession: %s\r\nRange: npt=0.000-\r\n\r\n", url_path, sess);
    if (write(fd, rq, strlen(rq)) < 0) return 1;
    fprintf(stderr, "RTSP %s playing, sending over FLRC\n", url_path);
    static uint8_t buf[65536 + 4]; int have = 0;
    uint64_t t0 = now_ns(), tl = t0;
    while (!stop && (seconds == 0 || now_ns() - t0 < (uint64_t)seconds * 1000000000ull)) {
        int r = read(fd, buf + have, sizeof buf - have);
        if (r <= 0) { fprintf(stderr, "RTSP stream stopped (no data for 3 s)\n"); break; }
        have += r;
        int i = 0;
        for (;;) {
            if (have - i < 1) break;
            if (buf[i] != '$') {                          /* RTSP text (PLAY reply): skip to end of headers */
                char *e = memmem(buf + i, have - i, "\r\n\r\n", 4);
                if (!e) break;
                i = (uint8_t *)e - buf + 4; continue;
            }
            if (have - i < 4) break;
            int ch = buf[i + 1], len = buf[i + 2] << 8 | buf[i + 3];
            if (have - i < 4 + len) break;
            if (ch == 0 && rtp_h265(buf + i + 4, len)) return 1;
            i += 4 + len;
        }
        memmove(buf, buf + i, have - i); have -= i;
        uint64_t now = now_ns();
        if (now - tl > 2000000000ull) {
            double dt = (now - t0) / 1e9;
            fprintf(stderr, "vtx %.0fs  air pkts %ld  video %.0f kbit/s  tx_fail %ld\n", dt, v_pkts, v_bytes * 8 / dt / 1e3, v_fail);
            fflush(stdout); tl = now;
        }
    }
    snprintf(rq, sizeof rq, "TEARDOWN rtsp://127.0.0.1%s RTSP/1.0\r\nCSeq: 4\r\nSession: %s\r\n\r\n", url_path, sess);
    if (write(fd, rq, strlen(rq)) < 0) { /* ignore */ }
    close(fd);
    return v_fail ? 1 : 0;
}

static int do_vrx(const char *host, int port, int seconds)
{
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(port) };
    int to_stdout = !strcmp(host, "-");
    if (!to_stdout && inet_pton(AF_INET, host, &dst.sin_addr) != 1) { fprintf(stderr, "bad host %s\n", host); return 2; }
    W(0x02, 0x49, (7 << 2) | 2, (1 << 6) | (1 << 3) | 1, 0, 255);   /* max length in dynamic mode */
    W(0x02, 0x0A);
    W(0x02, 0x0C, 0xFF, 0xFF, 0xFF);
    fprintf(stderr, "receiving video -> %s:%d\n", to_stdout ? "stdout" : host, port);
    uint64_t t0 = now_ns(), tl = t0; long good = 0, bad = 0, lost = 0, bytes = 0, resync = 0; int prev = -1;
    double rssi = 0;
    while (!stop && (seconds == 0 || now_ns() - t0 < (uint64_t)seconds * 1000000000ull)) {
        uint32_t irq;
        if (irq_get_clear(&irq)) return 1;
        if (irq & (IRQ_CRCERR | IRQ_LENERR)) { bad++; W(0x01, 0x1E); }
        else if (irq & IRQ_RXDONE) {
            /* read the packet first: the 256-byte FIFO holds only one packet */
            uint8_t tx[2 + 255] = {0x00, 0x01}, rx[2 + 255];
            struct spi_ioc_transfer t; memset(&t, 0, sizeof t);
            t.tx_buf = (unsigned long)tx; t.rx_buf = (unsigned long)rx; t.len = 2 + 255;
            t.speed_hz = spi_hz; t.bits_per_word = 8;
            if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 || wait_ready()) return 1;
            if (rx[2] != VSYNC || rx[5] > VMAX) {               /* FIFO misaligned (overflow): flush and resync */
                resync++; W(0x01, 0x1E); prev = -1; continue;
            }
            int seq = rx[3] << 8 | rx[4];
            if (prev >= 0) { uint16_t g = (uint16_t)(seq - prev - 1); if (g < 1000) lost += g; }
            prev = seq; good++;
            int d = rx[5];
            if (to_stdout) { fwrite(rx + 6, 1, d, stdout); fflush(stdout); } else sendto(us, rx + 6, d, 0, (void *)&dst, sizeof dst);
            bytes += d;
            if ((good & 63) == 1) {
                uint8_t st[5]; uint16_t s;
                if (cmd_read(0x024B, st, 5, &s) == 0) rssi = -((st[2] << 1) | (st[4] & 1)) / 2.0;
            }
        }
        uint64_t now = now_ns();
        if (now - tl > 2000000000ull) {
            double dt = (now - t0) / 1e9;
            fprintf(stderr, "vrx %.0fs  good %ld  crc_err %ld  lost %ld  resync %ld  RSSI %.1f dBm  video %.0f kbit/s\n",
                   dt, good, bad, lost, resync, rssi, bytes * 8 / dt / 1e3);
            tl = now;
        }
    }
    W(0x01, 0x28, 0x01);
    return 0;
}

/* ===================== IP link over FLRC (half duplex, token passing) =====================
 * Each side: veth pair "rf0" (has the IP address) <-> peer (raw AF_PACKET socket owned here).
 * Air frame 255 B: [0xA5][seq_hi][seq_lo][len][flags] + up to 250 B Ethernet frame.
 * flags bit0 = TOKEN: sender has finished its burst, the other side may transmit.
 * The master (ground) starts with the token and takes it back after a silence timeout. */
#define NMAX 250
#define NQ 64
static uint8_t nq[NQ][NMAX + 2]; static int nq_len[NQ], nq_h, nq_t;
static long n_tx, n_rx, n_bad, n_drop, n_tok_to, n_rx_bytes, n_tx_bytes;

static int net_tx_frame(const uint8_t *d, int n, int tok)
{
    uint8_t b[2 + 255];
    W(0x01, 0x1F);
    b[0] = 0x00; b[1] = 0x02; b[2] = VSYNC; b[3] = vseq >> 8; b[4] = (uint8_t)vseq; b[5] = (uint8_t)n; b[6] = tok ? 1 : 0;
    if (n) memcpy(b + 7, d, n);
    memset(b + 7 + n, 0, NMAX - n);
    if (wr(b, 2 + 255)) return -1;
    uint32_t irq; irq_get_clear(&irq);
    W(0x02, 0x0D, 0x00, 0x0C, 0x80);
    uint64_t t0 = now_ns();
    do { if (irq_get_clear(&irq)) return -1; }
    while (!(irq & (IRQ_TXDONE | IRQ_TIMEOUT | IRQ_CMDERR)) && now_ns() - t0 < 200000000ull);
    if (!(irq & IRQ_TXDONE)) v_fail++;
    vseq++; n_tx++; n_tx_bytes += n;
    usleep(v_gap_us);
    return 0;
}


/* veth leaves TCP/UDP checksums "partial" (offload): finish them before the frame leaves the host */
static void fix_l4_csum(uint8_t *f, int n)
{
    if (n < 34 || f[12] != 0x08 || f[13] != 0x00) return;
    uint8_t *ip = f + 14; int ihl = (ip[0] & 15) * 4, tot = ip[2] << 8 | ip[3];
    if (ihl < 20 || 14 + tot > n || (ip[6] & 0x3F) || ip[7]) return;  /* skip fragments */
    int proto = ip[9], len = tot - ihl, off;
    if (proto == 6) off = 16; else if (proto == 17) off = 6; else return;
    uint8_t *l4 = ip + ihl; if (len < off + 2) return;
    l4[off] = l4[off + 1] = 0;
    uint32_t sum = 0;
    for (int i = 12; i < 20; i += 2) sum += ip[i] << 8 | ip[i + 1];
    sum += proto + len;
    for (int i = 0; i + 1 < len; i += 2) sum += l4[i] << 8 | l4[i + 1];
    if (len & 1) sum += l4[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    uint16_t c = ~sum; if (proto == 17 && c == 0) c = 0xFFFF;
    l4[off] = c >> 8; l4[off + 1] = (uint8_t)c;
}


/* TCP MSS clamping: frame = 14 + IP(20..) + TCP(20) + payload must fit NMAX, so MSS <= NMAX - 14 - 20 - 20 - 12 (timestamps) */
#define MSS_MAX (NMAX - 14 - 40 - 12)
static int clamp_mss(uint8_t *f, int n)
{
    if (n < 54 || f[12] != 0x08 || f[13] != 0x00) return 0;
    uint8_t *ip = f + 14; int ihl = (ip[0] & 15) * 4;
    if (ip[9] != 6) return 0;
    uint8_t *t = ip + ihl; int doff = (t[12] >> 4) * 4;
    if (!(t[13] & 0x02) || 14 + ihl + doff > n) return 0;          /* SYN only */
    for (int i = 20; i + 3 < doff;) {
        int k = t[i];
        if (k == 0) break;
        if (k == 1) { i++; continue; }
        if (k == 2 && t[i + 1] == 4) {
            int m = t[i + 2] << 8 | t[i + 3];
            if (m > MSS_MAX) { t[i + 2] = MSS_MAX >> 8; t[i + 3] = MSS_MAX & 0xFF; return 1; }
            return 0;
        }
        if (t[i + 1] < 2) break;
        i += t[i + 1];
    }
    return 0;
}

static int do_net(const char *peer, int master, int burst, int tok_ms, int bridge)
{
    /* bridge=1 (ground on eth0): forward to radio only broadcast frames and frames for the learned remote MAC */
    uint8_t rmac[6] = {0}; int rmac_ok = 0;
    int ps = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (ps < 0) { perror("AF_PACKET"); return 2; }
    struct sockaddr_ll sl; memset(&sl, 0, sizeof sl);
    sl.sll_family = AF_PACKET; sl.sll_protocol = htons(ETH_P_ALL); sl.sll_ifindex = if_nametoindex(peer);
    if (!sl.sll_ifindex || bind(ps, (void *)&sl, sizeof sl)) { perror(peer); return 2; }
    if (bridge) {
        struct packet_mreq mr; memset(&mr, 0, sizeof mr);
        mr.mr_ifindex = sl.sll_ifindex; mr.mr_type = PACKET_MR_PROMISC;
        setsockopt(ps, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof mr);
        /* GRO would merge incoming TCP segments into frames larger than one radio packet: turn it off */
        struct ethtool_value ev = { ETHTOOL_SGRO, 0 }; struct ifreq ifr; memset(&ifr, 0, sizeof ifr);
        strncpy(ifr.ifr_name, peer, IFNAMSIZ - 1); ifr.ifr_data = (void *)&ev;
        if (ioctl(ps, SIOCETHTOOL, &ifr)) perror("disable GRO"); else fprintf(stderr, "GRO off on %s\n", peer);
    }
    W(0x02, 0x49, (7 << 2) | 2, (1 << 6) | (1 << 3) | 1, 0, 255);
    fprintf(stderr, "net %s on %s, burst %d, token timeout %d ms\n", master ? "MASTER" : "slave", peer, burst, tok_ms);
    int have_tok = master, rx_on = 0;
    uint64_t t_last_rx = now_ns(), t0 = now_ns(), tl = t0;
    while (!stop) {
        /* 1. collect outgoing Ethernet frames from the kernel */
        struct pollfd pf = { ps, POLLIN, 0 };
        while (poll(&pf, 1, 0) > 0 && (pf.revents & POLLIN)) {
            uint8_t f[2048]; struct sockaddr_ll from; socklen_t fl = sizeof from;
            int n = recvfrom(ps, f, sizeof f, 0, (void *)&from, &fl);
            if (n <= 0) break;
            if (n < 14) continue;
            if (bridge) {
                int bc = f[0] == 0xFF && f[1] == 0xFF && f[2] == 0xFF && f[3] == 0xFF && f[4] == 0xFF && f[5] == 0xFF;
                int to_r = rmac_ok && !memcmp(f, rmac, 6), from_r = rmac_ok && !memcmp(f + 6, rmac, 6);
                if (from_r || !(bc || to_r)) continue;               /* not for the remote side, or our own injection */
            } else if (from.sll_pkttype == PACKET_OUTGOING) continue;  /* our own injected frames */
            if (getenv("NETDBG")) fprintf(stderr, "in %d B type %02x%02x pkttype %d\n", n, f[12], f[13], from.sll_pkttype);
            if (n > NMAX || (nq_h + 1) % NQ == nq_t) { n_drop++; continue; }
            if (clamp_mss(f, n) || !bridge) fix_l4_csum(f, n);
            memcpy(nq[nq_h], f, n); nq_len[nq_h] = n; nq_h = (nq_h + 1) % NQ;
        }
        /* 2. our turn: send a burst, the last frame carries the token */
        if (have_tok) {
            if (rx_on) { W(0x01, 0x28, 0x01); rx_on = 0; }
            int k = 0;
            while (nq_t != nq_h && k < burst) {
                int last = ((nq_t + 1) % NQ == nq_h) || k == burst - 1;
                if (net_tx_frame(nq[nq_t], nq_len[nq_t], last)) return 1;
                nq_t = (nq_t + 1) % NQ; k++;
                if (last) break;
            }
            if (k == 0 && net_tx_frame(NULL, 0, 1)) return 1;           /* empty token frame */
            have_tok = 0; t_last_rx = now_ns();
        }
        /* 3. listen */
        if (!rx_on) { W(0x02, 0x0C, 0xFF, 0xFF, 0xFF); rx_on = 1; }
        uint32_t irq;
        if (irq_get_clear(&irq)) return 1;
        if (irq & (IRQ_CRCERR | IRQ_LENERR)) { n_bad++; W(0x01, 0x1E); }
        else if (irq & IRQ_RXDONE) {
            uint8_t tx[2 + 255] = {0x00, 0x01}, rx[2 + 255];
            struct spi_ioc_transfer t; memset(&t, 0, sizeof t);
            t.tx_buf = (unsigned long)tx; t.rx_buf = (unsigned long)rx; t.len = 2 + 255;
            t.speed_hz = spi_hz; t.bits_per_word = 8;
            if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &t) < 0 || wait_ready()) return 1;
            t_last_rx = now_ns();
            if (rx[2] != VSYNC || rx[5] > NMAX) { n_bad++; W(0x01, 0x1E); }
            else {
                n_rx++;
                if (rx[5] >= 14 && bridge && !rmac_ok) { memcpy(rmac, rx + 7 + 6, 6); rmac_ok = 1;
                    fprintf(stderr, "remote MAC %02x:%02x:%02x:%02x:%02x:%02x\n", rmac[0], rmac[1], rmac[2], rmac[3], rmac[4], rmac[5]); }
                if (rx[5]) { send(ps, rx + 7, rx[5], 0); n_rx_bytes += rx[5]; }
                if (rx[6] & 1) have_tok = 1;
            }
        }
        /* 4. master takes the token back if the slave stays silent */
        if (master && !have_tok && now_ns() - t_last_rx > (uint64_t)tok_ms * 1000000ull) { have_tok = 1; n_tok_to++; }
        if (!have_tok && !(irq & IRQ_RXDONE)) usleep(100);
        uint64_t now = now_ns();
        if (now - tl > 5000000000ull) {
            double dt = (now - t0) / 1e9;
            fprintf(stderr, "net %.0fs  tx %ld (%.0f kbit/s)  rx %ld (%.0f kbit/s)  bad %ld  qdrop %ld  token_timeouts %ld  tx_fail %ld\n",
                    dt, n_tx, n_tx_bytes * 8 / dt / 1e3, n_rx, n_rx_bytes * 8 / dt / 1e3, n_bad, n_drop, n_tok_to, v_fail);
            tl = now;
        }
    }
    W(0x01, 0x28, 0x01);
    return 0;
}

/* tcp test server (no radio): "tsrv": per connection, first byte 'S' = server sends N bytes, 'R' = server receives until close */
static int do_tsrv(int port, int nbytes)
{
    int l = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(l, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (bind(l, (void *)&a, sizeof a) || listen(l, 2)) { perror("tsrv"); return 2; }
    fprintf(stderr, "tsrv on port %d\n", port);
    static uint8_t buf[4096];
    while (!stop) {
        int c = accept(l, NULL, NULL); if (c < 0) continue;
        char m = 0; if (read(c, &m, 1) != 1) { close(c); continue; }
        uint64_t t0 = now_ns(); long tot = 0;
        if (m == 'S') { for (int i = 0; i < (int)sizeof buf; i++) buf[i] = (uint8_t)(i * 31);
            while (tot < nbytes) { int k = nbytes - tot < (int)sizeof buf ? nbytes - tot : (int)sizeof buf; int w = write(c, buf, k); if (w <= 0) break; tot += w; } }
        else { int r; while ((r = read(c, buf, sizeof buf)) > 0) tot += r; }
        double dt = (now_ns() - t0) / 1e9;
        fprintf(stderr, "tsrv %c %ld B in %.2fs = %.1f kbit/s\n", m, tot, dt, tot * 8 / dt / 1e3);
        close(c);
    }
    return 0;
}

/* band scan: RSSI per MHz (RX only, nothing transmitted) */

int main(int argc, char **argv)
{
    const char *dev = "/dev/spidev0.0";
    int busy = -1, rst = -1, count = 1000, len = 255, interval = 0, pwr = 0, rate = 2, cr = 1, secs = 30;
    uint32_t freq = 2440000000u;
    static const char *modes[] = {"tx", "rx", "vtx", "vrx", "btx", "net", "tsrv"};
    int known = 0;
    for (unsigned i = 0; argc >= 2 && i < sizeof modes / sizeof *modes; i++) if (!strcmp(argv[1], modes[i])) known = 1;
    if (!known) goto usage;
    int is_tx = !strcmp(argv[1], "tx");
    const char *vpath = "/live/1", *host = "-", *peer = "veth0"; int port = 5000, bridge = 0, master = 0, burst = 16, tok_ms = 60;
    optind = 2;
    for (int o; (o = getopt(argc, argv, "d:s:b:r:f:R:c:n:l:i:P:t:u:H:p:D:g:I:MLB:T:")) != -1;) switch (o) {
        case 'd': dev = optarg; break;
        case 's': spi_hz = strtoul(optarg, 0, 0); break;
        case 'b': busy = atoi(optarg); break;
        case 'r': rst = atoi(optarg); break;
        case 'f': freq = strtoul(optarg, 0, 0); break;
        case 'R': rate = atoi(optarg); break;
        case 'c': cr = atoi(optarg); break;
        case 'n': count = atoi(optarg); break;
        case 'l': len = atoi(optarg); break;
        case 'i': interval = atoi(optarg); break;
        case 'P': pwr = atoi(optarg); break;
        case 't': secs = atoi(optarg); break;
        case 'u': vpath = optarg; break;
        case 'H': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'I': peer = optarg; break;
        case 'M': master = 1; break;
        case 'L': bridge = 1; break;
        case 'B': burst = atoi(optarg); break;
        case 'T': tok_ms = atoi(optarg); break;
        case 'g': v_gap_us = atoi(optarg); break;
        case 'D': vdump_path = optarg; vdump = fopen(optarg, "wb"); break;
        default: goto usage;
    }
    if (!strcmp(argv[1], "tsrv")) { signal(SIGINT, on_sig); return do_tsrv(port, count * 1000); }
    if (rate < 0 || rate > 7 || cr < 0 || cr > 3 || len < 6 || len > 255 || pwr < -19 || pwr > 10 ||
        spi_hz > 16000000 || freq < 2400000000u || freq > 2483500000u) {
        fprintf(stderr, "bad parameter (rate 0-7, cr 0-3, len 6-255, P -19..10 dBm, f 2400-2483.5 MHz)\n");
        return 2;
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    spi_fd = open(dev, O_RDWR);
    if (spi_fd < 0) { perror(dev); return 2; }
    uint8_t mode = SPI_MODE_0; ioctl(spi_fd, SPI_IOC_WR_MODE, &mode);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_hz);
    if (busy >= 0) {
        char p[64]; gpio_setup(busy, "in");
        snprintf(p, sizeof p, "/sys/class/gpio/gpio%d/value", busy);
        busy_fd = open(p, O_RDONLY);
    }
    if (rst >= 0) {
        gpio_setup(rst, "out"); gpio_set(rst, 0); usleep(2000); gpio_set(rst, 1); usleep(20000);
        if (wait_ready()) { fprintf(stderr, "BUSY stuck after reset\n"); return 1; }
    }
    static const char *rates[] = {"2600","2080","1300","1040","650","520","325","260"};
    static const char *crs[] = {"1/2","3/4","1","2/3"};
    fprintf(stderr, "FLRC %s  %.3f MHz  %s kbit/s raw  CR %s  len %d  PA %d dBm (conducted)\n",
           argv[1], freq / 1e6, rates[rate], crs[cr], len, pwr);
    if (radio_setup(freq, rate, cr, pwr, len)) { fprintf(stderr, "radio setup FAILED\n"); return 1; }
    fprintf(stderr, "radio setup OK\n");
    if (!strcmp(argv[1], "net")) return do_net(peer, master, burst, tok_ms, bridge);
    if (!strcmp(argv[1], "vtx")) return do_vtx(vpath, secs);
    if (!strcmp(argv[1], "btx")) {                       /* bulk test with the video framing */
        uint8_t d[VMAX]; uint64_t t0 = now_ns();
        for (int k = 0; (count == 0 || k < count) && !stop; k++) { for (int i = 0; i < VMAX; i++) d[i] = (uint8_t)(k + i); if (send_air(d, VMAX)) return 1; }
        double dt = (now_ns() - t0) / 1e9;
        printf("btx R=%d gap=%dus  %ld pkts in %.2fs  offered %.0f kbit/s  tx_fail %ld\n", rate, v_gap_us, v_pkts, dt, v_bytes * 8 / dt / 1e3, v_fail);
        return 0;
    }
    if (!strcmp(argv[1], "vrx")) return do_vrx(host, port, secs);
    return is_tx ? do_tx(count, len, interval) : do_rx(secs);
usage:
    fprintf(stderr, "usage: %s tx|rx|btx|vtx|vrx|net|tsrv [-d /dev/spidev0.0] [-s spi_hz] [-b busy_gpio] [-r reset_gpio]\n"
                    "         [-f Hz] [-R rate 0-7] [-c cr 0-3] [-P dBm -19..10]\n"
                    "  tx:  [-n count] [-l len 6-255] [-i interval_ms]      rx: [-t seconds, 0 = forever]\n"
                    "  btx: [-n count] [-g gap_us]                          (bulk test with the video framing)\n"
                    "  vtx: [-u /live/1] [-t s] [-g gap_us] [-D dump.h265]  vrx: [-H host_ip | -] [-p udp_port] [-t s]\n"
                    "  net: -I veth_peer|eth0 [-L bridge] [-M master] [-B burst] [-T token_timeout_ms] [-g gap_us]\n"
                    "  tsrv: [-p port] [-n kbytes]                          (TCP test server, no radio)\n", argv[0]);
    return 2;
}
