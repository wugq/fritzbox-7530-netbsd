#!/bin/sh
#
# Build the flash edition image: the FRITZ7530_FLASH kernel with the
# memory-disk root (flash/) inside, gzipped, + the board DTB, as one FIT
# image U-Boot can boot from TFTP or from a UBI volume:
#
#	tftp/netbsd-flash.itb
#
# The root image goes into the kernel's md_root_image array with
# mdsetimage(8) and the kernel is gzipped (U-Boot unpacks it): 14 MB
# instead of 40 MB, so U-Boot's default "ubi read" of the whole volume
# stays clear of U-Boot itself (docs/09-how-the-flash-edition-works.md).
#
# The root image is made either from NetBSD's official binary sets
# (SETS=1, on any host: flash/mkroot-sets.py; AUTHORIZED_KEYS=file adds
# root's ssh key) or ON the router: NetBSD from the USB stick running and
# reachable as root@192.168.77.1 with the lab ssh key
# (~/.ssh/fritz7530-lab), flash/mkroot.sh.  The FRITZ7530_FLASH kernel must be built
# (KERNEL=FRITZ7530_FLASH tools/build.sh).  REUSE_ROOT=1: keep dl/flashroot.fs
# (no router needed).
#
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
cd "$R"
HOST=${HOST:-root@192.168.77.1}
SSH="ssh -i $HOME/.ssh/fritz7530-lab -o IdentitiesOnly=yes -o BatchMode=yes
    -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
TD=$R/tooldir/bin
SETS_PATH=pub/NetBSD/NetBSD-11.1/evbarm-earmv7hf/binary/sets
SETS_URL=${SETS_URL:-https://cdn.netbsd.org/$SETS_PATH}
SETS_MASTER=https://ftp.netbsd.org/$SETS_PATH
ELF=obj/sys/arch/evbarm/compile/FRITZ7530_FLASH/netbsd
[ -f "$ELF" ] || { echo "no $ELF: KERNEL=FRITZ7530_FLASH tools/build.sh first" >&2; exit 1; }

if [ "${REUSE_ROOT:-0}" = 1 ] && [ -f dl/flashroot.fs ]; then
	echo ">>> reusing dl/flashroot.fs (REUSE_ROOT=1)"
elif [ "${SETS:-0}" = 1 ]; then
	# from NetBSD's official binary sets: no router needed (the CI way)
	S=dl/sets
	mkdir -p $S
	# The CDN can stall (503 "first byte timeout", or a few KB/s until
	# it drops the transfer): retry, give up on a transfer slower than
	# 50 KB/s for a minute, then try the master site.  A file is kept
	# only when complete, so a failed run does not leave half of one.
	for f in base.tgz etc.tgz SHA512; do
		[ -f $S/$f ] && continue
		for u in "$SETS_URL" "$SETS_MASTER"; do
			curl -sfL --retry 3 --retry-all-errors --speed-limit 50000 \
			    --speed-time 60 -o $S/$f.part "$u/$f" &&
			    mv $S/$f.part $S/$f && break
			echo "$u/$f: download failed" >&2
		done
		[ -f $S/$f ] || { rm -f "dl/sets/$f.part"; exit 1; }
	done
	for f in base.tgz etc.tgz; do
		want=$(grep "($f)" $S/SHA512 | awk '{print $4}')
		[ "$(sha512sum $S/$f | cut -d' ' -f1)" = "$want" ] ||
		    { echo "$f: SHA512 mismatch" >&2; exit 1; }
	done
	echo ">>> building the root image from the NetBSD 11.1 sets"
	python3 flash/mkroot-sets.py --sets $S --tooldir tooldir \
	    --out dl/flashroot.fs ${AUTHORIZED_KEYS:+--authorized-keys "$AUTHORIZED_KEYS"}
else
	echo ">>> building the root image on $HOST"
	tar cf - flash | $SSH "$HOST" 'rm -rf /var/tmp/flash && tar xf - -C /var/tmp'
	$SSH "$HOST" 'sh /var/tmp/flash/mkroot.sh /var/tmp/flashroot.fs'
	$SSH "$HOST" 'cat /var/tmp/flashroot.fs' > dl/flashroot.fs
fi
ls -l dl/flashroot.fs

echo ">>> root image into the kernel, gzip, FIT image"
cp "$ELF" dl/netbsd-flash.elf
"$TD"/armv7--netbsdelf-eabihf-mdsetimage -s dl/netbsd-flash.elf dl/flashroot.fs
"$TD"/armv7--netbsdelf-eabihf-objcopy -O binary dl/netbsd-flash.elf dl/netbsd-flash.bin
gzip -9nf dl/netbsd-flash.bin		# -> dl/netbsd-flash.bin.gz
python3 -I tools/mkfit.py dl/netbsd-flash.bin.gz dl/fritzbox-7530.dtb - \
    tftp/netbsd-flash.itb
ls -l dl/netbsd-flash.bin.gz tftp/netbsd-flash.itb
