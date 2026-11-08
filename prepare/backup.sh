#!/bin/bash
# Back up the full flash of the FRITZ!Box 7530 while OpenWrt runs from RAM (initramfs)
# Run after flash.sh, once 192.168.1.1 answers, BEFORE install.sh / ubirmvol / sysupgrade.
set -e
cd "$(dirname "$0")"
D=backup/$(date +%Y%m%d-%H%M%S); mkdir -p "$D"
SSH="ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@192.168.1.1"
$SSH 'cat /proc/mtd; echo; ubinfo -a 2>/dev/null' > "$D/layout.txt"
cat "$D/layout.txt"
for m in $($SSH "grep -o '^mtd[0-9]*' /proc/mtd"); do
  echo ">>> $m"; $SSH "cat /dev/$m" > "$D/$m.bin"
done
for v in $($SSH "ls /dev/ | grep -E '^ubi[0-9]+_[0-9]+$'"); do
  echo ">>> $v"; $SSH "cat /dev/$v" > "$D/$v.bin"
done
( cd "$D" && sha256sum *.bin > sha256sums && ls -l )
echo ">>> Backup saved in $D"
