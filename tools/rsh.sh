#!/bin/bash
# Run a command on the 7530's NetBSD over ssh, logging the command, its
# output and exit status to ops.log (and the output to stdout).  Standard
# input is passed on, e.g.:
#	tools/rsh.sh 'sysupgrade -n -' < tftp/netbsd-flash.itb
# usage: rsh.sh "command"     env: R=root@192.168.77.1
H=$(dirname "$(readlink -f "$0")"); O=$H/../ops.log
R=${R:-root@192.168.77.1}
printf '\n=== %s [7530 NetBSD, ssh] %s\n' "$(date +%T)" "$1" >> "$O"
ssh -i ~/.ssh/fritz7530-lab -o ConnectTimeout=10 "$R" "$1" 2>&1 |
    tee >(sed 's/^/    | /' >> "$O")
rc=${PIPESTATUS[0]}
sleep 0.2	# let the log copy finish
printf '    (exit %d)\n' "$rc" >> "$O"
exit "$rc"
