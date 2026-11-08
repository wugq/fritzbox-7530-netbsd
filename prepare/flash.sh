#!/bin/bash
# Steps 1-2: load U-Boot through EVA, then the OpenWrt initramfs over TFTP
# (it runs from RAM only; nothing is written to the flash)
# usage: sudo ./flash.sh [interface]    (Ctrl+C once OpenWrt is up)
set -e
cd "$(dirname "$0")"
IF=${1:-enp0s31f6}   # the PC's wired interface, e.g. sudo ./flash.sh enp0s20f0u3

# two addresses on the wired interface: 192.168.178.10 (to talk to EVA) and
# 192.168.1.70 (U-Boot's TFTP server)
nmcli con delete fritz-flash >/dev/null 2>&1 || true
nmcli con add type ethernet ifname $IF con-name fritz-flash \
  ipv4.method manual ipv4.addresses "192.168.178.10/24,192.168.1.70/24" \
  ipv6.method disabled connection.autoconnect-priority 100 >/dev/null
nmcli con up fritz-flash >/dev/null 2>&1 || true

dnsmasq --no-daemon --port=0 --enable-tftp --tftp-root="$PWD/tftp" \
  --interface=$IF --bind-dynamic --log-facility=- &
DNSMASQ=$!
trap 'kill $DNSMASQ 2>/dev/null' EXIT

echo
echo ">>> Power-cycle the FRITZ!Box now (cable in a LAN port). Waiting for EVA at 192.168.178.1 ..."
until ping -c1 -W1 192.168.178.1 >/dev/null 2>&1; do sleep 0.2; done
echo ">>> EVA found, uploading U-Boot"
until python3 -I scripts/eva_ramboot.py --offset 0x85000000 192.168.178.1 firmware/uboot-fritz7530.bin; do
  sleep 0.5
done
echo
echo ">>> U-Boot sent; it will fetch FRITZ7530.bin over TFTP (see the log below)."
echo ">>> Wait 1-2 minutes, then in another terminal: once 192.168.1.1 answers ping, run ./backup.sh"
wait $DNSMASQ
