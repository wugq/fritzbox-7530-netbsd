#!/bin/bash
# usage: sw.sh "<line>" [wait]  — send a line to the 7530 serial console
# (/dev/ttyUSB0), print the new output; the line is also noted in ops.log.
H=$(dirname "$(readlink -f "$0")"); L=$H/../serial.log; O=$H/../ops.log
# NetBSD's tty takes at most MAX_CANON (255) characters per line: the
# rest, the CR included, is dropped and the shell waits for a line end
# that never comes (the console looks dead until ^C).  Refuse such lines.
if [ ${#1} -gt 250 ] && [ "${RAW:-0}" != 1 ]; then
	echo "sw.sh: line too long (${#1} > 250 characters), split it" >&2
	exit 2
fi
[ -n "$1" ] && printf '=== %s [7530 serial] %s\n' "$(date +%T)" "$1" >> "$O"
T=${2:-4}; n=$(stat -c %s "$L")
# The UART driver polls RX 100 times a second and reads only after an idle
# ("stale") gap: a long burst overruns the FIFO.  Send 32 characters at a
# time with a short pause (RAW=1: send in one go, to test the driver).
if [ "${RAW:-0}" = 1 ]; then
	printf '%s\r' "$1" > /dev/ttyUSB0
else
	for ((i = 0; i < ${#1}; i += 32)); do
		printf '%s' "${1:i:32}" > /dev/ttyUSB0; sleep 0.02
	done
	printf '\r' > /dev/ttyUSB0
fi
sleep $T
tail -c +$((n+1)) "$L" | tr -d '\r'
