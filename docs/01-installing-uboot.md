# Chapter 1. Installing U-Boot

## 1.1. Synopsis

NetBSD is loaded by **U-Boot**, a boot loader the stock FRITZ!Box 7530 does not have.
OpenWrt builds a U-Boot for this model, and AVM's own boot loader can run it from RAM
without installing anything. This chapter uses that to start U-Boot, then NetBSD, both
from RAM; backs up the whole flash from NetBSD; and finally writes U-Boot into the
flash with U-Boot itself. OpenWrt is not installed at any point: only its U-Boot file is
used.

After reading this chapter, you will know:

- how the 7530 boots, and which parts are never changed;
- how to connect the serial console;
- how to run U-Boot and NetBSD from RAM on a stock box;
- how to back up the whole flash;
- how to install U-Boot, and how to get back if something goes wrong.

Before reading this chapter, you should have:

- a **3.3 V USB serial adapter** and three jumper wires;
- a **Linux PC with a wired Ethernet port** (`enp0s31f6` below), NetworkManager
  (`nmcli`), `dnsmasq`, `python3` and a terminal program for the serial console
  (`picocom`, `screen`, ...);
- an Ethernet cable from the PC to a **LAN port** of the 7530;
- a clone of this repository: the scripts are in `prepare/`.

> [!WARNING]
> This chapter writes the boot loader partitions of the flash. Read all of it before
> starting, and keep a copy of the backup it makes.

> [!NOTE]
> Each step of this chapter was tested on the author's box, but that box no longer
> had the stock system when U-Boot was installed this way: it had been installed
> through OpenWrt before ([Section 1.7](#17-the-other-way-through-openwrt)). The
> steps that read a stock flash were tested on a copy of one.

## 1.2. How the Box Boots

```
PBL (ROM inside the SoC)
 └→ SBL1 (Qualcomm second-stage loader, in flash)
     └→ QSEE / TrustZone (secure-world firmware)
         └→ EVA (AVM's own loader, also called "urlader")
             └→ whatever is in the uboot0 partition:
                  stock: AVM's Linux kernel
                  after this chapter: U-Boot (OpenWrt's u-boot-fritz7530)
                   └→ NetBSD (the UBI volume "kernel"), or an image from the PC (TFTP)
```

- **PBL to EVA are the vendor's and are never touched.** EVA cannot boot NetBSD kernels,
  but it starts whatever is in `uboot0` (or `uboot1`, its spare copy); U-Boot goes
  there, in place of AVM's kernel.
- **EVA is the way back.** For a few seconds after power-on it listens for FTP at
  192.168.178.1 and can run a program straight from RAM
  (`prepare/scripts/eva_ramboot.py`). As long as EVA is intact, a broken system in flash
  can always be repaired.

## 1.3. Connecting the Serial Console

- Where: the 4-pin header below the DECT chip (Dialog SC14448); the square pad is VCC.
- Order: VCC - RX - TX - GND, 115200 8N1, **3.3 V**.
- Connect only RX, TX and GND, crossed (adapter TX → board RX). **Do not connect VCC.**

```
% picocom -b 115200 /dev/ttyUSB0
```

- Output but no input: usually the board's RX pin (2) or its wire.
- Output stopping half-way, or nothing at all: often a loose jumper wire. Check the wires
  before the software.

> [!IMPORTANT]
> **Power-cycle with the adapter unplugged.** The adapter is powered by the PC; through
> its TX line it can keep the SoC partly powered when the board's supply is removed, and
> the board then comes back with the power LED on but no output at all. Unplug the
> board's supply and the adapter's USB, wait ten seconds, plug in the board, then the
> adapter.

## 1.4. Getting the Files

Firmware is not part of the repository, only its checksums. Two files are needed:
U-Boot as built by OpenWrt 25.12.6, and the NetBSD image from this project's
[releases page](../../../releases) ([Section 2.3](02-installing-netbsd.md#23-the-release-images)).

```
% cd prepare/firmware
% curl -O https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic/u-boot-fritz7530/uboot-fritz7530.bin
% sha256sum -c sha256sums --ignore-missing
uboot-fritz7530.bin: OK
% mkdir -p ../tftp
% cp uboot-fritz7530.bin ../tftp/
% cp ~/Downloads/netbsd-flash.itb ../tftp/
```

`prepare/tftp/` is what the PC serves to the router over TFTP.

## 1.5. Running from RAM, and the Backup

Nothing is written to the flash in this section.

The PC must have no other network on 192.168.1.x, 192.168.77.x or 192.168.178.x. If it
has a firewall, allow TFTP and ssh from the router: with ufw,
`sudo ufw allow in on enp0s31f6`. Keep the router powered off for now. Run everything
in `prepare/`.

### 1.5.1. U-Boot into RAM through EVA

In terminal A, keep it running:

```
% sudo ./flash.sh
```

The script gives the wired port three addresses (192.168.178.10 for EVA, 192.168.1.70
and 192.168.77.2 for TFTP), starts dnsmasq as TFTP server for `prepare/tftp/`, waits for
you to power the box on, sends U-Boot to EVA (into RAM at 0x85000000) and starts it.

On the serial console, press a key at `Hit any key to stop autoboot`. The U-Boot prompt
appears:

```
(FRITZ7530) #
```

If you miss it, U-Boot looks for a UBI volume named `kernel` (a stock box has none) and
then tries TFTP every few seconds: press Ctrl-C.

### 1.5.2. NetBSD into RAM

```
(FRITZ7530) # setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
(FRITZ7530) # tftpboot 0x85000000 netbsd-flash.itb
(FRITZ7530) # bootm 0x85000000
```

After about 30 seconds the console shows `login:`. Log in as `root` (no password on the
console). The NAND driver lists what it found in the flash:

```
# dmesg | grep qcomqpic0
qcomqpic0: UBI volume 0 "avm_filesys_0": 364 LEBs, ...
qcomqpic0: UBI volume 1 "avm_filesys_1": 364 LEBs, ...
qcomqpic0: UBI volume 2 "avm_config": 17 LEBs, ...
qcomqpic0: UBI volume 3 "avm_userdata": 103 LEBs, ...
```

These are AVM's: two copies of its system, its settings, and user data.

### 1.5.3. Backing up the Whole Flash

The backup is copied over ssh. Allow your key on the console, as in
[Section 3.3](03-configuring-the-router.md#33-allowing-ssh-logins) (there is nowhere to
save it yet: it lasts until the router is reset):

```
# mkdir -p /root/.ssh
# echo 'ssh-ed25519 AAAA... you@pc' > /root/.ssh/authorized_keys
```

Then, in terminal B on the PC:

```
% ./backup-netbsd.sh
```

It reads the whole chip through the NAND driver (read-only) and writes one file per
partition and per UBI volume into `prepare/backup/<date>/`, with checksums, in about a
minute. `layout.txt` should list `uboot0`, `uboot1` and the four `avm_` volumes.

> [!CAUTION]
> The backup may contain device keys and WiFi passwords. Never commit or upload it
> (`prepare/backup/` is git-ignored). Keep a copy somewhere else.

## 1.6. Writing U-Boot into the Flash

> [!WARNING]
> This section writes the flash: the `uboot0` and `uboot1` partitions, where AVM's
> kernel is. After it, the box no longer starts AVM's system (its files stay in the UBI
> partition until [Chapter 2](02-installing-netbsd.md) removes them).

1. Power the box off, and start U-Boot from RAM again: `sudo ./flash.sh` in terminal A
   (if it is still running, Ctrl-C it first), power on, and press a key on the serial
   console ([Section 1.5.1](#151-u-boot-into-ram-through-eva)).

2. Load U-Boot's file into RAM, and check it:

   ```
   (FRITZ7530) # setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
   (FRITZ7530) # tftpboot 0x84000000 uboot-fritz7530.bin
   (FRITZ7530) # crc32 0x84000000 ${filesize}
   CRC32 for 84000000 ... 8407ffff ==> 1640d699
   ```

   The CRC must be `1640d699` (the file of OpenWrt 25.12.6). If not, stop here.

3. Write the spare copy, `uboot1`, first, and read it back:

   ```
   (FRITZ7530) # nand erase 0xf00000 0x80000
   (FRITZ7530) # nand write 0x84000000 0xf00000 0x80000
   (FRITZ7530) # mw.l 0x85000000 0 0x20000
   (FRITZ7530) # nand read 0x85000000 0xf00000 0x80000
   (FRITZ7530) # crc32 0x85000000 0x80000
   CRC32 for 85000000 ... 8507ffff ==> 1640d699
   ```

   `mw.l` fills the buffer with zeros first, so that the CRC really comes from what was
   read back. If the CRC differs, write the copy again.

4. The same for `uboot0`, the copy EVA starts:

   ```
   (FRITZ7530) # nand erase 0xb00000 0x80000
   (FRITZ7530) # nand write 0x84000000 0xb00000 0x80000
   (FRITZ7530) # mw.l 0x85000000 0 0x20000
   (FRITZ7530) # nand read 0x85000000 0xb00000 0x80000
   (FRITZ7530) # crc32 0x85000000 0x80000
   CRC32 for 85000000 ... 8507ffff ==> 1640d699
   ```

5. Reset, and stop U-Boot again: this time EVA starts it from the flash.

   ```
   (FRITZ7530) # reset
   ...
   Hit any key to stop autoboot:  0
   (FRITZ7530) #
   ```

The addresses come from U-Boot's partition table (`printenv mtdparts`): `kernel` at
0xb00000 is `uboot0`, `reserved-kernel` at 0xf00000 is `uboot1`, 4 MB each. U-Boot is
512 KB; only that much is erased and written, as OpenWrt's installation does: the rest
of each partition keeps what AVM left there, which nothing reads. `nand erase` and
`nand write` skip bad blocks. Bad blocks differ from chip to chip; `nand bad` lists
them (the author's box has one, at 0x4140000, in the UBI partition). If one is inside
0xb00000-0xb80000 or 0xf00000-0xf80000, stop: U-Boot would then be written past it,
and whether EVA can start it that way is unknown.

> [!WARNING]
> Never run `saveenv` in U-Boot. Settings are typed (or scripted) each time and never
> stored, so U-Boot keeps behaving exactly as OpenWrt built it.

Continue with [Chapter 2](02-installing-netbsd.md).

## 1.7. The Other Way: through OpenWrt

This repository started with OpenWrt's installation method for this model (OpenWrt
commit 95b0c07a618f, "ipq40xx: add support for FritzBox 7530"): OpenWrt runs from RAM,
backs up the flash with `prepare/backup.sh`, and writes U-Boot and OpenWrt into the
flash. NetBSD is then installed next to OpenWrt, which stays as a second system
([Section 2.7](02-installing-netbsd.md#27-next-to-openwrt)).

1. Download, besides U-Boot, OpenWrt's initramfs and sysupgrade images into
   `prepare/firmware/`, and copy the initramfs to `prepare/tftp/FRITZ7530.bin`:

   ```
   % u=https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic
   % curl -O $u/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-initramfs-uImage.itb
   % curl -O $u/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin
   % sha256sum -c sha256sums
   % cp openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-initramfs-uImage.itb ../tftp/FRITZ7530.bin
   ```

2. `sudo ./flash.sh`, power on, and **do not** stop U-Boot: it fetches `FRITZ7530.bin`
   and starts OpenWrt from RAM. U-Boot answers ping too: wait until dnsmasq logs `sent
   .../FRITZ7530.bin` and ssh to 192.168.1.1 works (about 2 minutes).

3. `./backup.sh` copies every MTD partition and UBI volume into `prepare/backup/<date>/`.

4. `./install.sh` copies the files to the router; then, on the router
   (`ssh -o UserKnownHostsFile=/dev/null root@192.168.1.1`):

   ```
   # mtd write /tmp/uboot-fritz7530.bin uboot0
   # mtd write /tmp/uboot-fritz7530.bin uboot1
   # ubirmvol /dev/ubi0 --name=avm_filesys_0
   # ubirmvol /dev/ubi0 --name=avm_filesys_1
   # sysupgrade -n /tmp/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin
   ```

   `Command failed: Connection failed` and the ssh session dropping are normal. The
   reboot takes 2-3 minutes; do not cut the power meanwhile.

## 1.8. Troubleshooting

| symptom | cause | fix |
|---|---|---|
| `flash.sh` waits for EVA forever | cable not in, port NO-CARRIER | plug it, power-cycle (EVA is only there for a few seconds) |
| `tftpboot` times out (`T T T`) | `ipaddr`/`serverip` not set after a reset (they are back to 192.168.1.x), or the PC firewall blocks TFTP | `setenv` them again; allow TFTP |
| `tftpboot` reports every port "Down" | the link was not up yet | run it again |
| power LED blinks slowly, console at `Eva_AVM >` | EVA stopped at its prompt (perhaps noise on the serial line at power-on) | type `go` |
| the console shows nothing after power-on | the serial adapter back-powers the board | power-cycle with the adapter unplugged ([Section 1.3](#13-connecting-the-serial-console)) |

## 1.9. Recovery

- **EVA is always there**: `sudo ./flash.sh` starts U-Boot from RAM whatever is in the
  flash, and from there NetBSD or OpenWrt from RAM.
- **Back to stock**: the backup in `prepare/backup/`, or AVM's recovery tool.
