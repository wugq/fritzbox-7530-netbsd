# Chapter 6. How the Drivers Work

## 6.1. Synopsis

First the patterns every NetBSD driver follows, then each driver. Keep this open next to
the code in `sys/arch/arm/qcom/`.

Several drivers were ported from FreeBSD's IPQ4018 support (`qcom_gcc`, `qcom_tlmm`,
`qcom_rnd`, `ar40xx`, the USB PHYs) or follow Linux (`qcom_nandc`, `ipqess`, `msm_serial`,
`gcc-ipq4019`); Linux code was read for the hardware behaviour, not copied.

After reading this chapter, you will know:

- how a NetBSD driver finds its hardware through the device tree, and how it touches registers;
- how the secondary CPUs are started without PSCI;
- how each driver of the port works: serial console, clocks and resets, watchdog, random numbers, USB, GPIO, Ethernet and the switch;
- what went wrong on the way, driver by driver.

Before reading this chapter, you should:

- have read [Chapter 5](building-and-testing.md), at least how the code is organised and the device tree;
- be able to read C.

## 6.2. The NetBSD Driver Pattern

### From the device tree to a driver

At boot, U-Boot hands the kernel a device tree (DTB). NetBSD's FDT bus takes the nodes one
by one and asks each driver whether it is theirs:

```c
static const struct device_compatible_entry compat_data[] = {
	{ .compat = "qcom,kpss-wdt" },
	DEVICE_COMPAT_EOL
};

static int
qcom_wdt_match(device_t parent, cfdata_t cf, void *aux)
{
	struct fdt_attach_args * const faa = aux;
	return of_compatible_match(faa->faa_phandle, compat_data);
}
```

A non-zero `match` means "mine", and the kernel calls `attach`, which usually:
1. reads the register range from the device tree (`fdtbus_get_reg`) and maps it
   (`bus_space_map`);
2. reads other properties (interrupts, clocks, resets, ...);
3. sets up the hardware and registers with its subsystem (tty, network, watchdog, ...).

`CFATTACH_DECL_NEW(name, softc size, match, attach, detach, activate)` registers these
with autoconfiguration. The `softc` is the per-instance structure holding all the
driver's state.

### Two interfaces to the hardware

- **`bus_space`**: device registers. `bus_space_read_4(tag, handle, offset)`; drivers wrap
  it as `RD4(sc, reg)` / `WR4(sc, reg, val)`.
- **`bus_dma`**: memory the device reads or writes by itself (DMA):
  - `bus_dmamem_alloc` + `bus_dmamem_map`: memory the device can reach;
  - `bus_dmamap_load`: the **physical address** to give the device;
  - `bus_dmamap_sync`: **the CPU cache**. `PREWRITE` before the device reads what the CPU
    wrote (write the cache back), `POSTREAD` before the CPU reads what the device wrote
    (drop stale cache lines). A missing sync gives stale data or garbage, intermittently:
    the hardest kind of bug.

### Building it in

`files.qcom` declares devices and source files; a kernel configuration line
`xxx* at fdt?` attaches them (see [building-and-testing.md](building-and-testing.md)).

## 6.3. Platform Code: `ipq4019_platform.c`

Before any driver exists, the kernel needs the SoC's basics, registered with
`FDT_PLATFORM(ipq4019, "qcom,ipq4019", &ipq4019_platform)` and picked when the device
tree's root `compatible` has `qcom,ipq4019`:

- **`fp_devmap`**: mappings needed earliest: the UART (early console) and the APSS region
  (GIC, and the ACC/SAW blocks used to start the other cores).
- **`fp_mpstart`**: start the other CPUs.
- No `fp_reset`: reboot goes through the PS_HOLD driver (NetBSD calls
  `fdtbus_power_reset()` first); the board has no PSCI firmware.

### Starting the secondary cores (SMP)

At power-on only CPU0 runs; CPUs 1-3 are powered off. Each CPU node says
`enable-method = "qcom,kpss-acc-v2"`, so NetBSD calls the function registered under that
name with `ARM_CPU_METHOD`, `cpu_enable_qcom_kpss_acc_v2()`. Following Linux's
`arch/arm/mach-qcom/platsmp.c`, it:

1. **tells the secure firmware where the cores start**: Qualcomm's QSEE runs in
   TrustZone; a core powering up enters the secure world first and the firmware jumps to
   the address we gave. One SCM call does it: `ipq4019_scm_call()`, a single `smc #0`
   with the legacy convention (r0 = 0x401222, service "boot", command "set address";
   r2 = cold-boot flags of CPUs 1-3; r3 = physical address of `cpu_mpstart`). Once.
2. **powers the core up** through its ACC registers: power switch (BHS), LDO bypass, then
   clamps and resets released in order, with Linux's delays. Wrong order or delays and the
   core stays dead. A value must also go into the **L2 cache's SAW** (not the CPU's own).
3. **sends an SGI** to wake the core waiting in WFI so it reads its start address.

## 6.4. Serial Console: `qcom_uart.c` (`qcom,msm-uartdm-v1.4`)

Qualcomm's UARTDM is not a 16550. The driver is interrupt driven: receive as FreeBSD's
`uart_dev_msm.c`, transmit as Linux's `msm_serial.c`.

### Receive: single-character mode

By default UARTDM packs received bytes **four to a 32-bit FIFO word**; a partial word stays
in a packing register until a **stale event** (the line idle for a moment) pushes it out
and `RX_TOTAL_SNAP` tells how many bytes the transfer had. The first, polled driver read
that way and met all its traps:
- reading words before the stale event gave NULs and reordered bytes;
- forcing a stale event with no data stopped reception for good;
- a burst of 256 characters or more (the FIFO is 64 words) overran before the idle gap,
  no stale event ever came again, and **the console was deaf until power-off**.

FreeBSD sidesteps all of it: `DMEN`'s **RX single-character mode** (`RX_SC_ENABLE`) gives
every character its own FIFO word: no packing, no stale events, no `RX_TOTAL_SNAP`. With
the receive watermark `RFWR` at 0, an **RXLEV interrupt** comes as soon as the FIFO holds
a character.

```
character arrives -> RXLEV interrupt -> qcom_uart_intr: drain the FIFO into a 1 KB ring,
                                        schedule the soft interrupt
                     qcom_uart_softintr: hand the characters to the tty layer (l_rint)
```

Two levels because the hard interrupt must be short, and tty functions cannot be called
at the UART's interrupt priority (IPL_SERIAL).

### Transmit: batches and a watermark interrupt

The polled driver busy-waited for every character (87 µs each at 115200: 90 ms of a
spinning CPU per kilobyte). Now:
- a batch is up to 128 characters: the count into `NCF_TX`, then the characters four to a
  word into the TX FIFO (`TF`);
- the TX watermark `TFWR` is 16 words: when the FIFO drains below it, a **TXLEV
  interrupt** puts in the next batch;
- when everything is in, TXLEV goes off and the soft interrupt clears the tty's `TS_BUSY`
  and lets the tty layer send more.

**Locking**: a tty's output queue (clist) belongs to the tty layer and may only be touched
with the global `tty_lock` held. The first version called `l_start` from its soft
interrupt without it; `jot 5000` panicked half-way in `memcpy` (another CPU's writer
changed the queue at the same time). Taking `tty_lock` around `l_start`, as `ttwrite()`
does around `ttstart()`, fixed it.

### The console and the lock

Kernel output (`printf`, ddb) goes through `cnputc`, one character at a time, busy
waiting. It shares a **spin lock** with the batches (`__cpu_simple_lock`: a kmutex is not
usable at the first printf). A batch is written under the lock in one go, so the two never
mix within a transfer; when the tty prints while the kernel prints, characters interleave,
as with any shared serial line. `cnputc` takes the lock with a timeout, so a panic or ddb
still prints if a stuck CPU holds it. A once-a-second backstop calls the interrupt handler,
in case an interrupt is ever lost.

| test | polled driver | interrupt driver |
|---|---|---|
| a 256 / 1000-character line in one burst | console dead, power-off needed | received and executed |
| `jot 5000` | works (busy wait per character) | 5000 lines, complete and in order, 2.7 s (line rate ~2.1 s) |

## 6.5. Reset, Watchdog, Clocks, Random Numbers

### `qcom_pshold.c` (`qcom,pshold`)

PS_HOLD is the SoC's "keep me powered" signal to the power management chip. Writing 0
drops it and the board resets. The driver registers as a power controller
(`fdtbus_register_power_controller`), which `reboot` ends up calling.

### `qcom_wdt.c` (`qcom,kpss-wdt`)

The hardware watchdog: a counter clocked at 32768 Hz that resets the board when it
reaches `BITE_TIME`. The driver sets 60 s and a callout pets it every 5 s (`WDT_RST`). If
the whole kernel hangs, the board reboots by itself after a minute. It only catches a
whole-kernel hang: with some CPUs still running callouts it never fires.

### `qcom_gcc.c` (`qcom,gcc-ipq4019`): clock and reset controller

The GCC (Global Clock Controller) owns the clock tree (PLLs, dividers, muxes, gates) and the
blocks' resets. Other drivers find it through the device tree:

```
resets = <&gcc ESS_RESET>;     ->  fdtbus_reset_get(phandle, "ess")
clocks = <&gcc GCC_ESS_CLK>;   ->  fdtbus_clock_get(phandle, "ess"), clk_enable()
```

- **Resets**: all of them; each is one bit in a GCC register (1 = in reset). Table from
  FreeBSD.
- **Gates** ("branch" clocks, the leaves of the tree): an enable bit, either in the clock's
  own control register (CBCR) or in a shared "vote" register (several masters share the
  clock: it runs while anyone votes), and a status bit (CBCR bit 31, CLK_OFF). Enable: set
  the bit, wait for CLK_OFF to clear. `disable` does nothing: without reference counts
  across drivers and firmware, switching a clock off is riskier than leaving it on.
- Written after Amlogic's `meson_resets.c` / `meson_clk.c`: the reset controller
  implements `acquire/assert/deassert`, the clock controller `decode` (device tree number
  to `struct clk`) and a set of `clk_funcs`. It attaches at `pass 1`, before the drivers
  that need it.

**The CPU clock and frequency scaling** (as Linux's `gcc-ipq4019.c`). `GCC_APPS_CLK_SRC`
is an RCG (root clock generator: a glitch-free source mux and a divider):

| source (`CFG_RCGR` bits 10:8) | frequency |
|---|---|
| 0 XO (crystal) | 48 MHz |
| 1 DDRPLLAPSS (the CPU PLL) | 716 MHz (PLL post divider 3) |
| 2 FEPLL500 | 500 MHz |
| 3 FEPLL200 | 200 MHz |

- Reading: which source; for the PLL, the reference, feedback and post dividers at
  `0x2e020` (the post divider is non-linear, with half steps).
- Setting: write `CFG_RCGR` (source, divider), set `CMD_RCGR`'s UPDATE, wait for it to clear.
  Before changing the PLL's post divider the CPUs are parked on FEPLL500 (Linux's "safe
  parent"): the divider change has no status bit to wait for.
- The device tree's OPP table (48/200/500/716 MHz) goes to NetBSD's own `cpufreqdt`, after
  patch 0004 made `opp-microvolt` optional (there is no CPU regulator).

```
sysctl machdep.cpufreq.cpu0.available      # 716 500 200 48
sysctl -w machdep.cpufreq.cpu0.target=200   # one clock for the 4 cores
```

At attach the driver also measures the CPU clock (cycle counter against 20 ms of the
48 MHz generic timer) and prints it next to the computed one:
`qcomgcc0: CPU clock: 716 MHz (measured), 716 MHz (from the registers)`. A CPU-bound shell
loop takes 0.83 / 1.20 / 3.05 / 12.61 s at 716 / 500 / 200 / 48 MHz. 716 MHz is the
maximum (U-Boot's setting), so scaling only saves power.

### `qcom_rng.c` (`qcom,prng`): hardware random numbers

The kernel needs entropy (ssh keys, TCP sequence numbers); a router has no keyboard or
mouse, and every boot used to say "system needs entropy". The IPQ4019's PRNG becomes an
entropy source (`rndsource`): when the pool wants data, the callback reads 32-bit words
from `PRNG_DATA_OUT`. Set-up from FreeBSD's `qcom_rnd`, the entropy glue as NetBSD's
`meson_rng.c`. Its clock (`GCC_PRNG_AHB_CLK`) is enabled through the GCC first: reading a
peripheral whose clock is off can hang the bus. Result: `entropy: ready` at attach.

## 6.6. USB: `qcom_dwc3.c` and `qcom_usbphy.c`

The USB 3 controller is a Synopsys DWC3, which NetBSD supports (`dwc3_fdt` + `xhci`). On
Qualcomm chips the device tree wraps it in a glue node:

```
usb3@8af8800 { compatible = "qcom,dwc3"; ...        <- Qualcomm's glue
	dwc3@8a00000 { compatible = "snps,dwc3"; ... }  <- the controller
};
```

The FDT bus only offers a node to drivers below its **parent**; with nobody claiming
`qcom,dwc3`, the controller is never seen. `qcom_dwc3.c` claims the glue node and calls
`fdt_add_bus()` to attach the children as a bus.

Two more things are needed:
- **Power**: the port's 5 V switch is GPIO 49, turned on at boot by the TLMM driver from a
  `gpio-hog` in the device tree.
- **PHYs**: `qcom_usbphy.c` (`qcom,usb-hs-ipq4019-phy`, `qcom,usb-ss-ipq4019-phy`). Bringing
  a PHY up is a reset sequence through the GCC: assert, wait, release. `dwc3_fdt` calls
  `fdtbus_phy_enable()` for every PHY in its `phys`.

Before these drivers, both had to be poked from U-Boot with `mw.l`.

## 6.7. GPIO: `qcom_tlmm.c` (`qcom,ipq4019-pinctrl`)

The TLMM owns the chip's 100 pins, each with a 4 KB register page: `+0` configuration
(function, pull, drive strength, output enable), `+4` level (bit 0 input, bit 1 output).
The driver provides:

- **GPIOs for other drivers**: `gpios = <&tlmm 42 GPIO_ACTIVE_LOW>` in the device tree,
  `fdtbus_gpio_acquire()` in the driver. NetBSD's generic `gpiokeys` and `gpioleds` use
  this, so buttons and LEDs need no driver of their own, only device tree nodes:
  - buttons: `gpiokeys` polls them and reports presses to `powerd`, which runs
    `/etc/powerd/scripts/hotkey_button wlan pressed` and the like. From top to bottom on
    the case: **WLAN (GPIO 42), DECT (GPIO 43), WPS (GPIO 41)**. `powerd` is off by default
    (`powerd=YES` in `/etc/rc.conf`); `powerd -d` prints every event.
  - LEDs: `sysctl -w hw.led.power=1` / `=0`. Without `default-state` in the device tree,
    NetBSD keeps an LED as it was at attach, which on this board is on.
- **`gpio-hog`** children: pins put in a fixed state at attach (the USB power switch).
- **a `gpiobus`** for `gpioctl gpio0 ...`. At securelevel 1 (NetBSD's default), only pins
  configured in `/etc/gpio.conf` before boot can be used from userland ("Operation not
  permitted" otherwise), by design.

Pin function selection is not implemented (the firmware set it up), nor GPIO interrupts
(polling is enough for buttons).

## 6.8. Ethernet: `ipq4019_edma.c` (`qcom,ipq4019-ess-edma`)

The biggest driver, and the most worth reading.

### The hardware

```
           CPU / memory
               |  (DMA)
        +------+------+
        |    EDMA     |  16 TX queues, 8 RX queues, a descriptor ring each
        +------+------+
               | port 0
        +------+------+
        | ESS switch  |  a QCA8337-class switch core, 6 ports
        +--+--+--+--+-+
           |  |  |  |   PSGMII (one fast serial link carrying several ports)
        +--+--+--+--+-+
        | QCA8075 PHY |  external PHY chip, the LAN jacks
        +-------------+
```

NetBSD's `qcomedma0` is the EDMA plus switch port 0. A frame from a jack goes PHY ->
PSGMII -> switch -> EDMA -> memory. Traffic between two LAN ports that the CPU need not
see is switched in hardware (line rate, 934 Mbit/s).

### Descriptors and rings

EDMA and the driver talk through **descriptor rings**, arrays used circularly:
- **TPD** (transmit, 16 bytes): length, flags (checksum offload, end of packet, ...), the
  data's physical address, target ports;
- **RFD** (receive free, 4 bytes): the address of an empty buffer;
- **RRD** (receive return, 16 bytes): written by the hardware **at the start of the
  buffer**: length, checksum status, valid bit, VLAN tag.

Each ring has a producer and a consumer index. On transmit the driver produces (fills
TPDs, writes the producer index register) and the hardware consumes; on receive the
driver produces empty buffers and the hardware consumes them as frames arrive.

### Set-up

The ESS reset and the switch set-up belong to the switch driver (section 7), which
attaches earlier (`pass 4`). EDMA then finds a freshly reset engine and only allocates
its rings (`bus_dmamem_alloc`), writes their base addresses, and sets up RSS, queues and
interrupts. The reset matters: U-Boot used the network for TFTP, and without a reset the
engine **ignores new ring base addresses** (the very first driver had to reuse U-Boot's
16- and 8-entry rings).

### Receive

```
frame arrives -> hardware fills a buffer, writes the RRD, advances the consumer, interrupts
   |
edma_intr (hard interrupt, short): mask this queue's interrupt, wake this CPU's RX thread
   |
edma_rx_work (workqueue thread) -> edma_rx:
   for each new frame: read the RRD, put a fresh mbuf into the RFD, pass the frame up
   all done: unmask the interrupt; more left: queue itself again
```

Each design point is a lesson:

- **Zero copy**: the hardware writes into mbuf clusters; the full one goes up the stack
  and a fresh one takes its place.
- **A thread, not the hard interrupt**: receiving in the interrupt kept feeding the
  protocol queues, `ipintr()` never caught up, the heartbeat soft interrupt starved and
  the kernel panicked. A thread runs below all soft interrupts, so the stack always
  finishes its work first (Linux calls this NAPI).
- **`rq_queued`**: a work item must never be queued twice, or the workqueue's list becomes
  a loop and the thread spins forever (the "hang under load" bug). The thread drops the
  queue lock while working, so the flag is checked before re-queueing.
- **RSS, 4 RX queues**: the hardware hashes the TCP/UDP 4-tuple and spreads flows over 4
  queues, each interrupt bound to its own CPU. Hashing addresses only (2-tuple) puts all
  flows between two hosts into one queue.
- **The backstop callout never receives on another CPU**: a flow must stay on one CPU, or
  its frames leave through different TX queues, get reordered, and TCP retransmits. The
  callout only sends work back to the queue's CPU.

### Transmit

- **One TX queue per CPU** (`if_transmit`): a forwarded frame leaves from the CPU it
  arrived on, without lock contention.
- **Zero copy for large frames, copies for small ones**: above 256 bytes the mbuf is
  mapped (`bus_dmamap_load_mbuf`); small frames (TCP ACKs) are copied into a prepared
  buffer, cheaper than a DMA mapping.
- **Checksum offload**: the stack leaves the pseudo-header sum; the TPD tells the hardware
  where to start summing and where to store the result.
- **No register reads on the fast path**: a register read waits for a bus round trip. The
  producer index is written, not read-modified-written; finished descriptors are
  reclaimed only when the ring is nearly full.
- **Sync only what was written**: descriptors live in uncached memory, synced with
  `PREWRITE` before the doorbell, **only the descriptors just written**; syncing the whole
  32 KB ring cost 4000 cycles per frame.
- **VLAN tags in hardware** (`ETHERCAP_VLAN_HWTAGGING`, as Linux's ipqess): on transmit
  the tag from the mbuf goes into TPD word 3 (bit 17 = insert, low 16 bits = tag); on
  receive `RXQ_CTRL`'s RMV_VLAN strips it into the RRD (word 4; word 7 bit 0 says a tag
  was there) and the driver hands it up with `vlan_set_tag()`.
- **TSO**: written, but the engine sends nothing for LSO descriptors; off by default.

### Debugging aids

| | |
|---|---|
| `sysctl hw.qcomedma0.dump=1` | EDMA and queue state on the console |
| `vmstat -e \| grep qcomedma` | per-queue counters: frames, interrupts, thread runs, idle runs, ... |
| `sysctl hw.qcomedma0.watch=N` | one line of queue state every N seconds (kernel printf: works on a half-hung system) |
| `sysctl hw.qcomedma0.prof=1` | print and clear per-CPU cycle accounting: RX, TX, lock waits |
| `sysctl hw.qcomedma0.rss_type` | which headers the RSS hash covers |

## 6.9. Switch: `ipq4019_ess.c` (`qca,ipq4019-qca8337n`)

Written after FreeBSD's `ar40xx(4)`. The EDMA driver handles the DMA engine; everything
on the switch and PHY side is here.

### ESS reset without breaking PSGMII

The ESS reset (switch + EDMA, `resets = <&gcc ESS_RESET>` on the switch node) **has to be
coordinated with the PHY**, or a PSGMII lane occasionally comes back misaligned: link up,
no frames, about one boot in three:

1. reset the PHY's end of the PSGMII link over MDIO, wait for its PLL;
2. freeze the PHY's receive clock recovery (CDR);
3. reset the ESS;
4. wait for the ESS end's PSGMII calibration;
5. release the CDR.

As FreeBSD's `ar40xx_hw_malibu_psgmii_ess_reset()`. MDIO, the CPU's bus to the PHY
registers, is a few functions here (`ess_mdio_read/write`, and `ess_mmd_read` for MMD
registers through clause-22 registers 13/14).

### Setting the switch up from scratch

The first version saved U-Boot's switch registers before the reset and wrote them back.
Now the driver programs them, as `ar40xx_attach()`:

| setting | registers | |
|---|---|---|
| PSGMII mode | PSGMII block 0x1b4, 0x288 | `ar40xx_hw_psgmii_set_mac_mode()` |
| ports 1-5 | `PORT_STATUS` | auto link: the switch reads speed/duplex from the PHY and enables the MAC once the link is up |
| all ports | `PORT_HEADER`, `PORT_VLAN0/1` | no Atheros header; VLAN tags untouched |
| forwarding | `FWD_CTRL0/1` | CPU port on; unknown unicast, multicast, broadcast flooded everywhere |
| MIB | `MODULE_EN` | **must be on**: without it about 70% of the frames were lost |
| EEE | `EEE_CTRL` = 0 | 802.3az off, as FreeBSD and OpenWrt |
| port lookup | `PORT_LOOKUP` | "port-based VLAN": where each port may send; normally all of 0-5, with `isolate=1` ports 1-5 only to the CPU |
| CPU port 0 | `PORT_STATUS(0)` | fixed 1000/full with flow control, then MAC on |

Not taken from FreeBSD: jumbo frames (EDMA's receive buffers are 2 KB); the max frame is
1522 (room for a VLAN tag). Not done: the PSGMII self-test (the reset above was enough).
With `boot -v` the driver lists the registers that differ from U-Boot's, for comparison.

### Link state

Once a second the driver reads `PORT_STATUS` of ports 1-5. On a change it logs
(`qcomess0: port 3: link up 1000FD`) and flushes the port's learned addresses (ATU).
`sysctl hw.qcomess0.ports` shows e.g. `1:down 2:down 3:1000FD 4:1000FD 5:down`.

### VLANs and a WAN port

All four jacks are LAN (the 7530's WAN is DSL, which is a modem chip, not Ethernet). To
route, one jack can become the WAN:

```
sysctl -w hw.qcomess0.wan_port=3        # switch port 3 is the WAN
ifconfig vlan2 create
ifconfig vlan2 vlan 2 vlanif qcomedma0  # the WAN interface
ifconfig vlan2 192.0.2.1/24 up          # or: dhcpcd vlan2
```

- `wan_port=0` (default): port-based VLANs only, one segment, tags untouched.
- `wan_port=N`: 802.1Q, two VLAN table (VTU) entries:
  - **VLAN 1**: the CPU and the other ports, **all untagged**: `qcomedma0` stays the LAN;
  - **VLAN 2**: port N untagged, **the CPU tagged**: the WAN frames reach NetBSD tagged,
    for `vlan2`;
  - each port's default VLAN (PVID) is its VLAN; ingress "secure" drops frames tagged
    with a VLAN the port is not in.

This is how FreeBSD's `ar40xx` and OpenWrt's old swconfig did it: one link between switch
and CPU, the tag tells LAN from WAN. Seen from the front, the jack farthest from the DSL
jack is switch port 4, the next one port 3.

Routing LAN <-> WAN: about 500 Mbit/s with one flow, 600-850 with four or eight, a little
below the untagged case; the per-frame cost of the VLAN path is small, the spread comes
from how RSS distributes the flows over the CPUs (see below).

### MIB counters

Each port has 39 counters (frames by size, bytes, broadcast/multicast, pause frames, CRC
errors, overflows, ...). Once a second the driver has the switch snapshot them
(`MIB_FUNC` = capture), reads them and adds the difference to NetBSD event counters:

```
vmstat -e | grep qcomess          # non-zero ones only
qcomess0 port4 TxPause    2756    # the switch sent 2756 pause frames to port 4's host
```

Directions are **the switch's**: port 0's Rx is what the CPU sent. Counters are kept on
read (`MIB_CPU_KEEP`, as Linux's qca8k); 32-bit wraps are handled modulo 2^32. The pause
counters showed that the switch throttles the sending host when the CPU cannot receive
faster: that, not the VLAN path, is the routing limit. The CPU runs at its maximum
(716 MHz); a saturated CPU handles about 35-40 thousand frames per second, around 20000
cycles per frame, of which the driver accounts for about 5600; the rest is the network
stack (`tprof` can show where).

### sysctl

| | |
|---|---|
| `hw.qcomess0.ports` | link state of ports 1-5 |
| `hw.qcomess0.wan_port=N` | port N becomes the WAN (VLAN 2), 0 = none |
| `hw.qcomess0.isolate=1` | ports 1-5 talk only to the CPU (routing tests) |
| `hw.qcomess0.dump=1` | the main switch registers on the console |

Not done: one network interface per port (like Linux's DSA).
