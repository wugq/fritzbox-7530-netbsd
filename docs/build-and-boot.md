# Step 1: building and booting NetBSD

## How the code is organised

NetBSD's source tree (`src/`) is not in this repository, only what was added and changed:

- `sys/`: **new** files, at the same paths as in NetBSD's tree:
  `sys/arch/arm/qcom/qcom_uart.c` is `src/sys/arch/arm/qcom/qcom_uart.c`. `tools/build.sh`
  makes a symbolic link in `src/` for each of them, pointing back here, so code is edited
  in the repository.
- `patches/`: changes to **stock** files, as diffs, applied by `build.sh` before building:
  - `0001`: one line in `files.generic`, `include "arch/arm/qcom/files.qcom"`, so the
    kernel configuration system knows the qcom directory.
  - `0002`: GENERIC: `SOC_IPQ40XX`, our drivers, each in the section of its kind (the UART
    with the UARTs, the watchdog with the watchdogs, ...), `NET_MPSAFE` and `GATEWAY`.
  - `0003`: NetBSD's GIC driver learns the `qcom,msm-qgic2` compatible string.
  - `0004`: `cpufreqdt` accepts OPP tables without `opp-microvolt` (the IPQ4019 has no CPU
    regulator; Linux allows it too), so CPU frequency scaling works.

Why hook into **GENERIC** rather than write a board kernel first? On ARM, NetBSD's idea is
one kernel for every board: GENERIC carries every SoC's drivers, and what differs between
boards (which devices, at which addresses) is in the **device tree** (DTB) the boot loader
hands over. The kernel attaches drivers for what the device tree lists. Adding a board is
mostly drivers and a device tree. (A small board kernel came later, see
[kernel-config.md](kernel-config.md).)

### What is in files.qcom

```
defflag	opt_soc.h	SOC_IPQ40XX                   # defines a kernel option
file	arch/arm/qcom/ipq4019_platform.c	soc_ipq40xx   # built only with that option
device	qcomuart: tty                           # declares a device class: a tty
attach	qcomuart at fdt with qcom_uart          # it attaches on the device tree bus
file	arch/arm/qcom/qcom_uart.c	qcom_uart        # built when a kernel config uses it
```

`qcomuart* at fdt? pass 4` in a kernel configuration means: attach a `qcomuart` to every
matching device tree node; `pass 4` orders the attachments (the console early, the
network late).

## The device tree

The board's device tree is `sys/arch/arm/dts/qcom-ipq4019-fritzbox-7530.dts`. It
`#include`s NetBSD's own `qcom-ipq4019.dtsi` (imported from Linux, under
`src/sys/external/gpl2/dts/`) and only adds this board's parts:

- memory size, console (`stdout-path`), boot arguments (`bootargs`);
- devices the dtsi leaves disabled: UART, watchdog, USB 3, NAND, ...;
- **the ESS (switch + EDMA) nodes**: Linux mainline never merged an IPQ40xx Ethernet
  driver, so the dtsi has none; these come from OpenWrt;
- buttons, LEDs, the USB power switch (a GPIO "hog"), the NAND's UBI partition.

**No MAC address in it**: at `bootm`, U-Boot fills `local-mac-address` into the node the
`ethernet0` alias points to.

NetBSD's habit is to **fix drivers, not device trees**. The dtsi describes this chip's GIC
only as `qcom,msm-qgic2`, which NetBSD's GIC driver did not know: rather than adding
another compatible string to the device tree, the driver's list gets one more entry (patch
0003). Likewise, the USB controller sits inside a `qcom,dwc3` "glue" node no NetBSD driver
claimed, so the controller inside was never seen: a tiny glue driver, `qcom_dwc3.c`,
attaches to that node and hands its children to the FDT bus.

The `-usbroot` variant only includes the main file and sets `bootargs` to `root=sd0a -v`.

The device tree uses macros like `GIC_SPI` and `GCC_ESS_CLK`, so `build.sh` runs the C
preprocessor first and `dtc` after, as Linux and NetBSD do.

## Building

```
tools/build.sh
```

The first run:
1. clones NetBSD's `netbsd-11` branch shallowly into `src/` (about 4 GB);
2. builds the cross toolchain into `tooldir/` (about 15 minutes, once);
3. builds the GENERIC kernel into `obj/`;
4. compiles the device trees with `dtc` and packs FIT images with `tools/mkfit.py` into `tftp/`.

After a code change, run it again: only what changed is rebuilt (a minute or so). The
kernel build log is `build-kernel.log`. `KERNEL=FRITZ7530 tools/build.sh` builds the board
kernel instead; its images get the configuration name (`tftp/netbsd-usb-FRITZ7530.itb`).

For `tftp/netbsd.itb`, NetBSD's installation ramdisk is needed in `dl/ramdisk.fs`:

```
curl -o dl/ramdisk.fs https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.1/evbarm-earmv7hf/installation/ramdisk/ramdisk.fs
```

### Why a FIT image, and why load at 0x80000040

- This U-Boot (Qualcomm's IPQ version of 2012.07) **only boots FIT images**; a legacy
  uImage gives `unknown image format type`. A FIT is itself a device-tree-format file
  holding the kernel, the DTB and optionally a ramdisk.
- The kernel's virtual addresses start at 0x80000040; NetBSD wants the virtual-to-physical
  offset aligned to 1 MB; RAM starts at physical 0x80000000. Together only "virtual =
  physical" works, so the kernel must be loaded exactly at 0x80000040, or it panics with
  "overlapping mappings".

## Booting

### 1. TFTP server on the PC (terminal A, keep it running)

```
sudo tools/tftp-serve.sh
```

It gives the wired port 192.168.77.2 and serves `tftp/` with dnsmasq.

### 2. Loading from U-Boot

Power on, press a key at `Hit any key to stop autoboot`, then at `(FRITZ7530) #`:

```
setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
tftpboot 0x85000000 netbsd.itb
bootm 0x85000000
```

- The settings stay in RAM: **no `saveenv`**.
- If every port shows "Down", the link was not up yet: `tftpboot` again.
- `netbsd.itb` carries the installation ramdisk: in sysinst, `a` (English) then `x`
  (exit) gives a shell.

### 3. A full NetBSD from a USB stick

Write the official image to a stick:

```
curl -O https://cdn.netbsd.org/pub/NetBSD/NetBSD-11.1/evbarm-earmv7hf/binary/gzimg/armv7.img.gz
gunzip armv7.img.gz
sudo dd if=armv7.img of=/dev/sdX bs=1M     # sdX: the stick -- check twice
```

Plug it into the 7530's USB port and boot `netbsd-usb.itb` instead (its device tree says
`root=sd0a`). The first boot grows `/` to the whole stick and reboots; then log in as root
(no password). NetBSD prints nothing when the stick attaches; `sysctl hw.disknames` shows it.

### 4. Network

```
ifconfig qcomedma0 192.168.77.1/24 up
```

## Automation

`tools/nb-cycle.sh` does all of the above through the serial console: reboot into U-Boot
(from OpenWrt or NetBSD, logging in first if NetBSD sits at its login prompt; `go` if EVA
stopped), set the addresses, arm the hardware watchdog, TFTP, boot, and wait for the
login prompt.

```
ITB=netbsd-usb.itb tools/nb-cycle.sh      # root on the USB stick
tools/nb-cycle.sh                         # installation ramdisk
UBOOT_ONLY=1 tools/nb-cycle.sh            # stop at the U-Boot prompt
```

It needs the serial logger running:

```
tools/serial-log.sh /dev/ttyUSB0 serial.log &
```

The logger reconnects by itself when the USB serial adapter is re-plugged (it watches
the device node's inode: a re-created `/dev/ttyUSB0` leaves a plain `cat` reading a dead
device forever). `tools/sw.sh "command"` sends one line to the router's console (32
characters at a time, at most 250 per line) and prints the new output;
`tools/op.sh "note" command...` runs a command on the PC and records it in `ops.log`.

The PC's Intel e1000e NIC sometimes drops into a low-power mode when the router resets
the link (the router then reports the port at 10 Mbit/s and the PC sees no carrier);
`nmcli con up fritz-netbsd` wakes it up, and `nb-cycle.sh` does that between TFTP tries.

## Early console

Between kernel entry and the UART driver's attach, nothing is printed. To see that part,
uncomment in the kernel configuration (`VERBOSE_INIT_ARM` is GENERIC's own, the `EARLYCONS`
line comes with patch 0002):

```
options 	VERBOSE_INIT_ARM
options 	EARLYCONS=qcom, CONSADDR=0x078af000
```

## Test bench and automated tests

The test bench: a PC (192.168.77.2) on switch port 4 and a second machine running
FreeBSD (192.168.78.2) on switch port 3, with `iperf3 -s -D` on it; the router routes
between 192.168.77.0/24 and 192.168.78.0/24 (`hw.qcomess0.isolate=1`, so the switch does
not bridge the two ports by itself). Seen from the front, the port farthest from the DSL
jack is switch port 4, the next one port 3.

`tools/hwtest.sh` is a whole-system test, not a unit test: the script drives the router
and the second machine over their serial consoles and runs iperf3/nc on the PC.

```
tools/hwtest.sh "what changed"     # boot tftp/netbsd-usb.itb and test, ~5 minutes
NOBOOT=1 tools/hwtest.sh           # test the running kernel
QUICK=1 tools/hwtest.sh            # skip throughput
ITB=netbsd-usb-FRITZ7530.itb tools/hwtest.sh
```

Checks: boot, PSGMII calibration, USB stick, both switch ports up, ping both ways,
console input, 100 MB of random data through the router and back (sha256), the WAN port
(isolation, `vlan2`, full-size frames), back to the default set-up, console after load;
throughput: six iperf3 runs in the default mode and six through the VLAN WAN port. The
report goes to `private/test-results/` (not published) and is compared with the previous
one, marking changes over 10%.

What cannot be automated: a real power cycle (a relay on the router's supply would do it).
