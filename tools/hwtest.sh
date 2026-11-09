#!/bin/bash
#
# Whole-system test of a NetBSD kernel on the FRITZ!Box 7530 (hardware in
# the loop): boot it, check it, measure it, write a report.
#
#   PC (192.168.77.2) ── switch port 4 ─┐
#                                        7530 (qcomedma0 = 77.1 / 78.1)
#   M93p (192.168.78.2) ─ switch port 3 ─┘
#
# The router is driven over its serial console (sw.sh), the M93p over its
# own (m93p-sw.sh), the PC runs iperf3/nc itself.  Every step is logged in
# ops.log as usual.
#
# usage: tools/hwtest.sh [label]
# env:   ITB=netbsd-usb.itb   image to boot (default: root on the USB stick)
#        NOBOOT=1             test the kernel that is running (logged in)
#        QUICK=1              skip the throughput measurements
#        WANPORT=3            switch port of the M93p (the one made WAN)
#
# Report: private/test-results/<date>-<time>.md, compared with the previous
# report; exit status 1 if a check failed.
#
set -u
H=$(dirname "$(readlink -f "$0")"); R=$H/..
L=$R/serial.log SW=$H/sw.sh M93P=$H/m93p-sw.sh
ITB=${ITB:-netbsd-usb.itb} WANPORT=${WANPORT:-3}
RTR=192.168.77.1 RTR2=192.168.78.1 PEER=192.168.78.2
LABEL=${1:-}
OUTDIR=$R/private/test-results; mkdir -p "$OUTDIR"
STAMP=$(date +%Y-%m-%d-%H%M); OUT=$OUTDIR/$STAMP.md
PREV=$(ls "$OUTDIR"/20*.md 2>/dev/null | grep -v "$STAMP" | tail -1)
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
FAILS=0

log() { echo "=== $(date +%T) [hwtest] $*" >> "$R/ops.log"; echo ">> $*"; }
clean() { tr -cd '\11\12\40-\176'; }
sw() { "$SW" "$1" "${2:-4}" | clean; }		# router console
m93p() { "$M93P" "$1" "${2:-3}"; }		# M93p console
waitfor() { for i in $(seq "$2"); do tail -c 800 "$L" | grep -aqE "$1" && return 0; sleep 2; done; return 1; }
# result rows: "| check | result | ok/FAIL |"
ROWS=()
check() {	# check "name" "result" ok|fail
	local st=ok; [ "$3" = ok ] || { st="**FAIL**"; FAILS=$((FAILS + 1)); }
	ROWS+=("| $1 | $2 | $st |"); echo "   $1: $2 [$st]"
}
pingcount() { grep -oE "[0-9]+ (packets )?received" | grep -oE "^[0-9]+"; }

# --- 1. boot -----------------------------------------------------------
if [ "${NOBOOT:-0}" != 1 ]; then
	log "boot $ITB"
	ITB=$ITB BOOTWAIT=300 timeout 600 "$H/nb-cycle.sh" > "$TMP/boot.txt" 2>&1
	if ! grep -q "shell ready" "$TMP/boot.txt"; then	# one retry
		log "boot failed ($(tail -1 "$TMP/boot.txt")), retrying"
		ITB=$ITB BOOTWAIT=300 timeout 600 "$H/nb-cycle.sh" > "$TMP/boot.txt" 2>&1
	fi
	waitfor "login:|(^|[^a-z])panic:|db\{" 150
	if tail -c 2000 "$L" | grep -aqE "(^|[^a-z])panic:|db\{"; then
		check "boot" "panic" fail
	elif grep -q "shell ready" "$TMP/boot.txt"; then
		check "boot" "login prompt" ok
	else
		check "boot" "$(tail -1 "$TMP/boot.txt")" fail
	fi
fi
# log in if the console sits at the login prompt (both modes)
printf '\003\025\r' > /dev/ttyUSB0; sleep 2
if tail -c 300 "$L" | tr -d ' ' | grep -aq "login:$"; then
	sw root 8 >/dev/null
fi
KVER=$(sw "uname -v" 3 | grep -aoE "\([A-Z0-9_]+\) #[0-9]+: [^w]*" | head -1 | sed 's/ *$//')
COMMIT=$(git -C "$R" log -1 --format=%h)$(git -C "$R" diff --quiet || echo "+dirty")
log "testing kernel $KVER (repo $COMMIT)"

# --- 2. smoke ----------------------------------------------------------
BOOTLOG=$R/last-boot.log	# written by nb-cycle.sh for this boot
grep -aq "ESS calibration done" "$BOOTLOG" &&
	check "PSGMII calibration" "done" ok || check "PSGMII calibration" "missing/timeout" fail
grep -aqE "sd0 at scsibus" "$BOOTLOG" &&
	check "USB stick" "sd0" ok || check "USB stick" "no sd0" fail
n=$(grep -ac "cpufreqdt.: couldn't acquire clock" "$BOOTLOG")
ROWS+=("| cpufreqdt errors (known) | $n | - |")

# (sw.sh lines must stay under 250 characters: NetBSD's MAX_CANON)
sw "sysctl -w net.inet.ip.forwarding=1 net.inet.ip.redirect=0 >/dev/null" 3 >/dev/null
sw "sysctl -w hw.qcomess0.wan_port=0 hw.qcomess0.isolate=1 >/dev/null" 3 >/dev/null
sw "ifconfig vlan2 destroy 2>/dev/null; ifconfig qcomedma0 $RTR/24 up" 4 >/dev/null
sw "ifconfig qcomedma0 inet $RTR2/24 alias; arp -d -a >/dev/null 2>&1" 4 >/dev/null
sleep 3
ports=$(sw "sysctl -n hw.qcomess0.ports" 3 | grep -a "1:" | tail -1)
echo "$ports" | grep -q "3:1000FD" && echo "$ports" | grep -q "4:1000FD" &&
	check "switch ports" "$ports" ok || check "switch ports" "$ports" fail
a=$(ping -c 5 -i 0.2 -W 1 $RTR | pingcount)
check "ping PC -> router" "$a/5" "$([ "$a" = 5 ] && echo ok)"
b=$(sw "ping -c 5 -i 0.2 -w 3 $PEER | grep received" 6 | pingcount)
check "ping router -> M93p" "$b/5" "$([ "$b" = 5 ] && echo ok)"
ok=0; for i in 1 2 3 4 5; do
	sw "echo IN-\$(($i*11))" 2 | grep -aq "IN-$((i * 11))" && ok=$((ok + 1))
done
check "console input" "$ok/5" "$([ $ok = 5 ] && echo ok)"

# --- 3. functional -----------------------------------------------------
# 100 MB of random data PC -> router -> M93p and back, sha256 compared
head -c 100000000 /dev/urandom > "$TMP/rand.bin"
want=$(sha256sum "$TMP/rand.bin" | cut -c1-64)
m93p "rm -f /tmp/hwt.bin; nc -l 5590 > /tmp/hwt.bin &" 2 >/dev/null; sleep 1
timeout 60 nc -N $PEER 5590 < "$TMP/rand.bin"; sleep 2
got=$(m93p "sha256 -q /tmp/hwt.bin" 6 | grep -oE "^[0-9a-f]{64}" | head -1)
check "100 MB PC -> M93p" "${got:0:12}…" "$([ "$got" = "$want" ] && echo ok)"
m93p "nc -N -l 5591 < /tmp/hwt.bin &" 2 >/dev/null; sleep 1
timeout 60 nc -d $PEER 5591 > "$TMP/back.bin"
got=$(sha256sum "$TMP/back.bin" | cut -c1-64)
check "100 MB M93p -> PC" "${got:0:12}…" "$([ "$got" = "$want" ] && echo ok)"

# --- 4. throughput (iperf3 server runs on the M93p) ----------------------
iperf() {	# iperf "args" -> Mbit/s (receiver)
	timeout 30 iperf3 -c $PEER -t 8 $1 2>&1 | grep receiver | tail -1 |
	    grep -oE "[0-9.]+ Mbits/sec" | cut -d' ' -f1
}
CASES=("" "-R" "-P 4" "-R -P 4" "-P 8" "-R -P 8")
NAMES=("1 flow PC→M93p" "1 flow M93p→PC" "4 flows PC→M93p" "4 flows M93p→PC" "8 flows PC→M93p" "8 flows M93p→PC")
declare -A PERF
if [ "${QUICK:-0}" != 1 ]; then
	m93p "pgrep -q iperf3 || iperf3 -s -D" 2 >/dev/null
	log "throughput, default mode (routing between 77/24 and 78/24)"
	for i in "${!CASES[@]}"; do
		PERF[default,$i]=$(iperf "${CASES[$i]}"); echo "   default ${NAMES[$i]}: ${PERF[default,$i]}"
	done
fi

# --- 5. WAN port (802.1Q) ----------------------------------------------
sw "sysctl -w hw.qcomess0.wan_port=$WANPORT >/dev/null; arp -d -a >/dev/null 2>&1" 3 >/dev/null; sleep 2
c=$(sw "ping -c 3 -w 3 $PEER | grep received" 5 | pingcount)
check "WAN isolated from qcomedma0" "$c/3 replies" "$([ "$c" = 0 ] && echo ok)"
sw "ifconfig qcomedma0 inet $RTR2 -alias; ifconfig vlan2 create" 4 >/dev/null
sw "ifconfig vlan2 vlan 2 vlanif qcomedma0; ifconfig vlan2 $RTR2/24 up" 4 >/dev/null
# moving 78.1 between interfaces leaves a stale neighbour entry behind
# (not shown by arp -an, but it blocks traffic): flush after every move
sw "arp -d -a >/dev/null 2>&1" 3 >/dev/null; sleep 2
c=$(sw "ping -c 3 -w 3 $PEER | grep received" 5 | pingcount)
check "WAN via vlan2" "$c/3" "$([ "$c" = 3 ] && echo ok)"
c=$(sw "ping -c 3 -w 3 -D -s 1472 $PEER | grep received" 5 | pingcount)
check "vlan2 1500-byte frames" "$c/3" "$([ "$c" = 3 ] && echo ok)"
a=$(ping -c 3 -i 0.2 -W 1 $RTR | pingcount)
check "LAN still up" "$a/3" "$([ "$a" = 3 ] && echo ok)"
if [ "${QUICK:-0}" != 1 ]; then
	log "throughput, LAN <-> WAN (vlan2)"
	for i in "${!CASES[@]}"; do
		PERF[vlan,$i]=$(iperf "${CASES[$i]}"); echo "   vlan ${NAMES[$i]}: ${PERF[vlan,$i]}"
	done
fi
# back to the default set-up
sw "ifconfig vlan2 destroy; sysctl -w hw.qcomess0.wan_port=0 >/dev/null" 4 >/dev/null
sw "ifconfig qcomedma0 inet $RTR2/24 alias; arp -d -a >/dev/null 2>&1" 4 >/dev/null
sw "ping -c 1 -w 2 $PEER >/dev/null" 4 >/dev/null	# warm up ARP
b=$(sw "ping -c 3 -w 3 $PEER | grep received" 5 | pingcount)
check "back to default mode" "$b/3" "$([ "$b" = 3 ] && echo ok)"
c=$(sw "echo ALIVE-\$((6*7))" 3 | grep -ac "ALIVE-42")
check "console after load" "$([ "$c" -ge 1 ] && echo answers || echo silent)" "$([ "$c" -ge 1 ] && echo ok)"

# --- 6. report -----------------------------------------------------------
prevval() {	# prevval "row name" column -> number from the previous report
	[ -n "$PREV" ] || return
	# first number in the cell (a cell may also hold a ▲/▼ mark)
	grep -F "| $1 |" "$PREV" | head -1 |
	    awk -F'|' -v c="$2" '{ split($c, w, " "); gsub(/[^0-9.]/, "", w[1]); print w[1] }'
}
{
	echo "# Hardware test $STAMP${LABEL:+: $LABEL}"
	echo
	echo "- kernel: \`NetBSD 11.1 $KVER\`, repo \`$COMMIT\`, image \`$( [ "${NOBOOT:-0}" = 1 ] && echo "(running system)" || echo "$ITB")\`"
	echo "- previous report: $([ -n "$PREV" ] && basename "$PREV" || echo none)"
	echo "- result: $([ $FAILS = 0 ] && echo "all checks passed" || echo "**$FAILS check(s) failed**")"
	echo
	echo "## Checks"
	echo
	echo "| check | result | |"
	echo "|---|---|---|"
	printf '%s\n' "${ROWS[@]}"
	if [ "${QUICK:-0}" != 1 ]; then
		echo
		echo "## Throughput (iperf3, Mbit/s, routed through the 7530)"
		echo
		echo "Changes of more than 10% against the previous report are marked."
		echo
		echo "| test | default | prev | LAN↔WAN (vlan2) | prev |"
		echo "|---|---|---|---|---|"
		for i in "${!CASES[@]}"; do
			d=${PERF[default,$i]:-?} v=${PERF[vlan,$i]:-?}
			pd=$(prevval "${NAMES[$i]}" 3) pv=$(prevval "${NAMES[$i]}" 5)
			mark() { awk -v n="$1" -v p="$2" 'BEGIN { if (p+0 > 0 && n+0 > 0) {
				r = (n - p) / p; if (r < -0.1) printf " ▼%d%%", r*100; else if (r > 0.1) printf " ▲%d%%", r*100 } }'; }
			echo "| ${NAMES[$i]} | $d$(mark "$d" "$pd") | ${pd:--} | $v$(mark "$v" "$pv") | ${pv:--} |"
		done
	fi
} > "$OUT"
log "report $OUT ($FAILS failed)"
echo; cat "$OUT"
[ $FAILS = 0 ]
