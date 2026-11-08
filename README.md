# NetBSD on the AVM FRITZ!Box 7530

A learning port of NetBSD 11 to the AVM FRITZ!Box 7530, a router built around the
Qualcomm IPQ4019 (four Cortex-A7 cores at 716 MHz, 256 MB RAM, 128 MB NAND, a 5-port
gigabit switch). The goal was a NetBSD that does roughly what OpenWrt does on this box
(DSL and WiFi aside), and to understand every piece on the way: the drivers are written
from the hardware's register descriptions, with Linux and FreeBSD drivers read for
reference, and each one is explained in [docs/](docs/).

It is not an upstream submission. The repository is an overlay on the NetBSD source tree:
new files under `sys/` (same paths as in NetBSD's `src/sys`), a few small patches to stock
files in `patches/`, and scripts that fetch `netbsd-11`, apply both and build.

## What works

| | |
|---|---|
| 4 cores (SMP), GIC, generic timer, 256 MB | `ipq4019_platform.c`; secondary cores started through the secure monitor (SCM) |
| Serial console | `qcom_uart.c`: UARTDM, interrupt driven (RX in single-character mode, batched TX) |
| Reboot, watchdog | `qcom_pshold.c` (PS_HOLD), `qcom_wdt.c` (KPSS watchdog) |
| Clocks and resets, CPU frequency | `qcom_gcc.c`: all resets, 43 clock gates, the CPU clock (48/200/500/716 MHz via `cpufreqdt`) |
| GPIO, buttons, LEDs | `qcom_tlmm.c`; NetBSD's `gpiokeys` (buttons to `powerd`) and `gpioleds` (`sysctl hw.led`) |
| Hardware random numbers | `qcom_rng.c` |
| USB 3 | NetBSD's `dwc3`/`xhci`/`umass` with `qcom_dwc3.c` (glue) and `qcom_usbphy.c` |
| Ethernet | `ipq4019_edma.c`: the ESS EDMA engine, 4 RSS receive queues on 4 CPUs, 4 transmit queues, zero copy, checksum offload, hardware VLAN tagging |
| Switch | `ipq4019_ess.c`: ESS reset that keeps the PSGMII link aligned, switch set up from scratch, link state, MIB counters, one port as WAN through 802.1Q |
| NAND | `qpic_nand.c` + `dev/ubi/ubi_lite.c`: the QPIC controller driven by register writes, UBI volumes as character devices; the volume `config` is writable and keeps the settings |
| Flash edition | NetBSD boots from the NAND (a UBI volume U-Boot already knows) and keeps its settings there: no PC or USB stick needed |

Routing between two switch ports (iperf3): about 590 Mbit/s with one TCP flow and
800-900 Mbit/s with four or eight; stock OpenWrt on the same box does 599 and about 930.

Not done: DSL (not wanted), WiFi (NetBSD's 802.11 stack lacks 802.11n/ac; to be tried on
FreeBSD), one network interface per switch port (DSA).

## Steps

| | | |
|---|---|---|
| 0 | Install U-Boot (through OpenWrt's installation method) | [docs/uboot.md](docs/uboot.md) |
| 1 | Build NetBSD, boot it over TFTP, root on a USB stick; test bench and automated tests | [docs/build-and-boot.md](docs/build-and-boot.md) |
| 2-3 | The drivers, one by one | [docs/drivers.md](docs/drivers.md) |
| 5 | `FRITZ7530`, a kernel configuration for this board (3.9 MB instead of 10.2 MB) | [docs/kernel-config.md](docs/kernel-config.md) |
| 6 | The flash edition: kernel and root file system in one 14.5 MB image in a UBI volume | [docs/flash-edition.md](docs/flash-edition.md) |
| 7 | NAND and UBI from NetBSD | [docs/nand.md](docs/nand.md) |

## Quick start

```
tools/build.sh                         # GENERIC kernel, tftp/netbsd*.itb (first run: toolchain, ~20 min)
KERNEL=FRITZ7530 tools/build.sh        # the small board kernel
KERNEL=FRITZ7530_FLASH tools/build.sh  # the flash edition kernel (see docs/flash-edition.md)
```

The GitHub Actions workflow (`.github/workflows/build.yml`) builds the same images,
including the flash edition, and attaches them to the run.

Booting needs the U-Boot of step 0 and a serial console (3.3 V, 115200 8N1).

## Layout

| | |
|---|---|
| `sys/` | new files, at their place in NetBSD's tree (`sys/arch/arm/qcom/`, `sys/dev/ubi/`, the board device tree, kernel configurations) |
| `patches/` | changes to stock NetBSD files |
| `flash/` | the flash edition's root file system: program list, `/etc`, builders |
| `tools/` | build, boot, test and serial console helpers |
| `prepare/` | step 0: installing U-Boot and OpenWrt (scripts only; firmware is not included) |
| `docs/` | explanations |

## License

BSD 2-clause for the code in this repository (see the file headers and `LICENSE`).
The board device tree includes NetBSD's copy of the Linux `qcom-ipq4019.dtsi`.
`prepare/scripts/eva_ramboot.py` comes from OpenWrt (GPL-2.0).
