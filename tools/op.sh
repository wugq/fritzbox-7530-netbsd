#!/bin/bash
# Run a command on the PC, logging command + output to ops.log (and stdout).
# usage: op.sh "what this does" command args...
H=$(dirname "$(readlink -f "$0")"); O=$H/../ops.log
d=$1; shift
printf '\n=== %s [PC] %s\n$ %s\n' "$(date +%T)" "$d" "$*" >> "$O"
"$@" 2>&1 | tee -a "$O"
rc=${PIPESTATUS[0]}
printf '(exit %d)\n' "$rc" >> "$O"
exit "$rc"
