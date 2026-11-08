#!/bin/sh
#
# Build a NetBSD/evbarm kernel for the FRITZ!Box 7530 and the FIT images
# U-Boot loads over TFTP:
#
#   tftp/netbsd.itb      kernel + DTB + NetBSD's install ramdisk (dl/ramdisk.fs)
#   tftp/netbsd-usb.itb  kernel + DTB with root=sd0a (userland on a USB stick)
#
# KERNEL=GENERIC (default) or KERNEL=FRITZ7530 (the small board kernel,
# sys/arch/evbarm/conf/FRITZ7530); other kernels get their name in the
# image names: tftp/netbsd-FRITZ7530.itb, tftp/netbsd-usb-FRITZ7530.itb.
#
# Steps:
#   1. src/ = NetBSD netbsd-11 (shallow clone on first run)
#   2. overlay: every file under sys/ is symlinked into src/sys/ (new files
#      only; they are listed in src/.git/info/exclude so src stays "clean")
#   3. patches/*.diff are applied to src/ unless already applied
#   4. toolchain in tooldir/ (built once), kernel in obj/
#   5. DTBs with dtc, FIT images with tools/mkfit.py
#
# Needs: git, dtc, python3; about 5 GB of disk.
set -e
R=$(cd "$(dirname "$0")/.." && pwd)
SRC=$R/src OBJ=$R/obj TOOLDIR=$R/tooldir
KERNEL=${KERNEL:-GENERIC}
cd "$R"

# 1. NetBSD source
# NETBSD_COMMIT=<sha>: that commit instead of the branch head (the CI
# pins one, so its builds are reproducible)
if [ ! -d "$SRC/.git" ]; then
	if [ -n "$NETBSD_COMMIT" ]; then
		echo ">>> fetching NetBSD $NETBSD_COMMIT (shallow) into src/"
		git init -q "$SRC"
		git -C "$SRC" remote add origin https://github.com/NetBSD/src.git
		git -C "$SRC" fetch -q --depth 1 origin "$NETBSD_COMMIT"
		git -C "$SRC" checkout -q FETCH_HEAD
	else
		echo ">>> fetching NetBSD netbsd-11 (shallow) into src/"
		git clone --depth 1 --branch netbsd-11 https://github.com/NetBSD/src.git "$SRC"
	fi
fi

# 2. overlay
EXCLUDE=$SRC/.git/info/exclude
(cd sys && find . -type f) | sed 's|^\./||' | while read -r f; do
	dst=$SRC/sys/$f
	if [ -e "$dst" ] && [ ! -L "$dst" ]; then
		echo "!! sys/$f would replace a stock NetBSD file; make it a patch" >&2
		exit 1
	fi
	mkdir -p "$(dirname "$dst")"
	ln -sfn "$R/sys/$f" "$dst"
	grep -qxF "sys/$f" "$EXCLUDE" 2>/dev/null || echo "sys/$f" >> "$EXCLUDE"
done

# 3. patches
for p in "$R"/patches/*.diff; do
	[ -e "$p" ] || continue
	if git -C "$SRC" apply -R --check "$p" 2>/dev/null; then
		continue			# already applied
	fi
	echo ">>> applying $(basename "$p")"
	git -C "$SRC" apply "$p"
done

# 4. toolchain + kernel
NB="./build.sh -U -u -j$(nproc) -m evbarm -a earmv7hf -O $OBJ -T $TOOLDIR"
if [ ! -x "$TOOLDIR/bin/nbmake" ]; then
	echo ">>> building the toolchain (once, ~15 min)"
	(cd "$SRC" && $NB tools > "$R/build-tools.log" 2>&1) ||
	    { tail -20 "$R/build-tools.log"; exit 1; }
fi
echo ">>> building kernel $KERNEL"
(cd "$SRC" && $NB kernel="$KERNEL" > "$R/build-kernel.log" 2>&1) ||
    { grep -E "error:|\*\*\*" "$R/build-kernel.log" | head -20; exit 1; }
K=$OBJ/sys/arch/evbarm/compile/$KERNEL/netbsd
"$TOOLDIR"/bin/armv7--netbsdelf-eabihf-objcopy -O binary "$K" "$OBJ/netbsd-$KERNEL.bin"

# 5. device trees and FIT images
mkdir -p dl tftp
# The board .dts includes NetBSD's qcom-ipq4019.dtsi and uses the
# dt-bindings macros (GIC_SPI, GCC_ESS_CLK, ...): run cpp first, as the
# NetBSD and Linux builds do.
DTS=$SRC/sys/external/gpl2/dts/dist
dtb() {
	cpp -nostdinc -undef -P -x assembler-with-cpp -D__DTS__ \
	    -I sys/arch/arm/dts -I "$DTS/arch/arm/boot/dts" -I "$DTS/include" \
	    "sys/arch/arm/dts/$1.dts" | dtc -q -I dts -O dtb -o "dl/$2.dtb" -
}
dtb qcom-ipq4019-fritzbox-7530 fritzbox-7530
dtb qcom-ipq4019-fritzbox-7530-usbroot fritzbox-7530-usbroot
SUF=; [ "$KERNEL" = GENERIC ] || SUF=-$KERNEL
if [ -f dl/ramdisk.fs ]; then
	python3 -I tools/mkfit.py "$OBJ/netbsd-$KERNEL.bin" dl/fritzbox-7530.dtb \
	    dl/ramdisk.fs tftp/netbsd$SUF.itb
else
	echo "(no dl/ramdisk.fs: skipping tftp/netbsd.itb, see docs/build-and-boot.md)"
fi
python3 -I tools/mkfit.py "$OBJ/netbsd-$KERNEL.bin" dl/fritzbox-7530-usbroot.dtb \
    - tftp/netbsd-usb$SUF.itb
ls -l "$OBJ/netbsd-$KERNEL.bin" tftp/netbsd*$SUF.itb
