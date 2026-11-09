# NetBSD on the AVM FRITZ!Box 7530

A port of NetBSD 11 to the AVM FRITZ!Box 7530, a home router built around the Qualcomm
IPQ4019 (four Cortex-A7 cores at 716 MHz, 256 MB RAM, 128 MB NAND, a 5-port gigabit
switch). The result is a NetBSD that runs from the router's flash and works as a router:
NAT, DHCP, DNS, IPv6 on the LAN, PPPoE, ssh. WiFi and DSL are not supported.

It is a learning project: every driver was written for this port, from the hardware's
register descriptions with Linux and FreeBSD drivers read for reference, and the
documents explain each piece. It is not an upstream NetBSD submission.

## Using it

Download `netbsd-flash.itb` from the [releases](../../releases) and follow
**[docs/install.md](docs/install.md)**: installing U-Boot, trying NetBSD from RAM,
installing it into the flash, setting it up, updating and going back. You need a 3.3 V
USB serial adapter and a Linux PC with a wired port.

## How it works

Read in this order; each document stands on the ones before it.

| | |
|---|---|
| [docs/uboot.md](docs/uboot.md) | how the 7530 boots, and installing U-Boot |
| [docs/build-and-boot.md](docs/build-and-boot.md) | building from source, how the code is laid out over NetBSD's tree, booting over TFTP, the test bench |
| [docs/drivers.md](docs/drivers.md) | the NetBSD driver pattern, then every driver: platform and SMP, serial, clocks, USB, GPIO, Ethernet, switch |
| [docs/kernel-config.md](docs/kernel-config.md) | `FRITZ7530`, a kernel configuration for this board alone (3.9 MB instead of 10.2 MB); profiling with `tprof` |
| [docs/flash-edition.md](docs/flash-edition.md) | the system in the flash: the image, the root file system, the boot script, how settings are kept |
| [docs/nand.md](docs/nand.md) | the NAND controller and UBI, written for NetBSD |

## What works

| | |
|---|---|
| 4 cores (SMP), GIC, generic timer, 256 MB | `ipq4019_platform.c`; secondary cores started through the secure monitor (SCM) |
| Serial console | `qcom_uart.c`: UARTDM, interrupt driven |
| Reboot, watchdog | `qcom_pshold.c` (PS_HOLD), `qcom_wdt.c` (KPSS watchdog) |
| Clocks and resets, CPU frequency | `qcom_gcc.c`: resets, 43 clock gates, the CPU clock (48/200/500/716 MHz via `cpufreqdt`) |
| GPIO, buttons, LEDs | `qcom_tlmm.c`; NetBSD's `gpiokeys` (buttons to `powerd`) and `gpioleds` |
| Hardware random numbers | `qcom_rng.c` |
| USB 3 | NetBSD's `dwc3`/`xhci`/`umass` with `qcom_dwc3.c` (glue) and `qcom_usbphy.c` |
| Ethernet | `ipq4019_edma.c`: the ESS EDMA engine, 4 receive queues on 4 CPUs (RSS), 4 transmit queues, checksum offload, hardware VLAN tagging |
| Switch | `ipq4019_ess.c`: reset, set-up from scratch, link state, MIB counters, one port as WAN through 802.1Q |
| NAND | `qpic_nand.c` + `dev/ubi/ubi_lite.c`: UBI volumes as character devices; the settings are kept in one |

Routing between two switch ports (iperf3, TCP): about 600 Mbit/s with one flow and
900 Mbit/s (line rate) with four. OpenWrt on the same box does 599 and about 930.

Not done: WiFi (NetBSD's 802.11 stack lacks 802.11n/ac), DSL, DECT, one network
interface per switch port (Linux's DSA).

## Building

```
tools/build.sh                         # GENERIC kernel (first run: NetBSD source and toolchain, ~20 min)
KERNEL=FRITZ7530 tools/build.sh        # the board kernel, root on a USB stick
KERNEL=FRITZ7530_FLASH tools/build.sh  # the flash edition kernel
SETS=1 tools/mkflash.sh                # the flash edition image, tftp/netbsd-flash.itb
```

Details in [docs/build-and-boot.md](docs/build-and-boot.md). The GitHub Actions workflow
(`.github/workflows/build.yml`) builds the same images on every push and publishes them
as a release for every `v*` tag.

## Layout

| | |
|---|---|
| `sys/` | new files, at their place in NetBSD's tree (`sys/arch/arm/qcom/`, `sys/dev/ubi/`, the board device tree, kernel configurations) |
| `patches/` | changes to stock NetBSD files |
| `flash/` | the flash edition's root file system: program list, `/etc`, builders |
| `tools/` | build, boot, test and serial console helpers |
| `prepare/` | installing U-Boot and OpenWrt (scripts only; firmware is not included) |
| `docs/` | the documents above |

## License

BSD 2-clause for the code in this repository (see the file headers and `LICENSE`).
The board device tree includes NetBSD's copy of the Linux `qcom-ipq4019.dtsi`.
`prepare/scripts/eva_ramboot.py` comes from OpenWrt (GPL-2.0).
