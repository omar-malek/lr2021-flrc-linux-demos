#!/bin/sh
# rf_net.sh <ip/24> [master] : create rf0 (IP side) and print the raw-socket peer name
IP=$1
ip link show rf0 >/dev/null 2>&1 || ip link add rf0 type veth peer name rf1
PEER=$(ip link show rf0 | head -1 | sed 's/^[0-9]*: rf0@\([^:]*\):.*/\1/')
ip link set rf0 mtu 236; ip link set $PEER mtu 236
ip addr flush dev rf0; ip addr add $IP dev rf0
ip link set $PEER up; ip link set rf0 up
echo $PEER
