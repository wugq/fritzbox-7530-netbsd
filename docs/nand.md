# Step 7: NAND and UBI from NetBSD

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

## The chip

`FETCH_ID` returns `c2 f1 80 95`: Macronix, 1 Gbit (128 MB), 8-bit bus (MX30LF1G18AC
class): 2048-byte pages with 64 spare bytes, 64 pages per 128 KB block.

## The controller without its DMA engine

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

## UBI

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
PEB erased and given a new erase counter header. If the power fails in between, both
copies exist and the next scan picks the newer one. Only dynamic volumes; no volume
creation, wear-leveling moves or fastmap.

The volumes are character devices (minor = volume id):

```
mknod /dev/ubivol4 c <major> 4          # the major is printed at attach
dd if=/dev/ubivol4 bs=126976 count=116 | head -c <size> | sha256
```

**Only the volume named `config` can be written**; the others hold boot images. Tested:
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
