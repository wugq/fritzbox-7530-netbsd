# Chapter 1. Installing U-Boot

## 1.1. Synopsis

NetBSD is loaded by **U-Boot**, a boot loader the stock FRITZ!Box 7530 does not have.
There is no ready-made way to install U-Boot on its own: the way that exists is OpenWrt's
installation method for this model, which writes U-Boot and OpenWrt into the flash. This
chapter follows it. OpenWrt stays installed afterwards, as a known-good system to go back
to.

After reading this chapter, you will know:

- how the 7530 boots, and which parts are never changed;
- how to connect the serial console;
- how to back up the whole flash;
- how to install U-Boot and OpenWrt, and how to get back if something goes wrong.

Before reading this chapter, you should have:

- a **3.3 V USB serial adapter** and three jumper wires;
- a **Linux PC with a wired Ethernet port** (`enp0s31f6` below), `dnsmasq`, `python3`
  and a terminal program for the serial console (`picocom`, `screen`, ...);
- an Ethernet cable from the PC to a **LAN port** of the 7530;
- a clone of this repository: the scripts are in `prepare/`.

> [!WARNING]
> This chapter writes the flash, including the boot loader partitions. Read all of it
> before starting, and keep a copy of the backup it makes.

## 1.2. How the Box Boots

```
PBL (ROM inside the SoC)
 └→ SBL1 (Qualcomm second-stage loader, in flash)
     └→ QSEE / TrustZone (secure-world firmware)
         └→ EVA (AVM's own loader, also called "urlader")
             └→ U-Boot (OpenWrt's u-boot-fritz7530, in the uboot0/uboot1 partitions)
                 └→ the kernel (OpenWrt, or NetBSD)
```

- **PBL to EVA are the vendor's and are never touched.** EVA cannot boot OpenWrt or
  NetBSD kernels, but it starts whatever is in `uboot0`; U-Boot goes there.
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

## 1.4. Getting the Firmware

Firmware is not part of the repository, only its checksums. Download the three files of
OpenWrt 25.12.6 into `prepare/firmware/`:

```
% cd prepare/firmware
% u=https://downloads.openwrt.org/releases/25.12.6/targets/ipq40xx/generic
% curl -O $u/u-boot-fritz7530/uboot-fritz7530.bin
% curl -O $u/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-initramfs-uImage.itb
% curl -O $u/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin
% sha256sum -c sha256sums
% mkdir -p ../tftp
% cp openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-initramfs-uImage.itb ../tftp/FRITZ7530.bin
```

| file | |
|---|---|
| `uboot-fritz7530.bin` | U-Boot as built by OpenWrt |
| `...-initramfs-uImage.itb` | OpenWrt running from RAM, used to write the flash |
| `...-squashfs-sysupgrade.bin` | OpenWrt to install into the flash |

## 1.5. Installing U-Boot and OpenWrt

The PC must have no other network on 192.168.1.x or 192.168.178.x. If it has a firewall,
allow TFTP: `sudo ufw allow in on enp0s31f6 from 192.168.1.0/24`. Keep the router
powered off for now. Run everything in `prepare/`.

### 1.5.1. U-Boot and OpenWrt into RAM through EVA

In terminal A, keep it running:

```
% sudo ./flash.sh
```

The script gives the wired port two addresses (192.168.178.10 for EVA, 192.168.1.70 for
TFTP), starts dnsmasq as TFTP server, waits for you to power the box on, sends U-Boot to
EVA (into RAM at 0x85000000) and starts it. U-Boot then fetches `FRITZ7530.bin`, the
OpenWrt initramfs, over TFTP. **Nothing has been written to the flash yet.**

U-Boot answers ping too: 192.168.1.1 answering does not mean OpenWrt is up. Wait until
dnsmasq logs `sent .../FRITZ7530.bin` and ssh works (about 2 minutes).

### 1.5.2. Backing up the Whole Flash

In terminal B:

```
% ./backup.sh
```

It copies every MTD partition and UBI volume over ssh into `prepare/backup/<date>/`, with
checksums. `layout.txt` should list `uboot0`, `uboot1`, `avm_filesys_0`, `avm_filesys_1`.

> [!CAUTION]
> The backup may contain device keys and WiFi passwords. Never commit or upload it
> (`prepare/backup/` is git-ignored). Keep a copy somewhere else.

### 1.5.3. Writing the Flash

In terminal B:

```
% ./install.sh
% ssh -o UserKnownHostsFile=/dev/null root@192.168.1.1
```

Then, on the router:

```
# mtd write /tmp/uboot-fritz7530.bin uboot0
# mtd write /tmp/uboot-fritz7530.bin uboot1
# ubirmvol /dev/ubi0 --name=avm_filesys_0
# ubirmvol /dev/ubi0 --name=avm_filesys_1
# sysupgrade -n /tmp/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin
```

U-Boot goes into both partitions (EVA picks one of them); the vendor system is removed;
OpenWrt is installed. `Command failed: Connection failed` and the ssh session dropping are
normal. The reboot takes 2-3 minutes; do not cut the power meanwhile.

This procedure is the one from the OpenWrt commit that added the 7530
(95b0c07a618f "ipq40xx: add support for FritzBox 7530").

### 1.5.4. Checking

With the serial console connected, power on: SBL, EVA, U-Boot and OpenWrt print in turn.
Pressing a key at `Hit any key to stop autoboot` gives the U-Boot prompt:

```
(FRITZ7530) #
```

This is where NetBSD is loaded from ([Chapter 2](install.md)).

> [!WARNING]
> Never run `saveenv` in U-Boot. Settings are typed (or scripted) each time and never
> stored, so U-Boot keeps behaving exactly as OpenWrt installed it.

## 1.6. Troubleshooting

| symptom | cause | fix |
|---|---|---|
| `flash.sh` waits for EVA forever | cable not in, port NO-CARRIER | plug it, power-cycle (EVA is only there for a few seconds) |
| 192.168.1.1 answers ping, ssh/backup hang | the PC firewall blocks TFTP, the box sits in U-Boot retrying every 20 s | allow TFTP (see above), no power cycle needed |
| power LED blinks slowly, console at `Eva_AVM >` | EVA stopped at its prompt (perhaps noise on the serial line at power-on) | type `go` |

## 1.7. Recovery

- **EVA is always there**: run `flash.sh` again and repair from OpenWrt in RAM.
- **Back to stock**: the images in `prepare/backup/`, or AVM's recovery tool.
