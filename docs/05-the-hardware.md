# Chapter 5. The Hardware

## 5.1. Synopsis

This chapter describes the machine the port runs on: the board, the system-on-chip, the
CPU cores and their caches, how memory is laid out, and, for every piece of hardware,
which NetBSD driver handles it and where that driver comes from.

After reading this chapter, you will know:

- what is on the FRITZ!Box 7530's board, and which parts this port uses;
- what the Qualcomm IPQ4019 contains, and how its parts are connected;
- what kind of CPU the Cortex-A7 is, and how it compares with other common ones;
- where things are in the physical address space;
- which driver serves which hardware block: written for this port, stock NetBSD, or
  stock with a patch.

Before reading this chapter, you should know what a driver is and what memory-mapped
registers are: a device appears to the CPU as a range of addresses, and reading or
writing them talks to the device.

## 5.2. The Board

```
                         FRITZ!Box 7530
  ┌───────────────────────────────────────────────────────────────┐
  │                                                               │
  │   ┌──────────────────────────┐       256 MB RAM               │
  │   │     Qualcomm IPQ4019     │──────                          │
  │   │  4 x Cortex-A7, 716 MHz  │       128 MB NAND (Macronix)   │
  │   │  switch, Wi-Fi, USB, ... │──────                          │
  │   └──┬──────────┬─────────┬──┘                                │
  │      │ PSGMII   │ PCIe    │ USB 3                             │
  │   ┌──┴────────┐ │      [USB port]     buttons, LEDs (GPIO)    │
  │   │ QCA8075   │ │                     serial header (UART)    │
  │   │ 5-port PHY│ └─ DSL chip           DECT chip               │
  │   └──┬─┬─┬─┬──┘                                               │
  │   [LAN 1-4]                           [DSL]   [phone]         │
  └───────────────────────────────────────────────────────────────┘
```

| part | chip | used by this port |
|---|---|---|
| system-on-chip | Qualcomm IPQ4019 | yes |
| RAM | 256 MB | yes |
| flash | 128 MB NAND, Macronix (ID `c2 f1 80 95`, MX30LF1G18AC class): 2 KB pages, 128 KB blocks | yes ([Chapter 10](10-nand-and-ubi.md)) |
| Ethernet PHY | Qualcomm QCA8075, five ports on one PSGMII link | yes, four of its ports are the LAN jacks |
| USB | one USB 3 port, from the SoC | yes |
| buttons, LEDs | on SoC GPIO pins | yes |
| serial console | 4-pin header, 3.3 V, from the SoC's first UART | yes |
| Wi-Fi | the SoC's two built-in radios (2.4 and 5 GHz) | no |
| DSL | a separate DSL chip on the SoC's PCIe bus | no |
| DECT | Dialog SC14448 | no |

The 7530 has no dedicated Ethernet WAN port: in the stock firmware, the internet comes
through DSL. This port turns one of the four LAN jacks into a WAN port instead
([Section 7.9](07-how-the-drivers-work.md#79-switch-ipq4019_essc-qcaipq4019-qca8337n)).

## 5.3. The System-on-Chip: Qualcomm IPQ4019

A **system-on-chip** (SoC) puts a computer's CPU and most of its controllers on one
chip. The IPQ4019 belongs to Qualcomm's IPQ40xx family (IPQ4018, IPQ4019, IPQ4028,
IPQ4029: the same CPU cores, with different packages and peripherals), made for Wi-Fi
access points and routers around 2015-2016.

```
   ┌──────────────────────────── IPQ4019 ─────────────────────────────┐
   │                                                                  │
   │  ┌─────────── CPU cluster ────────────┐   ┌──────────────────┐   │
   │  │ Cortex-A7  Cortex-A7  ×4           │   │ GIC (interrupts) │   │
   │  │ L1 32K+32K each                    │   │ GCC (clocks,     │   │
   │  │ shared L2 256K                     │   │      resets)     │   │
   │  │ timers, PMU per core               │   │ TLMM (GPIO pins) │   │
   │  └────────────────┬───────────────────┘   └──────────────────┘   │
   │                   │  on-chip interconnect                        │
   │   ┌───────┬───────┼────────┬─────────┬─────────┬─────────┐       │
   │   │       │       │        │         │         │         │       │
   │  ESS    QPIC    USB 3    UARTs     PRNG      Wi-Fi     PCIe      │
   │ EDMA +  NAND    USB 2    SPI/I2C   watchdog  2 radios  crypto    │
   │ switch          PHYs     (BLSP)    PS_HOLD                       │
   └───┬──────────────────────────────────────────────────────────────┘
       │ PSGMII → QCA8075 PHY
```

Every block is a set of registers at a fixed physical address. The device tree
(`qcom-ipq4019.dtsi` plus the board's `.dts`) lists them; NetBSD matches each node to a
driver by its `compatible` string ([Section 7.2](07-how-the-drivers-work.md#72-the-netbsd-driver-pattern)).

## 5.4. The CPU: Cortex-A7

NetBSD's boot messages describe each core:

```
cpu0 at cpus0: Cortex-A7 r0p5 (Cortex V7A core)
cpu0: L1 32KB/32B 2-way (512 set) VIPT Instruction cache
cpu0: L1 32KB/64B 4-way (128 set) write-back-locking-C PIPT Data cache
cpu0: L2 256KB/64B 8-way (512 set) write-through PIPT Unified cache
vfp0 at cpu0: NEON MPE (VFP 3.0+), rounding, NaN propagation, denormals
```

| | |
|---|---|
| architecture | ARMv7-A, 32-bit; revision r0p5 |
| cores | 4, sharing the L2 cache |
| clock | 48, 200, 500 or 716 MHz (`machdep.cpufreq`); U-Boot leaves it at 716 |
| pipeline | in-order, partially dual-issue: instructions run in program order, at most two at a time |
| L1 instruction cache | 32 KB per core, 32-byte lines |
| L1 data cache | 32 KB per core, **64-byte lines**, write-back |
| L2 cache | 256 KB, shared by the four cores, 64-byte lines |
| floating point and SIMD | VFPv4 and NEON |
| per core | a generic timer (48 MHz), a performance monitor unit (PMU) |
| security | TrustZone: Qualcomm's secure firmware (QSEE) runs beside the kernel |

What these mean for the code:

- **The 64-byte cache line** is the unit of everything the CPU does with memory: of the
  cache maintenance a driver must do for DMA, and of the ping-pong when two cores
  write the same line ([Counting Cycles](articles/counting-cycles.md)).
- **In-order execution**: the core cannot work around a slow load or a device register
  read by running later instructions in the meantime, so such stalls cost their full
  time.
- **The PMU** is what `tprof` samples with
  ([Profiling Packet Forwarding with tprof](articles/profiling-packet-forwarding-with-tprof.md)).
- **TrustZone**: some operations, such as setting where secondary cores start, must ask
  the secure firmware through a `smc` call ([Section 7.3](07-how-the-drivers-work.md#73-platform-code-ipq4019_platformc)).

## 5.5. How It Compares

The Cortex-A7 is ARM's small, power-efficient core of its generation. Some neighbours:

| core | architecture | execution | where you meet it |
|---|---|---|---|
| **Cortex-A7** | ARMv7-A, 32-bit | in-order, partially dual-issue | IPQ40xx routers; Raspberry Pi 2 (4 cores at 900 MHz); Allwinner H3 boards |
| Cortex-A9 | ARMv7-A, 32-bit | out-of-order | older high-end routers and boards (e.g. Marvell Armada 38x, NXP i.MX6) |
| Cortex-A53 | ARMv8-A, 64-bit (also runs 32-bit code) | in-order, dual-issue | Raspberry Pi 3; newer Qualcomm router SoCs (IPQ60xx, IPQ807x) |
| MIPS 1004Kc | MIPS32, 32-bit | in-order, two hardware threads per core | MediaTek MT7621, many low-cost routers |
| x86-64 (a PC) | 64-bit | out-of-order, wide | your PC: many times faster per core and per clock |

What it means in practice: a router SoC like this one cannot forward a gigabit of
traffic through a general-purpose network stack on one core. It gets there by spreading
work over its four cores and by avoiding per-packet work (zero copy, hardware checksums,
the switch forwarding LAN-to-LAN traffic without the CPU at all). The articles show how
close to the limit that is: forwarding one TCP flow keeps one core 100% busy at about
600 Mbit/s.

## 5.6. Physical Memory Map

The CPU sees one 32-bit physical address space. Device registers live at low
addresses, RAM starts at 0x80000000:

| address | what |
|---|---|
| 0x00022000 | PRNG (random numbers) |
| 0x00090000 | MDIO (management bus to the PHY) |
| 0x01000000 | TLMM (GPIO pins) |
| 0x01800000 | GCC (clocks and resets) |
| 0x06000000, 0x08a00000 | USB 2 and USB 3 controllers |
| 0x078af000 | UART 1, the console |
| 0x079b0000 | QPIC (NAND controller) |
| 0x0a000000, 0x0a800000 | Wi-Fi radios |
| 0x0b000000 | GIC (interrupt controller) |
| 0x0b017000 | watchdog |
| 0x0c000000 | ESS switch |
| 0x0c080000 | ESS EDMA (Ethernet DMA) |
| 0x40000000 | PCIe |
| **0x80000000** | **RAM, 256 MB, to 0x8fffffff** |
| 0x80000040 | the kernel is loaded here ([Section 6.4](06-building-and-testing.md#64-building)) |
| 0x85000000 | where U-Boot loads images (`tftpboot`, `ubi read`) |
| about 0x875f0000-0x88013000 | U-Boot itself, while it runs |
| 0x87e00000 | shared memory with the firmware (SMEM), reserved |
| 0x87e80000 | TrustZone firmware, reserved |

The two reserved ranges are marked `no-map` in the device tree, so the kernel never
uses them.

## 5.7. Hardware and Drivers

Every hardware block, the driver NetBSD attaches to it (as it appears in `dmesg`), and
where that driver comes from:

- **new**: written for this port, in `sys/arch/arm/qcom/` or `sys/dev/ubi/`;
- **stock**: NetBSD's own, unchanged;
- **patched**: NetBSD's own, with a change in `patches/`.

| hardware | compatible | NetBSD device | source | | described in |
|---|---|---|---|---|---|
| SoC start-up, secondary cores | `qcom,ipq4019` | (platform code) | `ipq4019_platform.c` | new | [7.3](07-how-the-drivers-work.md#73-platform-code-ipq4019_platformc) |
| Cortex-A7 cores | `arm,cortex-a7` | `cpu0`-`cpu3`, `vfp0`-`vfp3` | NetBSD arm | stock | |
| CPU frequency | `operating-points-v2` | `cpufreqdt0`-`3` | `cpufreq_dt.c` | patched (0004) | [7.5](07-how-the-drivers-work.md#75-reset-watchdog-clocks-random-numbers) |
| interrupt controller | `qcom,msm-qgic2` | `gic0`, `armgic0` | `gic.c` | patched (0003, 0005) | [the tprof article](articles/profiling-packet-forwarding-with-tprof.md) |
| generic timer | `arm,armv7-timer` | `gtmr0`, `armgtmr0` | NetBSD arm | stock | |
| performance monitor | `arm,cortex-a7-pmu` | `armpmu0` | NetBSD arm, `tprof` | patched (0006) | [8.5](08-board-kernel-configuration.md#85-profiling-with-tprof) |
| clocks and resets | `qcom,gcc-ipq4019` | `qcomgcc0` | `qcom_gcc.c` | new | [7.5](07-how-the-drivers-work.md#75-reset-watchdog-clocks-random-numbers) |
| reset (PS_HOLD) | `qcom,pshold` | `qcompshold0` | `qcom_pshold.c` | new | [7.5](07-how-the-drivers-work.md#75-reset-watchdog-clocks-random-numbers) |
| watchdog | `qcom,kpss-wdt` | `qcomwdt0` | `qcom_wdt.c` | new | [7.5](07-how-the-drivers-work.md#75-reset-watchdog-clocks-random-numbers) |
| random numbers | `qcom,prng` | `qcomrng0` | `qcom_rng.c` | new | [7.5](07-how-the-drivers-work.md#75-reset-watchdog-clocks-random-numbers) |
| GPIO pins | `qcom,ipq4019-pinctrl` | `qcomtlmm0`, `gpio0` | `qcom_tlmm.c` | new | [7.7](07-how-the-drivers-work.md#77-gpio-qcom_tlmmc-qcomipq4019-pinctrl) |
| buttons | `gpio-keys` | `gpiokeys0` | NetBSD | stock | [3.7](03-configuring-the-router.md#37-leds-and-buttons) |
| LEDs | `gpio-leds` | `gpioleds0` | NetBSD | stock | [3.7](03-configuring-the-router.md#37-leds-and-buttons) |
| serial console | `qcom,msm-uartdm-v1.4` | `qcomuart0` | `qcom_uart.c` | new | [7.4](07-how-the-drivers-work.md#74-serial-console-qcom_uartc-qcommsm-uartdm-v14) |
| USB PHYs | `qcom,usb-hs/ss-ipq4019-phy` | `qcomusbphy0`, `1` | `qcom_usbphy.c` | new | [7.6](07-how-the-drivers-work.md#76-usb-qcom_dwc3c-and-qcom_usbphyc) |
| USB 3 glue | `qcom,dwc3` | `qcomdwc0` | `qcom_dwc3.c` | new | [7.6](07-how-the-drivers-work.md#76-usb-qcom_dwc3c-and-qcom_usbphyc) |
| USB 3 controller | `snps,dwc3` | `xhci0`, `usb0`, `usb1` | NetBSD `dwc3`/`xhci` | stock | |
| USB disks | | `umass0`, `sd0` | NetBSD | stock | |
| Ethernet DMA | `qcom,ipq4019-ess-edma` | `qcomedma0` | `ipq4019_edma.c` | new | [7.8](07-how-the-drivers-work.md#78-ethernet-ipq4019_edmac-qcomipq4019-ess-edma) |
| switch, MDIO, PSGMII | `qca,ipq4019-qca8337n` | `qcomess0` | `ipq4019_ess.c` | new | [7.9](07-how-the-drivers-work.md#79-switch-ipq4019_essc-qcaipq4019-qca8337n) |
| NAND controller | `qcom,ipq4019-nand` | `qcomqpic0` | `qcom_qpic.c`, `qpic_nand.c` | new | [10](10-nand-and-ubi.md) |
| UBI volumes | | `/dev/ubivolN` | `ubi_lite.c` | new | [10](10-nand-and-ubi.md) |
| IP fast forwarding | | (`GATEWAY`) | `ip_flow.c` | patched (0007) | [the tprof article](articles/profiling-packet-forwarding-with-tprof.md) |

Not supported:

| hardware | compatible | why |
|---|---|---|
| Wi-Fi radios | `qcom,ipq4019-wifi` | needs an ath10k driver and firmware; NetBSD's 802.11 stack lacks 802.11n/ac |
| PCIe and the DSL chip | `qcom,pcie-ipq4019` | no driver for the DSL chip, so PCIe has no use here |
| crypto engine, BAM DMA | `qcom,crypto-v5.1`, `qcom,bam-v1.7.0` | not needed: ssh uses the CPU, the NAND driver avoids DMA |
| SPI, I²C, second UART | `qcom,spi-qup-*`, `qcom,i2c-qup-*` | nothing on this board uses them |
| core power controllers | `qcom,kpss-acc-v2`, `qcom,saw2` | used directly by the platform code to start the cores; `dmesg` lists them as "not configured" |
| DECT | | separate chip, no driver |

## 5.8. Where to Read More

- The board's device tree: `sys/arch/arm/dts/qcom-ipq4019-fritzbox-7530.dts`; the SoC's:
  `src/sys/external/gpl2/dts/dist/arch/arm/boot/dts/qcom-ipq4019.dtsi`.
- ARM, *Cortex-A7 MPCore Technical Reference Manual*; *ARM Architecture Reference
  Manual, ARMv7-A and ARMv7-R edition*.
- Linux's drivers for the IPQ40xx (`drivers/clk/qcom/gcc-ipq4019.c`,
  `drivers/net/ethernet/qualcomm/ipqess/`, ...), FreeBSD's `sys/arm/qualcomm/` and
  `sys/dev/qcom_*`: the references the drivers here were written against.
