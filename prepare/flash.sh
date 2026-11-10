#!/bin/bash
# Load U-Boot into RAM through EVA and serve prepare/tftp/ over TFTP.
# Nothing is written to the flash.  U-Boot then either fetches
# FRITZ7530.bin by itself (the OpenWrt initramfs, docs/01-installing-uboot.md
# section 1.7) or is stopped on the serial console and loads NetBSD
# (section 1.5).
# usage: sudo ./flash.sh [interface]    (Ctrl+C when done)
set -e
cd "$(dirname "$0")"
IF=${1:-enp0s31f6}   # the PC's wired interface, e.g. sudo ./flash.sh enp0s20f0u3

# three addresses on the wired interface: 192.168.178.10 (to talk to EVA),
# 192.168.1.70 (U-Boot's default TFTP server) and 192.168.77.2 (the PC
# NetBSD's LAN expects)
nmcli con delete fritz-flash >/dev/null 2>&1 || true
nmcli con add type ethernet ifname $IF con-name fritz-flash \
  ipv4.method manual ipv4.addresses "192.168.178.10/24,192.168.1.70/24,192.168.77.2/24" \
  ipv6.method disabled connection.autoconnect-priority 100 >/dev/null
nmcli con up fritz-flash >/dev/null 2>&1 || true

mkdir -p tftp
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
echo ">>> U-Boot sent.  NetBSD: press a key on the serial console at"
echo ">>> \"Hit any key to stop autoboot\".  OpenWrt: it fetches FRITZ7530.bin by"
echo ">>> itself; once 192.168.1.1 answers ping, run ./backup.sh."
wait $DNSMASQ
