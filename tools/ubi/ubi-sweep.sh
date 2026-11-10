#!/bin/sh
#
# ubi-sweep.sh -- cut the power at every moment of an operation: run
# ubi-harness with CUT=0, 1, ... up to the operation's number of flash
# operations, and check each result with ubi-check.py.
#
#	./ubi-sweep.sh in.bin badpeb op...	(ops as for ubi-harness)
#	STEP=n: every n-th moment only; START=n: from the n-th
#
# Prints, per moment, whether the next scan works, the checker's verdict
# and the volumes found; at the end, how many moments had a problem.
#
H=$(dirname "$0")
[ -x "$H/ubi-harness" ] || { echo "build ubi-harness first (see ubi-harness.c)" >&2; exit 1; }
in=$1 bad=$2; shift 2
out=${TMPDIR:-/tmp}/ubi-sweep.$$.bin
trap 'rm -f "$out"' EXIT
total=$(CUT=1000000000 "$H/ubi-harness" "$in" /dev/null "$bad" "$@" |
    sed -n 's/operations: //p')
echo "flash operations: $total"
n=${START:-0} fails=0
while [ "$n" -le "$total" ]; do
	res=$(CUT=$n "$H/ubi-harness" "$in" "$out" "$bad" "$@")
	scan=$(echo "$res" | sed -n 's/^rescan \(-*[0-9]*\):.*/\1/p')
	vols=$(echo "$res" | sed -n 's/^  vol \([0-9]*\) \([^ ]*\) .*/\1:\2/p' | tr '\n' ' ')
	chk=$(python3 -I "$H/ubi-check.py" "$out" "$bad" | tail -1)
	echo "cut $n: scan $scan, $chk, $vols"
	[ "$scan" = 0 ] && [ "$chk" = "FAILURES: 0" ] || fails=$((fails + 1))
	n=$((n + ${STEP:-1}))
done
echo "moments with a problem: $fails"
[ "$fails" -eq 0 ]
