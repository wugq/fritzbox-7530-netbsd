#!/bin/sh
#
# backup-netbsd.sh -- copy the whole NAND of a FRITZ!Box 7530 running the
# NetBSD flash edition (from the flash, or from RAM) into backup/<date>/.
#
# Same files as backup.sh makes from OpenWrt: one file per MTD partition
# (mtd0.bin ... mtd10.bin, ECC-corrected data as Linux's /dev/mtdN gives
# it), one per UBI volume (ubi0_N.bin), layout.txt and sha256sums.  The
# partition table is the 7530's (Linux names); NetBSD reads the chip
# through the qcomqpic driver's read-only minor 255.
#
#	./backup-netbsd.sh [user@router] [ssh options...]
#
# The backup may contain device keys and WiFi passwords: never commit or
# upload it.
#
set -eu
cd "$(dirname "$0")"
R=${1:-root@192.168.77.1}
[ $# -gt 0 ] && shift
out=backup/$(date +%Y%m%d-%H%M%S)
mkdir -p "$out"

# name, offset, size (bytes)
parts="SBL1 0x0 0x80000
MIBIB 0x80000 0x80000
QSEE 0x100000 0x80000
CDT 0x180000 0x40000
QSEE_B 0x1c0000 0x80000
urlader0 0x240000 0x40000
urlader1 0x280000 0x40000
nand-tffs 0x2c0000 0x840000
uboot0 0xb00000 0x400000
uboot1 0xf00000 0x400000
ubi 0x1300000 0x6d00000"

# this boot's messages (the buffer may still hold earlier boots' too)
ssh -n "$@" "$R" "awk '/^\\[ *[0-9.]+\\] NetBSD /{b=\"\"} {b=b \$0 \"\\n\"} END{printf \"%s\", b}' /var/run/dmesg.boot" > "$out/dmesg.txt"
# the device node: major from the boot messages, minor 255 = whole chip
major=$(sed -n 's/.*qcomqpic0: volumes: character major \([0-9]*\),.*/\1/p' "$out/dmesg.txt" | tail -1)
[ -n "$major" ] || { echo "no qcomqpic0 volumes line in the router's dmesg" >&2; exit 1; }
ssh -n "$@" "$R" "[ -c /dev/nandchip ] || mknod /dev/nandchip c $major 255"

{
	echo "dev:    size   erasesize  name"
	n=0
	echo "$parts" | while read -r name off size; do
		printf 'mtd%d: %08x 00020000 "%s"\n' $n $((size)) "$name"
		n=$((n + 1))
	done
} > "$out/layout.txt"

n=0
echo "$parts" | while read -r name off size; do
	printf '%-10s mtd%-2d ' "$name" $n
	# 128 KB blocks: offsets and sizes are all multiples of it
	ssh -n "$@" "$R" "dd if=/dev/nandchip bs=128k skip=$((off / 131072)) count=$((size / 131072)) 2>/dev/null" > "$out/mtd$n.bin"
	got=$(wc -c < "$out/mtd$n.bin")
	[ "$got" -eq $((size)) ] || { echo "short read: $got of $((size))" >&2; exit 1; }
	echo "ok"
	n=$((n + 1))
done

# UBI volumes, as the driver lists them (id, name, LEBs)
echo >> "$out/layout.txt"
grep 'qcomqpic0: UBI volume' "$out/dmesg.txt" |
    sed -n 's/.*UBI volume \([0-9]*\) "\([^"]*\)": \([0-9]*\) LEBs.*/\1 \2 \3/p' |
    while read -r id name lebs; do
	printf '%-10s ubi0_%-2d ' "$name" "$id"
	echo "ubi0 volume $id: $name, $lebs LEBs" >> "$out/layout.txt"
	ssh -n "$@" "$R" "[ -c /dev/ubivol$id ] || mknod /dev/ubivol$id c $major $id; dd if=/dev/ubivol$id bs=126976 2>/dev/null" > "$out/ubi0_$id.bin"
	echo "ok"
done

(cd "$out" && sha256sum *.bin > sha256sums)
echo "backup in prepare/$out"
echo "it may contain device keys and WiFi passwords: never commit or upload it"
