#!/bin/bash
# Boot NetBSD from the USB stick (ITB=netbsd-usb.itb), log in, bring up
# qcomedma0 and measure ping + TCP throughput in both directions.
# NOBOOT=1: skip booting (NetBSD already at a root shell with qcomedma0 up).
H=$(dirname "$(readlink -f "$0")"); L=$H/../serial.log; SW=$H/sw.sh
R=192.168.77.1; MB=${MB:-200}
waitfor() { for i in $(seq $2); do tail -c 600 "$L" | grep -aqE "$1" && return 0; sleep 2; done; return 1; }
if [ "$NOBOOT" != 1 ]; then
	ITB=netbsd-usb.itb BOOTWAIT=300 "$H/nb-cycle.sh" >/dev/null 2>&1
	waitfor "login:|(^|[^a-z])panic:|db\{" 150 || { echo "!! no login"; exit 1; }
	tail -c 600 "$L" | grep -aqE "(^|[^a-z])panic:|db\{" && { echo "!! panic"; exit 1; }
	$SW root 3 >/dev/null
	$SW "ifconfig qcomedma0 $R/24 up" 3 >/dev/null
fi
tr -cd '\11\12\40-\176' < "$L" | grep -a "qcomedma0:" | tail -4
ping -c 30 -i 0.05 -W1 $R >/dev/null	# eat the stale-RFD losses
echo "== ping PC->7530"; ping -c 50 -i 0.02 -W1 -s 1472 $R | tail -2
echo "== TCP PC->7530 (${MB} MB)"
$SW "(time nc -l 5001 </dev/null | wc -c) > /tmp/rx.txt 2>&1 &" 2 >/dev/null
s=$(date +%s.%N); head -c ${MB}000000 /dev/zero | timeout 120 nc -N $R 5001; e=$(date +%s.%N)
python3 -c "print('PC side: %.1f MB/s' % ($MB / ($e - $s)))"; sleep 2
$SW "cat /tmp/rx.txt" 2 | tr -cd '\11\12\40-\176' | grep -aE "^ *[0-9]+$|real"
echo "== TCP 7530->PC (${MB} MB)"
O=$(mktemp); (timeout 120 nc -l 5002 | wc -c > $O &); sleep 1
$SW "dd if=/dev/zero bs=1m count=$MB 2>/tmp/dd.txt | nc -N 192.168.77.2 5002; tail -1 /tmp/dd.txt" $(( MB / 8 + 5 )) | tr -cd '\11\12\40-\176' | grep -a "bytes/sec"
sleep 1; echo "PC received $(cat $O) bytes"; rm -f $O
$SW "sysctl -w hw.qcomedma_dump=1" 3 | tr -cd '\11\12\40-\176' | grep -a "rx ok"; true
