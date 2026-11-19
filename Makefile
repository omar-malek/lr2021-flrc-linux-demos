# Native build (syntax check on a PC):   make
# Luckfox build (RV1103 SDK toolchain):  make CC=arm-rockchip830-linux-uclibcgnueabihf-gcc
CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
lr2021_flrc: src/lr2021_flrc.c
	$(CC) $(CFLAGS) -o $@ $<
clean:
	rm -f lr2021_flrc
