#!/bin/bash
# Copyright (c) 2026 Sevana OÜ
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

# Local end-to-end smoke test for the AF_XDP sender + recvmmsg receiver over a
# veth pair in two network namespaces. No real NIC touched. Needs root (AF_XDP +
# netns). Run `make` at the repo root first. Usage: sudo ./scripts/veth-test.sh [flows] [seconds]
set -u
FLOWS=${1:-1000}
SECS=${2:-5}
NS_S=vqxs NS_R=vqxr
VS=veth-s VR=veth-r
SIP=10.99.0.1 RIP=10.99.0.2
PORT=40000
HERE=$(cd "$(dirname "$0")/.." && pwd)/bin

cleanup() {
    ip netns del $NS_S 2>/dev/null
    ip netns del $NS_R 2>/dev/null
}
trap cleanup EXIT

[ "$(id -u)" = 0 ] || { echo "run as root (sudo $0)"; exit 1; }
[ -x "$HERE/rtp_afxdp_sender" ] && [ -x "$HERE/recvmmsg_receiver" ] || { echo "run 'make' at the repo root first"; exit 1; }

cleanup
ip netns add $NS_S
ip netns add $NS_R
ip link add $VS netns $NS_S type veth peer name $VR netns $NS_R

ip netns exec $NS_S ip addr add $SIP/24 dev $VS
ip netns exec $NS_R ip addr add $RIP/24 dev $VR
ip netns exec $NS_S ip link set $VS up
ip netns exec $NS_R ip link set $VR up
ip netns exec $NS_S ip link set lo up
ip netns exec $NS_R ip link set lo up

SMAC=$(ip netns exec $NS_S cat /sys/class/net/$VS/address)
RMAC=$(ip netns exec $NS_R cat /sys/class/net/$VR/address)
# Static neighbour so nothing depends on ARP (sender uses --dst-mac directly anyway).
ip netns exec $NS_S ip neigh replace $RIP lladdr $RMAC dev $VS nud permanent
echo "veth: $VS($SMAC) <-> $VR($RMAC)   $SIP -> $RIP:$PORT   flows=$FLOWS dur=${SECS}s"
echo

RXLOG=$(mktemp)
ip netns exec $NS_R "$HERE/recvmmsg_receiver" --port $PORT --batch 256 --report-sec 1 >"$RXLOG" 2>&1 &
RXPID=$!
sleep 1

echo "=== sender ==="
ip netns exec $NS_S "$HERE/rtp_afxdp_sender" \
    --iface $VS --dst-ip $RIP --dst-mac $RMAC --src-ip $SIP --src-mac $SMAC \
    --flows $FLOWS --dst-port $PORT --duration-sec $SECS

sleep 1
kill -INT $RXPID 2>/dev/null
wait $RXPID 2>/dev/null
echo
echo "=== receiver ==="
cat "$RXLOG"
rm -f "$RXLOG"
