#!/bin/sh
#
# Log a serial console to a file, forever: when the USB serial adapter goes
# away (unplugged, or re-enumerated when the board loses power) wait for it
# and attach again.  A plain "cat /dev/ttyUSB0" keeps reading the dead
# device after a re-enumeration and silently logs nothing; so does a cat
# whose device node was re-created under it (same name, new inode), which
# is why the node's inode is watched too.
#
# usage: serial-log.sh /dev/ttyUSB0 serial.log        (the 7530)
#        serial-log.sh /dev/ttyUSB1 m93p-serial.log   (second test box)
#
DEV=$1 LOG=$2
OPS=$(dirname "$(readlink -f "$0")")/../ops.log
while :; do
	until [ -c "$DEV" ]; do sleep 1; done
	sleep 1
	stty -F "$DEV" 115200 raw -echo cs8 -cstopb -parenb 2>/dev/null
	ino=$(stat -c %i "$DEV" 2>/dev/null)
	echo "=== $(date +%T) [PC] serial logger attached to $DEV -> $LOG" >> "$OPS"
	# one process reads the device, so the watchdog can stop it
	stdbuf -o0 tr -d '\r' < "$DEV" >> "$LOG" 2>/dev/null &
	rd=$!
	# watchdog: device gone or re-created -> stop this reader
	while kill -0 $rd 2>/dev/null; do
		sleep 2
		[ "$(stat -c %i "$DEV" 2>/dev/null)" = "$ino" ] || kill $rd
	done
	sleep 1
done
