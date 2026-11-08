#!/bin/bash
#
# TFTP server for U-Boot: the PC's wired port is 192.168.77.2, U-Boot uses
# 192.168.77.1.  Serves tftp/ (tools/build.sh puts the FIT images there).
#
# usage: sudo tools/tftp-serve.sh [interface]     (Ctrl+C to stop)
#
# The NetworkManager connection "fritz-netbsd" is created only if missing:
# it also carries the route to the second test machine's network
# (192.168.78.0/24 via 192.168.77.1), which deleting it would lose.
set -e
cd "$(dirname "$(readlink -f "$0")")/.."
IF=${1:-enp0s31f6}
if ! nmcli -t -f NAME connection show | grep -qx fritz-netbsd; then
	nmcli con add type ethernet ifname "$IF" con-name fritz-netbsd \
	    ipv4.method manual ipv4.addresses 192.168.77.2/24 \
	    ipv6.method disabled connection.autoconnect-priority 100 >/dev/null
fi
nmcli con up fritz-netbsd >/dev/null 2>&1 || true
ufw allow in on "$IF" from 192.168.77.0/24 >/dev/null
mkdir -p tftp
echo ">>> TFTP root: $PWD/tftp"; ls -l tftp
exec dnsmasq --no-daemon --port=0 --enable-tftp --tftp-root="$PWD/tftp" \
    --interface="$IF" --bind-dynamic --log-facility=-
