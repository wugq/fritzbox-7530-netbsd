#!/bin/bash
# Step 3: write OpenWrt to flash (run after OpenWrt initramfs answers at 192.168.1.1)
# Procedure from OpenWrt commit 95b0c07a618f "ipq40xx: add support for FritzBox 7530"
set -e
cd "$(dirname "$0")"
ls -d backup/*/ >/dev/null 2>&1 || { echo "No backup found - run ./backup.sh first"; exit 1; }
IMG=openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin
scp -O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  firmware/$IMG firmware/uboot-fritz7530.bin root@192.168.1.1:/tmp/
echo ">>> Uploaded. Log in and run these in order:"
echo "    ssh -o UserKnownHostsFile=/dev/null root@192.168.1.1"
echo "    mtd write /tmp/uboot-fritz7530.bin uboot0"
echo "    mtd write /tmp/uboot-fritz7530.bin uboot1"
echo "    ubirmvol /dev/ubi0 --name=avm_filesys_0"
echo "    ubirmvol /dev/ubi0 --name=avm_filesys_1"
echo "    sysupgrade -n /tmp/$IMG"
