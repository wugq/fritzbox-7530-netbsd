# ubi-check.py -- check a UBI partition image the way Linux's attach would
# (io.c validate_ec_hdr/validate_vid_hdr, vtbl.c vtbl_check/process_lvol),
# written from those rules, not from ubi_lite.
#
#	python3 ubi-check.py image.bin badpeb [original.bin]
#
# image.bin: the "ubi" partition (mtd10.bin of a backup, 2 KB pages, 128 KB
# blocks); badpeb: the bad block's index in it (-1: none); original.bin:
# the image before a change, to check that the other volumes' data is
# unchanged and that erase counters went up by exactly one.
import sys, struct, zlib
PG, PEB = 2048, 128 * 1024
LAYOUT = 0x7fffefff
def ucrc(b):
    # UBI's CRC-32: initial value 0xffffffff, no final inversion, i.e. the
    # inverse of the usual CRC-32
    return zlib.crc32(b) ^ 0xffffffff
def ok(cond, msg):
    if not cond: print("FAIL:", msg); global fails; fails += 1
fails = 0
img = open(sys.argv[1], 'rb').read()
bad = int(sys.argv[2])
orig = open(sys.argv[3], 'rb').read() if len(sys.argv) > 3 else None
n = len(img) // PEB
pebs = {}
seqs = set(); ecs = {}
for p in range(n):
    if p == bad: continue
    b = img[p*PEB:(p+1)*PEB]
    ec = b[:64]
    if ec == b'\xff'*64: pebs[p] = None; continue
    ok(ec[:4] == b'UBI#', f"PEB {p} EC magic")
    ok(ucrc(ec[:60]) == struct.unpack('>I', ec[60:64])[0], f"PEB {p} EC crc")
    ecv, vo, do, iseq = struct.unpack('>QIII', ec[8:28])
    ecs[p] = ecv; seqs.add((vo, do, iseq))
    vid = b[vo:vo+64]
    if vid == b'\xff'*64: pebs[p] = None; continue
    ok(vid[:4] == b'UBI!', f"PEB {p} VID magic")
    ok(ucrc(vid[:60]) == struct.unpack('>I', vid[60:64])[0], f"PEB {p} VID crc")
    vtype, copy, compat = vid[5], vid[6], vid[7]
    vol, lnum = struct.unpack('>II', vid[8:16])
    dsize, used, dpad, dcrc = struct.unpack('>IIII', vid[20:36])
    sq = struct.unpack('>Q', vid[40:48])[0]
    if vol >= 0x7fffef00: ok(compat in (1,2,4,5), f"PEB {p} internal vol compat {compat}")
    else: ok(compat == 0, f"PEB {p} user vol compat {compat}")
    if vtype == 1:
        ok(used == 0, f"PEB {p} used_ebs")
        if copy == 0: ok(dsize == 0 and dcrc == 0, f"PEB {p} copy_flag 0 with data_size")
    pebs[p] = (vol, lnum, sq, copy, do)
ok(len(seqs) == 1, f"EC headers disagree on offsets/image_seq: {seqs}")
vo, do, iseq = next(iter(seqs))
leb = PEB - do
# newest copy of each LEB
lebs = {}
for p, v in pebs.items():
    if v is None: continue
    k = (v[0], v[1])
    if k not in lebs or v[2] > pebs[lebs[k]][2]: lebs[k] = p
dups = [k for k in lebs if sum(1 for v in pebs.values() if v and (v[0],v[1]) == k) > 1]
ok(not dups, f"LEBs with two copies (attach must pick one): {dups}")
slots = min(leb // 172, 128); vsize = -(-slots*172 // PG) * PG
copies = []
for l in (0, 1):
    ok((LAYOUT, l) in lebs, f"layout LEB {l} missing")
    p = lebs[(LAYOUT, l)]; copies.append(img[p*PEB+do : p*PEB+do+vsize])
ok(copies[0] == copies[1], "layout copies differ")
empty = bytes(168) + struct.pack('>I', ucrc(bytes(168)))
vols = {}
for i in range(slots):
    r = copies[0][i*172:(i+1)*172]
    ok(ucrc(r[:168]) == struct.unpack('>I', r[168:])[0], f"vtbl record {i} crc")
    res = struct.unpack('>I', r[:4])[0]
    if res == 0: ok(r == empty, f"vtbl record {i} empty but not all zero"); continue
    align, dpad = struct.unpack('>II', r[4:12])
    vtype, upd = r[12], r[13]
    nl = struct.unpack('>H', r[14:16])[0]
    name = r[16:144]
    ok(align >= 1 and dpad == leb % align, f"vtbl record {i} alignment/data_pad")
    ok(vtype in (1, 2) and upd in (0, 1), f"vtbl record {i} type/upd_marker")
    ok(0 < nl <= 127 and name[nl] == 0 and 0 not in name[:nl], f"vtbl record {i} name length")
    vols[i] = (r[16:16+nl].decode(errors='replace'), res)
names = [v[0] for v in vols.values()]
ok(len(names) == len(set(names)), f"two volumes with the same name: {names}")
good = sum(1 for p in range(n) if p != bad)
ok(sum(v[1] for v in vols.values()) + 2 + 1 + 20 <= good, "volumes reserve more PEBs than there are")
for (v, l) in lebs:
    if v != LAYOUT: ok(v in vols, f"PEB of volume {v} which the table does not have")
print("volumes:", vols, "| used PEBs", sum(1 for v in pebs.values() if v), "free", sum(1 for v in pebs.values() if v is None))
if orig:
    # the remaining volumes' data is unchanged; EC of rewritten PEBs went up
    on = len(orig) // PEB
    def data(buf, p): return buf[p*PEB+do:(p+1)*PEB]
    olebs = {}
    for p in range(on):
        if p == bad: continue
        b = orig[p*PEB:(p+1)*PEB]
        if b[:4] != b'UBI#': continue
        vid = b[vo:vo+64]
        if vid[:4] != b'UBI!': continue
        vol, lnum = struct.unpack('>II', vid[8:16]); sq = struct.unpack('>Q', vid[40:48])[0]
        if (vol, lnum) not in olebs or sq > olebs[(vol, lnum)][1]: olebs[(vol, lnum)] = (p, sq)
    for (v, l), (p, sq) in olebs.items():
        if v == LAYOUT or v not in vols: continue
        ok(lebs.get((v, l)) is not None and data(img, lebs[(v, l)]) == data(orig, p), f"vol {v} LEB {l} changed")
    oec = {p: struct.unpack('>Q', orig[p*PEB+8:p*PEB+16])[0] for p in range(on) if p != bad and orig[p*PEB:p*PEB+4] == b'UBI#'}
    changed = [p for p in ecs if p in oec and ecs[p] != oec[p]]
    ok(all(ecs[p] == oec[p] + 1 for p in changed), "an erase counter did not go up by exactly 1")
    print("PEBs erased:", len(changed))
print("FAILURES:", fails)
