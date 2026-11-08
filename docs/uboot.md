# Step 0: installing U-Boot

NetBSD's kernel is loaded by **U-Boot**. The stock FRITZ!Box 7530 has none, so the first
step is to install it. The easy way is OpenWrt's official installation method, which puts
U-Boot into the flash along the way. OpenWrt itself stays useful: a known-good system to
go back to.

**This step writes the flash.** Read all of it first, and keep a copy of the backup it makes.

## How the box boots

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
- EVA sometimes stops at its `Eva_AVM >` prompt (visible on the serial console, power LED
  blinking slowly). Typing `go` makes it carry on; `tools/nb-cycle.sh` does that.

## You need

- A Linux PC with a wired port (`enp0s31f6` below), `dnsmasq`, `python3`.
- A cable from the PC to a **LAN port** of the 7530, which stays powered off for now.
- No other network of the PC on 192.168.1.x or 192.168.178.x.
- If the PC has a firewall, allow TFTP: `sudo ufw allow in on enp0s31f6 from 192.168.1.0/24`.
- Three files in `prepare/firmware/`, checked against `prepare/firmware/sha256sums`:
  - `uboot-fritz7530.bin` (U-Boot as built by OpenWrt)
  - `openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-initramfs-uImage.itb`
    (OpenWrt running from RAM; also copy it to `prepare/tftp/FRITZ7530.bin`)
  - `openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin`
- Firmware is not part of the repository (copyright and size), only its checksums.

## Steps

Run everything in `prepare/`.

### 1. U-Boot and OpenWrt into RAM through EVA (terminal A, keep it running)

```
sudo ./flash.sh
```

The script gives the wired port two addresses (192.168.178.10 for EVA, 192.168.1.70 for
TFTP), starts dnsmasq as TFTP server, waits for you to power the box on, sends U-Boot to
EVA (into RAM at 0x85000000) and starts it. U-Boot then fetches `FRITZ7530.bin`, the
OpenWrt initramfs, over TFTP.

**Nothing has been written to the flash yet.**

U-Boot answers ping too: 192.168.1.1 answering does not mean OpenWrt is up. Wait until
dnsmasq logs `sent .../FRITZ7530.bin` and ssh works (about 2 minutes).

### 2. Back up the whole flash (terminal B)

```
./backup.sh
```

It copies every MTD partition and UBI volume over ssh into `prepare/backup/<date>/`, with
checksums. `layout.txt` should list `uboot0`, `uboot1`, `avm_filesys_0`, `avm_filesys_1`.

**The backup may contain device keys and WiFi passwords. Never commit or upload it**
(`prepare/backup/` is git-ignored). Keep a copy somewhere else.

### 3. Write the flash (terminal B)

```
./install.sh
ssh -o UserKnownHostsFile=/dev/null root@192.168.1.1
```

Then, on the router:

```
mtd write /tmp/uboot-fritz7530.bin uboot0      # U-Boot into both partitions
mtd write /tmp/uboot-fritz7530.bin uboot1      # (EVA picks one of them)
ubirmvol /dev/ubi0 --name=avm_filesys_0        # remove the vendor system
ubirmvol /dev/ubi0 --name=avm_filesys_1
sysupgrade -n /tmp/openwrt-25.12.6-ipq40xx-generic-avm_fritzbox-7530-squashfs-sysupgrade.bin
```

`Command failed: Connection failed` and the ssh session dropping are normal. The reboot
takes 2-3 minutes; do not cut the power meanwhile.

This procedure is the one from the OpenWrt commit that added the 7530
(95b0c07a618f "ipq40xx: add support for FritzBox 7530").

### 4. Check

With the serial console connected, power on: SBL, EVA, U-Boot and OpenWrt print in turn.
Pressing a key at `Hit any key to stop autoboot` leaves you at the `(FRITZ7530) #` prompt,
where NetBSD is loaded from.

**Do not run `saveenv` in U-Boot.** Settings are typed (or scripted) each time and never
stored, so U-Boot keeps behaving exactly as OpenWrt installed it.

## Serial console

- Where: the 4-pin header below the DECT chip (Dialog SC14448); the square pad is VCC.
- Order: VCC - RX - TX - GND, 115200 8N1, **3.3 V**.
- Use a 3.3 V USB-TTL adapter, connect only RX, TX and GND, crossed (adapter TX → board RX).
  **Do not connect VCC.**
- Output but no input: usually the board's RX pin (2) or its wire.
- A loose jumper wire shows as output stopping half-way, or nothing at all. Check the wires
  before the software.
- **Power-cycle with the adapter unplugged.** The adapter is powered by the PC; through its
  TX line it can keep the SoC partly powered when the board's supply is removed, and the
  board then comes back with the power LED on but no output at all. Unplug the board's
  supply and the adapter's USB, wait ten seconds, plug the board, then the adapter.

## Problems met

| Symptom | Cause | Fix |
|---|---|---|
| `flash.sh` waits for EVA forever | cable not in, port NO-CARRIER | plug it, power-cycle (EVA is only there for a few seconds) |
| 192.168.1.1 answers ping, ssh/backup hang | the PC firewall blocks TFTP, the box sits in U-Boot retrying every 20 s | allow TFTP (see above), no power cycle needed |
| Power LED blinks slowly, console at `Eva_AVM >` | EVA stopped at its prompt (perhaps noise on the serial line at power-on) | type `go` |

## Recovery

- **EVA is always there**: run `flash.sh` again and repair from OpenWrt in RAM.
- **Back to stock**: the images in `prepare/backup/`, or AVM's recovery tool.
