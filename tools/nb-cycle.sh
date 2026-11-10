#!/bin/bash
# Get the 7530 to U-Boot (reboot / watchdog / OpenWrt), arm the HW watchdog,
# TFTP netbsd.itb, boot, and leave NetBSD at a '#' shell.
# env: BOOTWAIT=secs, NOWDT=1, ITB=netbsd-usb.itb (root on USB)
#      UBOOT_ONLY=1: stop at the U-Boot prompt (for looking around there)
#      USBPRE=1: USB VBUS + PHY reset pokes from U-Boot (only needed by kernels
#      older than the qcom_tlmm/qcom_usbphy drivers)
H=$(dirname "$(readlink -f "$0")"); L=$H/../serial.log; SW=$H/sw.sh
size(){ stat -c %s "$L"; }
since(){ tail -c +$(( $1 + 1 )) "$L"; }

get_uboot() {
	local n=$(size) t0=$SECONDS
	# Always type "root" first: at a NetBSD login prompt it logs in (else
	# "reboot" becomes the user name); at a shell it is a harmless
	# "not found".
	printf '\r\003\r' > /dev/ttyUSB0; sleep 1
	printf 'root\r' > /dev/ttyUSB0; sleep 4
	printf '\rreboot\r' > /dev/ttyUSB0
	while (( SECONDS - t0 < 240 )); do
		printf ' ' > /dev/ttyUSB0; sleep 0.05
		local out; out=$(since $n | tail -c 4000 | tr -d "\000")
		# the prompt at the start of a line: the motd's "NetBSD 11.1
		# (FRITZ7530) #0: ..." (the USB kernel's name) must not match
		if tr -d '\r' <<<"$out" | grep -aq '^(FRITZ7530) #'; then return 0; fi
		if grep -aq "Eva_AVM >" <<<"$out"; then
			# AVM's bootloader caught our keypresses: let it go on to U-Boot
			n=$(size); printf '\rgo\r' > /dev/ttyUSB0; sleep 1
		fi
		if grep -aqE "Please press Enter to activate|root@OpenWrt" <<<"$out"; then
			n=$(size); printf '\rreboot\r' > /dev/ttyUSB0
		fi
		# NetBSD at its login prompt: "reboot" would be taken as a user
		# name (and our spaces as the password), so log in first
		if grep -aq "login: *$" <<<"$(tr -d ' ' <<<"$out" | tail -c 200)"; then
			n=$(size); printf '\025root\r' > /dev/ttyUSB0; sleep 3
			printf '\rreboot\r' > /dev/ttyUSB0
		fi
	done
	return 1
}

ITB=${ITB:-netbsd.itb}
get_uboot || { echo "!! could not reach U-Boot"; exit 1; }
[ "$UBOOT_ONLY" = 1 ] && { echo "== at the U-Boot prompt"; exit 0; }
$SW '' 2 >/dev/null
$SW 'setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2' 1 >/dev/null
if [ "$USBPRE" = 1 ]; then
	$SW 'mw.l 0x1031004 0x2; mw.l 0x1031000 0x4a01' 1 >/dev/null
	$SW 'mw.l 0x181e038 0x34; sleep 1; mw.l 0x181e038 0x30; sleep 1; mw.l 0x181e038 0x20; sleep 1; mw.l 0x181e038 0x0' 5 >/dev/null
fi
ok=0
# The PC's e1000e NIC sometimes drops into its low-power mode when the
# router resets the link (docs/06-building-and-testing.md): wake it up between tries.
for t in 1 2 3 4 5 6; do
	$SW "tftpboot 0x85000000 $ITB" 30 | grep -q "Bytes transferred" && { ok=1; break; }
	nmcli con up fritz-netbsd >/dev/null 2>&1; sleep 3
done
[ $ok = 1 ] || { echo "!! TFTP failed"; exit 1; }
# hardware watchdog: 60 s (32768 Hz), armed just before boot; NetBSD qcomwdt pets it
[ "$NOWDT" = 1 ] || $SW 'mw.l 0xb017008 0; mw.l 0xb017004 1; mw.l 0xb017010 0x1e0000; mw.l 0xb017014 0x1e0000; mw.l 0xb017008 1' 1 >/dev/null
n=$(size)
printf 'bootm 0x85000000\r' > /dev/ttyUSB0
t0=$SECONDS
while (( SECONDS - t0 < ${BOOTWAIT:-60} )); do
	sleep 2
	if since $n | grep -aqE "Installation messages in English|login:|^# $"; then break; fi
	if since $n | grep -aqE "(^|[^a-z])panic:|Stopped in pid|db\{[0-9]\}>"; then echo "!! kernel panic"; since $n | tr -d '\r' | grep -aE "panic|Stopped|trap|fault" | head; exit 2; fi
done
since $n | tr -d '\r' > "$H/../last-boot.log"
if since $n | grep -aq "Installation messages in English"; then
	$SW 'a' 3 >/dev/null; $SW 'x' 3 >/dev/null
elif ! since $n | grep -aqE "login:|# $"; then
	echo "!! no shell/login prompt"; tail -5 "$H/../last-boot.log"; exit 3
fi
echo "== NetBSD shell ready (boot log: last-boot.log)"
