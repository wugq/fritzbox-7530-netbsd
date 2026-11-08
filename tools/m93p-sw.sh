#!/bin/bash
# Send one line to the ThinkCentre M93p serial console (/dev/ttyUSB1, 115200)
# and print the new output; the line is also noted in ops.log.
H=$(dirname "$(readlink -f "$0")"); L=$H/../m93p-serial.log; O=$H/../ops.log
printf '=== %s [M93p serial] %s\n' "$(date +%T)" "$1" >> "$O"
n=$(stat -c %s "$L"); printf '%s\r' "$1" > /dev/ttyUSB1; sleep "${2:-2}"
tail -c +$((n + 1)) "$L" | tr -cd '\11\12\40-\176'
