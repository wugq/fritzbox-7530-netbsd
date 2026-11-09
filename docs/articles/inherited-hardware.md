# Inherited Hardware: When the Boot Loader Was There First

## Abstract

A driver book usually starts from hardware just out of reset. On a router, the kernel
inherits hardware that the boot loader has already used: U-Boot loaded the kernel over
the network and from the NAND, and left the Ethernet engine, the switch and the NAND
controller in whatever state it finished with. Five problems in this port came from that
inheritance:

- the switch did not forward any packet to the CPU;
- the Ethernet engine ignored the driver's rings and kept using U-Boot's;
- after resetting the switch, 70% of the packets were lost;
- after resetting the switch, one port in three boots was dead, at random;
- the NAND controller ignored every command the CPU wrote.

A sixth problem is the same idea applied to memory: loading a large file overwrote
U-Boot itself while it was running.

The article explains what state survives a hand-over between programs, what a reset
does and does not cover, how a high-speed serial link between two chips locks on, and
the general approach: **find out what the previous owner did, then either undo it or
reset and rebuild**.

What you should know: the descriptor rings of [Receiving Packets without
Drowning](receiving-packets.md), section 1. Basic idea of a register: a memory address
that controls a device.

## 1. Background: What Survives a Hand-Over

When U-Boot starts the kernel with `bootm`, it jumps to the kernel's first instruction.
Nothing resets the chip in between. Every device register keeps its value: clocks that
were on stay on, a DMA engine that was configured stays configured, a controller left in
a special mode stays in it.

The boot loader cleans up some of it (Linux expects certain things, such as interrupts
off and caches in a known state), but only what it was written to clean up, and only for
the kernel it was written for.

Two strategies exist for a driver:

1. **Undo**: find out exactly what the previous owner changed and set it back. Cheap, but
   it needs knowledge of the previous owner's code, and misses whatever you did not
   know about.
2. **Reset and rebuild**: put the device through a hardware reset and configure it from
   scratch. Thorough, but you must know everything the device needs, including what the
   previous owner had set up for you.

This port used both, and learned the limits of each.

On this chip, resets are controlled by the clock and reset controller (GCC): writing a
bit in a "block control register" (BCR) holds one block in reset. Each block has its
own; the Ethernet engine and switch (ESS) share one, the NAND controller (QPIC) has
another, and the external PHY chip has none of these at all.

## 2. No Packets Reach the CPU

### 2.1. Symptom

The first Ethernet driver set up its rings, and the switch reported links up, but not
a single packet arrived.

### 2.2. Cause

U-Boot's network driver calls `eth_halt()` after every transfer (after each
`tftpboot`). For the IPQ4019 that calls `ipq40xx_ess_disable_lookup()`, which **removes
the CPU port (port 0) from every port's forwarding mask** and from the flooding mask.
The switch kept switching between the external ports, but considered the CPU an
invalid destination.

### 2.3. Fix (Undo)

The driver wrote back the values U-Boot's `enable_lookup()` would have written: the port
lookup registers (0x660 + port × 0xc) and `GLOFW_CTRL1` (0x624). Packets arrived.

This is strategy 1. It worked, and it was fragile: the fix depended on knowing U-Boot's
code, and the next problem showed what else was left.

## 3. The Engine Keeps U-Boot's Rings

### 3.1. Symptom

Received packets showed up, but **in U-Boot's buffers**, not the driver's.

### 3.2. Cause

The Ethernet engine latches the base addresses of its rings when it starts. Without a
reset, it ignored the new addresses the driver wrote (the register meant to load them,
`LOAD_PTR`, had no effect) and kept using the 16-entry receive and 8-entry transmit
rings U-Boot had given it.

### 3.3. A Workaround, Then a Reset

The first workaround reused U-Boot's rings: same addresses, but pointing the descriptors
at the driver's buffers, with that memory reserved in the device tree so the kernel
would not hand it out. It worked, with tiny rings, and the first 15 or so packets after
boot were lost.

The real fix was strategy 2: **reset the whole ESS** (GCC register 0x1812008, bit 0).
After the reset, the engine accepted new ring addresses. But a reset clears the switch's
configuration too, which is what U-Boot had set up and the driver relied on.

## 4. The Snapshot That Missed One Register

### 4.1. A Shortcut

Instead of learning to configure the switch from nothing, the driver took a **snapshot**
of all switch registers (0x000-0xFFC) before the reset and wrote them back afterwards.
Registers with side effects were skipped: the one with the soft-reset bit, the MIB and
MDIO command registers, the ACL and address tables. One more was skipped by caution:
`MODULE_EN` (0x030).

### 4.2. Symptom

About **70% of the packets were lost**.

### 4.3. Clue

The switch's per-port statistics (MIB counters) were all **zero**, even for ports that
were clearly passing some traffic. Counters that never count are not "no traffic"; they
are "counting is off". The MIB enable bit lives in `MODULE_EN`, and so do other module
enables. Restoring that register fixed both.

Later the switch driver was rewritten to configure everything from scratch, following
FreeBSD's `ar40xx` driver, and the snapshot was dropped. The table of what it sets is in
[Section 6.9](../06-how-the-drivers-work.md#69-switch-ipq4019_essc-qcaipq4019-qca8337n).

> [!TIP]
> A value that is exactly zero, or exactly some reset value, is information. Here it
> pointed at the one register the snapshot had left out.

## 5. One Port in Three Boots Dead: PSGMII

### 5.1. Symptom

After the ESS reset, about one boot in three left **one port dead**, a different one each
time. The switch reported the link up (port status `0x1ffe`), but nothing went through:
the router's ARP requests never reached the machine on that port (a packet capture on it
showed nothing), or the PC could not ping the router.

### 5.2. Investigation

| step | finding |
|---|---|
| Is it the newest change? | an older kernel had done it too, and the change did not touch transmit |
| Random or systematic? | rebooting several times and testing both ports every time: random port, about one boot in three |
| How do mature drivers reset the ESS? | FreeBSD's `ar40xx` does not just pulse the reset: it runs a sequence involving the PHY chip |

The third step found it. When a problem is intermittent and nothing in your own code
explains it, the difference between your sequence and a working one is the best lead.

### 5.3. Background: A Serial Link Has to Lock On

The switch inside the IPQ4019 and the external PHY chip (QCA8075, which drives the
actual jacks) are connected by **PSGMII**: one high-speed serial link that carries the
data of five ports, at 6.25 Gbit/s.

At such speeds there is no separate clock wire. Each receiver recovers the clock from the
data itself, with a circuit called **CDR** (clock and data recovery), and the two ends
must agree where each port's bits start (lane alignment and calibration). This happens
when the link starts up.

Resetting the ESS restarts **one end** of that link. The PHY's end keeps running, its CDR
still locked onto the old signal. Usually the link resynchronises; sometimes one lane ends
up misaligned: the link looks up, but the data of one port is garbage.

### 5.4. Fix: Reset Both Ends in Order

As FreeBSD's `ar40xx_hw_malibu_psgmii_ess_reset()` and Qualcomm's U-Boot do it:

1. reset the PHY's end of the PSGMII link (over MDIO, the management bus to the PHY),
   and wait for its PLL to lock;
2. **freeze** the PHY's receive CDR, so it does not chase the signal while the other end
   restarts;
3. reset the ESS;
4. wait until the ESS end reports its PSGMII calibration done;
5. release the CDR.

The driver needed MDIO access for this, which it did not have before. After it, six
reboots in a row had both ports working every time.

FreeBSD additionally runs a self-test after the reset (4,096 test packets per port,
retrying on failure). That was not needed so far, and is noted as the next step if dead
ports come back.

## 6. The NAND Controller Ignores the CPU

### 6.1. Symptom

The NAND driver wrote a "read the chip's ID" command (`FETCH_ID`) to the controller's
command register. The status register read 0x402b, and nothing happened. `NAND_CTRL`
read 1.

### 6.2. Cause

The controller (QPIC) normally works together with a DMA engine called **BAM**: commands
and data travel as BAM descriptors, not as CPU register writes. U-Boot leaves it in
**BAM mode**. A comment in Linux's `qcom_nandc.c` says what that means: in BAM mode, the
CPU may only *read* the "operational" registers. Writes are ignored.

### 6.3. Fix: Reset, but Keep What Worked

Writing a BAM driver was the alternative. Instead, the driver resets the QPIC block
through its GCC reset (`GCC_QPIC_BCR`). After the reset `NAND_CTRL` is 0, register mode
works, and `FETCH_ID` completes.

The reset also clears U-Boot's page and ECC configuration. That configuration was
**known to work** with this chip, so the driver saves those registers before the reset
and writes them back after. This is the snapshot idea of section 4 again, but narrowed
to a list of registers that are understood. Details in
[Chapter 9](../09-nand-and-ubi.md).

## 7. Overwriting the Boot Loader in Memory

The same lesson, "someone else is already there", applies to memory.

### 7.1. Symptom

Loading a 48 MB NAND volume from U-Boot (`ubi read 0x85000000 netbsd`, no size given)
reset the board in the middle of the CRC check that follows.

### 7.2. Two Clues

- **The reset reason.** The chip records why it last reset. A normal reboot shows 0x10;
  this showed **0x33**. Not a normal reboot. Disabling the hardware watchdog changed
  nothing, so not the watchdog either.
- **The arithmetic.** Without a size, `ubi read` reads the whole volume: 48 MB from
  0x85000000 ends at about 0x88000000. U-Boot itself, while it runs, occupies about
  **0x875F0000 to 0x88013000**. The read wrote over the
  running boot loader's code and data.

### 7.3. Consequences

U-Boot's default boot command (`nandboot`) reads the `kernel` volume without a size. So
the volume that holds the default system must stay below about 39 MB, which is one
reason the flash edition is a 16 MB compressed image. And a read-back check after
writing goes to 0x89000000, above U-Boot, not to 0x88000000.

## 8. Lessons

- **Ask what ran before you.** A boot loader, firmware, a previous driver, a previous
  version of your own code: their state is your starting point.
- **Undo needs knowledge, reset needs completeness.** Undoing U-Boot's `eth_halt()` was
  quick but fragile. Resetting the ESS was thorough but needed everything U-Boot had set
  up to be rebuilt: first a snapshot (which missed `MODULE_EN`), finally a configuration
  from scratch.
- **A reset covers a block, not a link.** When two chips talk over a link that locks on,
  resetting one end needs the other end's cooperation.
- **Intermittent problems need counting.** "One boot in three, random port" was
  established by repeating the test, before any theory.
- **Read mature drivers for sequences, not just registers.** The PSGMII fix was not a
  register value; it was an order of steps.

## References

- `sys/arch/arm/qcom/ipq4019_ess.c` (switch, ESS reset, MDIO),
  `sys/arch/arm/qcom/ipq4019_edma.c` (rings), `sys/arch/arm/qcom/qcom_qpic.c` (QPIC reset)
- [Section 6.9, Switch](../06-how-the-drivers-work.md#69-switch-ipq4019_essc-qcaipq4019-qca8337n),
  [Chapter 9, NAND and UBI](../09-nand-and-ubi.md),
  [Section 8.8, Pitfalls](../08-how-the-flash-edition-works.md#88-pitfalls)
- FreeBSD: `sys/dev/etherswitch/ar40xx/` (`ar40xx_hw_malibu_psgmii_ess_reset()`)
- Linux: `drivers/mtd/nand/raw/qcom_nandc.c` (the BAM mode comment)
