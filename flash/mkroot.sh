#!/bin/sh
#
# Build the root file system image of the flash edition.  Runs ON NetBSD
# (the full NetBSD 11.1 on the USB stick, booted on the 7530 or any other
# earmv7hf NetBSD), as root, from this directory:
#
#	sh mkroot.sh /var/tmp/flashroot.fs
#
# It copies the programs in files.list from the running system, adds the
# shared libraries ldd(1) says they need, the device nodes, the users and
# root's ssh key from this system, new ssh host keys and our own /etc
# (root/), then makes an FFS image with makefs(8).  tools/mkflash.sh on
# the PC runs it over ssh.
#
set -e
OUT=${1:?usage: mkroot.sh image}
HERE=$(cd "$(dirname "$0")" && pwd)
T=/var/tmp/flashroot.tree	# on the USB stick: /tmp is a small tmpfs
export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/pkg/bin

rm -rf "$T"
mkdir -p "$T"
cd "$T"
mkdir -p bin sbin lib libexec etc/ssh dev tmp kern proc mnt root/.ssh \
    usr/bin usr/sbin usr/lib usr/libexec usr/share/misc usr/pkg/bin \
    var/run var/log var/db var/chroot/sshd var/chroot/rtadvd var/chroot/tcpdump var/empty
chmod 1777 tmp
chmod 700 root/.ssh

# Copy one path into the tree.  Symbolic links stay links and their target
# is copied once (libc is /lib/libc.so.12.224 with links from /lib and
# /usr/lib: copying through the links stored it twice).
copy() {
	local f=$1 t
	[ -e ".$f" ] || [ -L ".$f" ] && return 0
	mkdir -p ".$(dirname "$f")"
	if [ -d "$f" ] && [ ! -L "$f" ]; then
		mkdir -p ".$f"
		for c in "$f"/*; do copy "$c"; done
	elif [ -L "$f" ]; then
		t=$(readlink "$f")
		ln -s "$t" ".$f"
		case "$t" in
		/*)	copy "$t" ;;
		*)	copy "$(cd "$(dirname "$f")" && pwd)/$t" ;;
		esac
	else
		cp -p "$f" ".$f"
	fi
}

# 1. programs and data files from the list
grep -v '^#' "$HERE/files.list" | grep -v '^$' | while read -r f; do
	copy "$f"
done

# 2. the shared libraries of everything copied (ldd lists the indirect
#    ones too; this also covers modules loaded with dlopen, like PAM's)
find . -type f | sed 's|^\.||' | while read -r f; do
	ldd -f '%p\n' "$f" 2>/dev/null || true
done | sort -u | while read -r lib; do
	[ -n "$lib" ] && [ -e "$lib" ] || continue
	copy "$lib"
done

# 3. device nodes, users, keys
(cd /dev && pax -rw -pe . "$T/dev")
mkdir -p dev/pts
for f in master.passwd group shells nsswitch.conf login.conf; do
	[ -f /etc/$f ] && cp -p /etc/$f etc/
done
pwd_mkdb -d "$T" -p "$T/etc/master.passwd"
[ -f /root/.ssh/authorized_keys ] &&
	install -m 600 /root/.ssh/authorized_keys root/.ssh/authorized_keys
ssh-keygen -q -A -f "$T"	# new host keys for the image

# 4. our /etc (and /root/.profile).  They come from the PC by tar, owned
#    by the PC user: make them root's, or sshd's StrictModes refuses key
#    logins (it checks that /root belongs to root).
cp -Rp "$HERE/root/." "$T/"
(cd "$HERE/root" && find .) | while read -r f; do
	chown root:wheel "$T/$f"
done

# 5. the image: FFS, little endian, 10% free for logs and leases
cd /
makefs -t ffs -B le -b 10% -f 10% "$OUT" "$T"
echo "$OUT: $(ls -l "$OUT" | awk '{print $5}') bytes," \
    "$(find "$T" -type f | wc -l | tr -d ' ') files," \
    "$(du -sk "$T" | cut -f1) KB of data"
