#!/usr/bin/env python3
#
# Build the flash edition's root file system image from NetBSD's official
# binary sets, on any host (Linux included, no root needed): the same job
# as flash/mkroot.sh, which copies from a running NetBSD.  Used by the
# GitHub Actions workflow and by tools/mkflash.sh SETS=1.
#
#   mkroot-sets.py --sets DIR --tooldir DIR --out IMAGE [--authorized-keys FILE]
#
# DIR holds base.tgz and etc.tgz (evbarm-earmv7hf).  It takes the files in
# flash/files.list from the sets, adds the shared libraries the ELF files
# need (readelf: NEEDED + RPATH, recursively, instead of ldd), the device
# nodes (the sets' MAKEDEV -s, as NetBSD's own build does), the users
# (pwd_mkdb) and flash/root/, and writes an mtree spec with owners and
# modes so makefs(8) builds the FFS image without root privileges.
#
# No ssh host keys go into the image: it is public, and everyone would
# share them.  /etc/rc creates them at the first boot; saveconfig keeps
# them.
#
import argparse
import os
import shutil
import subprocess
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))

ap = argparse.ArgumentParser()
ap.add_argument('--sets', required=True)
ap.add_argument('--tooldir', required=True)
ap.add_argument('--out', required=True)
ap.add_argument('--authorized-keys')
ap.add_argument('--stage', default=None, help='staging directory (default: OUT.d)')
a = ap.parse_args()

TD = os.path.join(a.tooldir, 'bin')
READELF = os.path.join(TD, 'armv7--netbsdelf-eabihf-readelf')
MAKEFS = os.path.join(TD, 'nbmakefs')
PWD_MKDB = os.path.join(TD, 'nbpwd_mkdb')
STAGE = a.stage or a.out + '.d'

# --- index the sets -----------------------------------------------------
# Unpack once (reading members of a .tgz one by one decompresses it from
# the start every time); keep the TarInfo for owners and modes.
UNPACK = os.path.join(a.sets, 'unpacked')
members = {}			# "/bin/sh" -> (None, TarInfo)
for s in ('base', 'etc'):
	with tarfile.open(os.path.join(a.sets, s + '.tgz')) as t:
		mark = os.path.join(UNPACK, '.' + s)
		if not os.path.exists(mark):
			t.extractall(UNPACK, filter=lambda m, path: m if not (
			    m.ischr() or m.isblk() or m.isfifo()) else None)
			open(mark, 'w').close()
		for m in t.getmembers():
			p = '/' + m.name.lstrip('./')
			if p != '/':
				members[p] = (None, m)

def norm(p):
	return os.path.normpath(p)

children = {}			# directory -> its entries
for q in members:
	children.setdefault(os.path.dirname(q), []).append(q)

chosen = {}			# path -> TarInfo (directories, files, links)

def choose_parents(p):
	"""the directories above p, as empty directory entries"""
	d = os.path.dirname(p)
	while d != '/' and d not in chosen and d in members:
		chosen[d] = members[d]
		d = os.path.dirname(d)

def choose(p, why='list'):
	p = norm(p)
	if p in chosen and not chosen[p][1].isdir() or p == '/':
		return
	if p not in members:
		print(f'mkroot-sets: {p} not in the sets ({why}), skipped', file=sys.stderr)
		return
	t, m = members[p]
	chosen[p] = (t, m)
	choose_parents(p)
	if m.isdir() and why in ('list', 'directory'):
		for q in children.get(p, ()):
			choose(q, 'directory')
	elif m.issym():
		tgt = m.linkname if m.linkname.startswith('/') else \
		    os.path.join(os.path.dirname(p), m.linkname)
		choose(tgt, 'link target')
	elif m.islnk():
		choose('/' + m.linkname.lstrip('./'), 'hard link')

for line in open(os.path.join(HERE, 'files.list')):
	line = line.split('#', 1)[0].strip()
	if line:
		choose(line)

# --- shared libraries: NEEDED and RPATH, recursively ------------------
def elf_deps(data):
	"""(needed, rpaths) of an ELF file given as bytes"""
	tmp = os.path.join(STAGE + '.tmp')
	with open(tmp, 'wb') as f:
		f.write(data)
	out = subprocess.run([READELF, '-d', tmp], capture_output=True,
	    text=True).stdout
	os.unlink(tmp)
	needed, rpaths = [], []
	for l in out.splitlines():
		if '(NEEDED)' in l:
			needed.append(l.split('[', 1)[1].rstrip(']'))
		elif '(RPATH)' in l or '(RUNPATH)' in l:
			rpaths += l.split('[', 1)[1].rstrip(']').split(':')
	return needed, rpaths

def content(p):
	t, m = members[p]
	if m.islnk():
		return content('/' + m.linkname.lstrip('./'))
	if m.issym():
		tgt = m.linkname if m.linkname.startswith('/') else \
		    os.path.join(os.path.dirname(p), m.linkname)
		return content(norm(tgt))
	with open(os.path.join(UNPACK, p.lstrip('/')), 'rb') as f:
		return f.read()

done = set()
todo = [p for p, (t, m) in chosen.items() if m.isfile() or m.issym()]
while todo:
	p = todo.pop()
	if p in done:
		continue
	done.add(p)
	try:
		data = content(p)
	except Exception:
		continue
	if not data.startswith(b'\x7fELF'):
		continue
	needed, rpaths = elf_deps(data)
	for lib in needed:
		for d in rpaths + ['/usr/lib', '/lib']:
			cand = norm(d.replace('$ORIGIN', os.path.dirname(p)) + '/' + lib)
			if cand in members:
				before = set(chosen)
				choose(cand, 'library of ' + p)
				todo += [q for q in set(chosen) - before]
				break
		else:
			print(f'mkroot-sets: {lib} (for {p}) not found', file=sys.stderr)

# --- stage the files ------------------------------------------------------
shutil.rmtree(STAGE, ignore_errors=True)
os.makedirs(STAGE)
spec = ['/set uid=0 gid=0', '. type=dir mode=0755']
entries = {}			# path -> spec line (later ones win)

def stage_path(p):
	return os.path.join(STAGE, p.lstrip('/'))

for p in sorted(chosen):
	t, m = chosen[p]
	mode = f'mode={m.mode & 0o7777:04o}'
	own = f'uid={m.uid} gid={m.gid}'
	if m.isdir():
		os.makedirs(stage_path(p), exist_ok=True)
		entries[p] = f'.{p} type=dir {mode} {own}'
	elif m.issym():
		os.makedirs(os.path.dirname(stage_path(p)), exist_ok=True)
		os.symlink(m.linkname, stage_path(p))
		entries[p] = f'.{p} type=link link={m.linkname} {mode} {own}'
	elif m.isfile() or m.islnk():
		os.makedirs(os.path.dirname(stage_path(p)), exist_ok=True)
		with open(stage_path(p), 'wb') as f:
			f.write(content(p))
		entries[p] = f'.{p} type=file {mode} {own}'

# directories the system needs (as flash/mkroot.sh)
for d, mode in (('/bin', 0o755), ('/sbin', 0o755), ('/lib', 0o755), ('/libexec', 0o755),
    ('/etc', 0o755), ('/etc/ssh', 0o755), ('/dev', 0o755), ('/dev/pts', 0o755),
    ('/tmp', 0o1777), ('/kern', 0o755), ('/proc', 0o755), ('/mnt', 0o755),
    ('/root', 0o755), ('/root/.ssh', 0o700), ('/usr', 0o755), ('/var', 0o755),
    ('/var/run', 0o755), ('/var/log', 0o755), ('/var/db', 0o755),
    ('/var/chroot', 0o755), ('/var/chroot/sshd', 0o755),
    ('/var/chroot/rtadvd', 0o755), ('/var/chroot/tcpdump', 0o755),
    ('/var/empty', 0o755)):
	os.makedirs(stage_path(d), exist_ok=True)
	if d not in entries:
		entries[d] = f'.{d} type=dir mode={mode:04o} uid=0 gid=0'

# users and groups from etc.tgz, with the databases
for f in ('master.passwd', 'group', 'shells', 'nsswitch.conf', 'login.conf'):
	p = '/etc/' + f
	if p in members:
		t, m = members[p]
		with open(stage_path(p), 'wb') as o:
			o.write(content(p))
		entries[p] = f'.{p} type=file mode={m.mode & 0o7777:04o} uid=0 gid=0'
subprocess.run([PWD_MKDB, '-p', '-d', STAGE, stage_path('/etc/master.passwd')],
    check=True)
for f, mode in (('passwd', 0o644), ('pwd.db', 0o644), ('spwd.db', 0o600),
    ('master.passwd', 0o600)):
	entries['/etc/' + f] = f'./etc/{f} type=file mode={mode:04o} uid=0 gid=0'

if a.authorized_keys:
	shutil.copy(a.authorized_keys, stage_path('/root/.ssh/authorized_keys'))
	entries['/root/.ssh/authorized_keys'] = \
	    './root/.ssh/authorized_keys type=file mode=0600 uid=0 gid=0'

# our /etc etc. (flash/root/), owned by root
root = os.path.join(HERE, 'root')
for dp, dns, fns in os.walk(root):
	rel = '/' + os.path.relpath(dp, root).lstrip('.').lstrip('/')
	rel = '/' if rel == '/' else rel.rstrip('/')
	for d in dns:
		p = norm(rel + '/' + d)
		os.makedirs(stage_path(p), exist_ok=True)
		entries.setdefault(p, f'.{p} type=dir mode=0755 uid=0 gid=0')
	for fn in fns:
		p = norm(rel + '/' + fn)
		src = os.path.join(dp, fn)
		if os.path.exists(stage_path(p)) or os.path.islink(stage_path(p)):
			os.unlink(stage_path(p))
		shutil.copy(src, stage_path(p))
		mode = 0o755 if os.access(src, os.X_OK) else 0o644
		entries[p] = f'.{p} type=file mode={mode:04o} uid=0 gid=0'

# device nodes: MAKEDEV -s from the sets (it is a plain sh script)
mk = STAGE + '.makedev'
shutil.rmtree(mk, ignore_errors=True)
os.makedirs(mk)
for f in ('/dev/MAKEDEV', '/dev/MAKEDEV.local'):
	with open(os.path.join(mk, os.path.basename(f)), 'wb') as o:
		o.write(content(f))
devspec = subprocess.run(['sh', './MAKEDEV', '-s', 'all'], cwd=mk,
    capture_output=True, text=True, check=True).stdout
shutil.rmtree(mk)
for l in devspec.splitlines():
	if not l.startswith('./') or l.startswith('./ '):
		continue
	name = l.split()[0][1:]
	p = '/dev' + name
	if 'type=dir' in l:
		os.makedirs(stage_path(p), exist_ok=True)
	entries[p] = '.' + p + l[len(name) + 1:]

# --- the image ------------------------------------------------------------
specfile = a.out + '.spec'
with open(specfile, 'w') as f:
	f.write('\n'.join(spec + [entries[p] for p in sorted(entries)]) + '\n')
subprocess.run([MAKEFS, '-t', 'ffs', '-B', 'le', '-b', '10%', '-f', '10%',
    '-F', specfile, a.out, STAGE], check=True)
nfiles = sum(1 for l in entries.values() if 'type=file' in l)
print(f'{a.out}: {os.path.getsize(a.out)} bytes, {nfiles} files')
