# Chapter 10. NAND and UBI

## 10.1. Synopsis

Goal: NetBSD reads the NAND and keeps its settings in a small UBI volume, so the flash
edition needs no USB stick. Neither NetBSD nor FreeBSD has a driver for the IPQ4019's
NAND controller (QPIC), and neither implements UBI.

The code is split so the hardware and format knowledge can move to FreeBSD later:

| | |
|---|---|
| `sys/arch/arm/qcom/qpic_nand.c` | the QPIC controller; OS-independent (registers through two callbacks, plus a delay) |
| `sys/dev/ubi/ubi_lite.c` | a small implementation of the UBI on-flash format; OS-independent |
| `sys/arch/arm/qcom/qcom_qpic.c` | NetBSD glue: attach, clocks, reset, the UBI volumes as character devices |

Linux's `qcom_nandc.c` and `ubi-media.h` were read for the hardware and the format; the
code is separate.

After reading this chapter, you will know:

- how the IPQ4019's NAND controller (QPIC) is driven without its DMA engine;
- how a page is laid out in codewords with ECC;
- how UBI's on-flash format is read, and how one volume is rewritten safely;
- how a volume is removed, and how that was tested without a Linux machine;
- how an update can survive a power failure at any moment, and how to test that by
  cutting the power on purpose, in a simulation.

Before reading this chapter, you should:

- have read the driver pattern ([Section 7.2](07-how-the-drivers-work.md#72-the-netbsd-driver-pattern));
- know how settings are kept ([Section 9.6](09-how-the-flash-edition-works.md#96-how-settings-are-kept)).

## 10.2. The Chip

`FETCH_ID` returns `c2 f1 80 95`: Macronix, 1 Gbit (128 MB), 8-bit bus (MX30LF1G18AC
class): 2048-byte pages with 64 spare bytes, 64 pages per 128 KB block.

## 10.3. The Controller without Its DMA Engine

QPIC normally works with a DMA engine, BAM: register writes and data move as BAM
descriptors. The boot loader leaves it in **BAM mode**, where (as a comment in Linux's
driver says) the CPU may only *read* the controller's "operational" registers. A probe
confirmed it: a `FETCH_ID` written by the CPU was ignored (status 0x402b), `NAND_CTRL` = 1.

The way out: **reset the block through the GCC** (`GCC_QPIC_BCR`). After the reset
`NAND_CTRL` is 0, register writes work, and `FETCH_ID` completes (status 0x20, ready). No
BAM driver is needed. The reset also clears the boot loader's page/ECC configuration
(`DEV0_CFG0`, `DEV0_CFG1`, `DEV0_ECC_CFG`, `EBI2_ECC_BUF_CFG`, the `DEV_CMD` registers):
the driver saves them before the reset and writes them back, since U-Boot's configuration
is known to read this chip correctly (`CFG0` = 0x2a5408c0: 4 codewords of 516 data bytes,
5 address cycles; it matches Linux's formulas, e.g. the bad block byte at
2048 - 528 x 3 + 1 = 465).

An operation is: command (`NAND_FLASH_CMD`), address (`NAND_ADDR0/1`), configuration,
`NAND_EXEC_CMD` = 1, poll `NAND_FLASH_STATUS` until ready. Page data passes through the
528-byte buffer at `FLASH_BUF_ACC`, one codeword at a time.

### Page layout

With 4-bit BCH ECC per 512-byte step, a 2 KB page is four **codewords**; the first three
hold 516 data bytes each, the last 500 data bytes and 16 free spare bytes
(516 x 3 + 500 = 2048). Each codeword's ECC bytes stay inside the controller.

- **Read**: for each codeword, "page read with ECC", wait, check `NAND_BUFFER_STATUS`
  (uncorrectable?), copy the data out of `FLASH_BUF_ACC`. A never-written page has no valid
  ECC: the controller reports an error, and `NAND_ERASED_CW_DETECT_STATUS` says the
  codeword is erased; it then reads as 0xff.
- **Program**: for each codeword, the data into `FLASH_BUF_ACC` (the last one followed by
  16 bytes of 0xff), "program page", wait, check status.
- **Erase**: "block erase" with the row (page) address in `NAND_ADDR0` and the raw
  configuration.
- **Bad block marker**: the factory marker (first spare byte of the block's first page)
  lands inside the last codeword in QPIC's layout; it is read raw (no ECC).

### Verified

- Pages 0, 1 and 0x40 (SBL1, which never changes) read identical to a flash backup made
  under Linux; page 0x2600 (the first UBI block, at 0x1300000) starts with `UBI#`.
- Throughput: 14.6 MB in 5 s (about 2.9 MB/s) without DMA.

## 10.4. UBI

UBI divides the `ubi` partition (0x1300000, 0x6d00000 bytes, from the device tree's
`fixed-partitions`) into physical eraseblocks (PEBs). Each starts with an **erase counter
header** (`UBI#`, how often it was erased) and, when in use, a **volume identifier header**
(`UBI!`: volume id, logical eraseblock number, sequence number) in the next page; the
logical eraseblock's data starts at the third page (126976 bytes per LEB). The layout
volume holds the volume table (names, sizes) twice. CRCs are UBI's CRC-32 (initial value
0xffffffff, no final inversion).

`ubi_lite` scans every PEB (bad blocks skipped), keeps for each (volume, LEB) the copy with
the highest sequence number, reads the volume table, and reads LEBs. Writing replaces a
LEB the way UBI does ("atomic LEB change"): the new contents go to a free PEB under a VID
header with a higher sequence number, are read back and compared, and only then is the old
PEB erased and given a new erase counter header.

If the power fails in between, both copies exist, and the newer one is not necessarily
the good one: the VID header is written first, so a copy cut off half-way already has the
higher sequence number. That is why the VID header also carries the data's size and CRC
(`copy_flag` set). When the scan finds two copies of a LEB, it checks the newer one's
data against its CRC and keeps the older one if they do not match, as Linux's
`compare_lebs()` does. The volume table gets the same care: if copy 0 of the layout
volume has a record that is not valid, copy 1 is used, as Linux's `process_lvol()`
does, and a damaged copy is never written back.

A first version had none of this: it wrote no CRC, and took the newer copy whatever it
held. Cutting the power at simulated moments showed it (`CUT=` in `tools/ubi/`,
[Section 10.7](#107-cutting-the-power-on-purpose)): a `saveconfig` interrupted while writing left a LEB half new, half old. Only
dynamic volumes; volumes can be
removed, created and renamed ([Section 10.5](#105-removing-a-volume),
[Section 10.6](#106-creating-a-volume-and-swapping-names)); no wear-leveling moves or
fastmap.

The volumes are character devices (minor = volume id):

```
mknod /dev/ubivol4 c <major> 4          # the major is printed at attach
dd if=/dev/ubivol4 bs=126976 count=116 | head -c <size> | sha256
```

**Only the volumes named `config` and `upgrade` can be written**: the saved settings, and
a new system being installed by `sysupgrade`
([Section 4.2](04-updating-and-recovery.md#42-updating-netbsd)); the others hold boot
images, the running one among them. Tested:
opening the `kernel` volume for writing fails with `EROFS` ("Read-only file system"), and
its contents stay identical to the installed image.

### Verified

- The scan matches U-Boot's: 872 PEBs (one bad), the maximum sequence number, the six
  volumes and their sizes.
- The `kernel` volume read through the driver has the same SHA-256 as the image written
  into it (14602398 bytes).

- Writing `config` (2 LEBs, created in U-Boot with `ubi create config 0x3e000`):
  200 KB of random data written and read back identical; a second, shorter write
  (150000 bytes, the old PEBs erased and given new erase counters) leaves the new data
  followed by the old, as on a disk. Both survive a reboot, and U-Boot's `ubi part ubi`
  attaches cleanly afterwards (same maximum sequence number, 0 corrupt PEBs) and reads the
  same bytes.
- The first write attempts had failed the read-back check at byte 1548, the start of the
  last codeword: the 16 spare bytes were written over the start of the codeword buffer
  instead of after the 500 data bytes.

### Settings in `config`

`saveconfig` writes a tar archive of the settings to the volume, whole LEBs at a time
(`dd bs=126976 conv=sync`, so one flash update per LEB); `/etc/rc` checks for a tar header
(`ustar` at byte 257) and unpacks it, otherwise falls back to the USB stick. Tested: settings
saved (about 120 KB, one LEB), a marker in `/etc/hosts`, the ssh host keys and file owners
and modes come back after a reboot; ssh logs in with the same host key.

## 10.5. Removing a Volume

### Why

A stock 7530's UBI partition is full: AVM's two system copies (`avm_filesys_0` and
`avm_filesys_1`, 364 LEBs each), its settings and user data leave no eraseblock
available. The boot loader, U-Boot 2012.07, can create volumes (`help ubi` lists part,
info, create, write and read) but cannot remove one. OpenWrt's installation removes
AVM's volumes from OpenWrt running in RAM, with `ubirmvol`. To install without
OpenWrt, NetBSD has to do it:

```
# sysctl -w hw.qcomqpic0.ubi_remove=avm_filesys_0
```

A sysctl, not a new program: `sysctl(8)` is already in the image, and the driver
gets a string, looks the name up, and returns an error number, which is all a
command needs.

### What removing is

A volume exists because of its record in the volume table; its eraseblocks are just
eraseblocks whose VID header names it. Linux's `ubirmvol` (`ubi_remove_volume()` in
`drivers/mtd/ubi/vmt.c`) clears the record, writes the table, then erases the
eraseblocks. `ubi_lite_remove()` does the same, in the same order:

1. Read the volume table from the layout volume, all 128 records (22016 bytes, padded
   to whole pages: 22528), not only the 32 that `ubi_lite` otherwise looks at, so
   records it does not know are written back unchanged.
2. Replace the volume's record with an **empty record**.
3. Write the table into the layout volume's LEB 0, then LEB 1, each an atomic LEB
   change ([Section 10.4](#104-ubi)).
4. Erase every PEB of the volume and give it a new erase counter header.

If the power fails after step 3, the table no longer has the volume but its PEBs still
carry its id. UBI's attach handles that: PEBs of a volume the table does not have are
erased (`check_attaching_info()` in `vtbl.c`). If it fails between the two copies of
step 3, they differ; attach trusts copy 0 and rewrites copy 1 from it, which is why
copy 0 goes first.

### The details that matter

Three things UBI checks at attach, any of which would make it refuse the whole device,
not just one volume:

- **An empty record is all zeros with a valid CRC**, not all zeros and not all 0xff.
  `vtbl_check()` compares every record whose `reserved_pebs` is 0 with this exact
  pattern. The stock table confirms it: its 124 unused records are just that.
- **The layout volume's VID headers carry `compat` = 5** (`UBI_COMPAT_REJECT`: "a UBI
  that does not know this internal volume must not attach the device"). The VID header
  of a user volume has `compat` 0, and `validate_vid_hdr()` rejects an internal volume
  with 0. The writing function gained a `compat` argument for this.
- **Both copies must be identical**, byte for byte over the table's size.

### How it was tested

Without root on the PC there is no `nandsim` and no Linux UBI to try it on, so:

1. **A copy of the stock flash, on the PC.** `ubi_lite.c` is OS-independent: a small
   harness (`tools/ubi/ubi-harness.c`) compiles it on Linux with the flash simulated in a file (the UBI partition
   from a backup; programming only clears bits, as on NAND; a page of all 0xff reads as
   erased). It removed `avm_filesys_0` and `avm_filesys_1`: 464 PEBs erased, 855 free.
2. **An independent check.** A Python script (`tools/ubi/ubi-check.py`), written from
   Linux's attach rules rather
   than from `ubi_lite`, checks every EC and VID header, the `compat` values, both table
   copies, every record and empty record, that no PEB belongs to a volume the table
   lacks, that the other volumes' data is unchanged, and that every erase counter that
   changed went up by exactly one. It passes the untouched stock image (which checks the
   checker) and the result.
3. **On the router.** A volume `test` made in U-Boot (`ubi create test 0x3e000`, two LEBs
   written), removed with the sysctl from NetBSD booted from RAM; full backups before
   and after compared with the same script; then U-Boot's own UBI code, a port of
   Linux's, attached the partition with no warning, the volume gone and its two
   eraseblocks available again (532, as before `test` was created); NetBSD's settings in
   `config` loaded as usual.

What was not tested: removing AVM's volumes on the router itself, since this box no
longer has them, and a power failure in the middle.

## 10.6. Creating a Volume and Swapping Names

### An update that a power failure cannot break

Before `sysupgrade`, updating meant `ubi write` into the volume `kernel` from U-Boot: the
running system overwritten in place. Cut the power half-way and `kernel` is half old,
half new, and nothing in the flash boots. The usual answer, in phones and in routers,
is two slots, **A/B**: write the new system next to the old one, then switch, with the
switch a single step that either has happened or has not.

Here the switch is free. U-Boot boots the volume *named* `kernel`, and a name is a field
of a record in the volume table. Swapping the names of two volumes is one write of the
table, and the table write is already atomic in the sense that matters
([Section 10.5](#105-removing-a-volume)): copy 0 is written first and attach trusts copy
0, so after a power failure the table is either the old one or the new one. So
`sysupgrade` writes the image into `upgrade`, checks it, and swaps the names of
`kernel` and `upgrade`. Before the swap the old system boots; after it, the new one;
the old one is still there, under the name `upgrade`.

### Creating a volume

`upgrade` has to exist first. U-Boot can create it, but asking the owner to do that at the
serial console would defeat the purpose, so `ubi_lite_create()` does it: find an unused
record (all zeros with a valid CRC), fill it in (`reserved_pebs`, alignment 1, data pad 0,
dynamic, the name), write the table.

The one real question is **space**. UBI refuses to attach a device whose volumes reserve
more PEBs than it has, after its own reserves: the two layout PEBs, one for
wear-leveling, and a pool for replacing blocks that go bad. Get this wrong and U-Boot no
longer attaches the partition at all. The size of that pool differs between UBI
versions: U-Boot 2012.07 keeps 1% of the PEBs (it prints "number of PEBs reserved for bad
PEB handling: 8"), Linux since 3.7 keeps 20 per 1024 PEBs of the whole chip (AVM's
Linux kept 19). `ubi_lite` counts 20, the larger, and asks for a margin of 8 more PEBs
besides, so that any of them accepts the result.

A detail: a removal interrupted by a power failure can leave PEBs that carry a volume id
the table no longer has. Linux and U-Boot erase those at attach; `ubi_lite` does not, so
it erases them before giving their id to a new volume, or they would show up as the new
volume's contents.

### The interface

Three more sysctls next to `ubi_remove`, all writing the table under the driver's lock:

```
# sysctl -n hw.qcomqpic0.ubi_volumes        # id, name, LEBs
# sysctl -w hw.qcomqpic0.ubi_create="upgrade 133"
# sysctl -w hw.qcomqpic0.ubi_swap="kernel upgrade"
```

`sysupgrade` (a shell script, `/usr/sbin/sysupgrade`) uses them, `dd` to write the volume,
and `cksum -a sha256` to compare what it reads back with what it was given.

### Verified

- In the PC harness (`tools/ubi/`), on a backup of the router: remove `test`, create
  `upgrade` (133 LEBs), write the image into it, swap with `kernel`. The checker passes,
  and the volume named `kernel` afterwards holds exactly the image, the one named
  `upgrade` exactly the old `kernel` volume. Creating a 600-LEB volume fails with
  `ENOSPC`; swapping with a name that does not exist, `ENOENT`.
- On the router: from #10 to #11 with `sysupgrade` (23 seconds); U-Boot then attaches
  with no warning, 7 volumes, 399 PEBs available (532 before, minus the 133 of
  `upgrade`), and boots #11. `sysupgrade -r` brought #10 back, and (run from #11 booted
  from RAM, since #10 has no `sysupgrade`) #11 again.
- Refused: the USB kernel image and 16 MB of random data, with nothing written.

Not tested: a power failure in the middle of a real update.

## 10.7. Cutting the Power on Purpose

The claims "a power failure leaves the old state or the new one" in Sections 10.4 to 10.6
are easy to write and hard to trust. They can be tested without pulling a plug a thousand
times: the harness in `tools/ubi/` simulates the flash, and with `CUT=n` in the
environment the flash stops changing after n program and erase operations, as if the
power had failed at that moment. The next scan then reads what is there.

Running an operation once to count its flash operations, and then once for every n from
0 to that count, tries every moment a power failure could hit. After each, the checker
(`ubi-check.py`, following Linux's rules) must accept the result, and the result must be
either the old state or the new one:

| operation | flash operations | result after a cut |
|---|---|---|
| swapping two names (`sysupgrade`'s switch) | 28 | old names before operation 12, new names from it; never anything else |
| rewriting two LEBs of `config` (`saveconfig`) | 104 | each LEB old or new, never mixed |
| removing a 364-LEB volume | 488 | the volume there before operation 12, gone from it; in between, PEBs the table no longer has, which attach erases |

The same sweep against the first version gave mixed LEBs. And a table write cut short,
followed by another change of the table, now leaves a table UBI accepts; the first
version returned a bogus error there (a page of 0xff written and read back looks erased
to the controller, and that was taken for a read error, also fixed).

On the router, the fixed code then wrote an image with `sysupgrade` and the settings with
`saveconfig`; U-Boot attached the partition with no warning and booted the new image,
and the settings came back.

