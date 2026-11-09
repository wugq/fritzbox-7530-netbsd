# Counting Cycles: Making an Ethernet Driver Fast

## Abstract

A gigabit link carries over 80,000 full-size packets a second in each direction. At
716 MHz, a CPU has fewer than 9,000 cycles to spend on each of them, for the driver, the
IP stack and everything else. This article follows the Ethernet driver of this port
from "it works" to "it keeps up", by measuring cycles per packet and removing what cost
too much:

- reading device registers, which stalls the CPU for a bus round trip;
- copying packets instead of handing the card the memory they are in;
- several CPUs waiting for one transmit lock;
- a hash that put all flows between two machines on one CPU;
- flushing 32 KB of cache to publish one 16-byte descriptor.

It ends with a slowdown that looked like a VLAN problem and turned out to be the same
bottleneck seen through Ethernet flow control.

What you should know: [Receiving Packets without Drowning](receiving-packets-without-drowning.md)
(descriptor rings, interrupts, the receive thread). Some feeling for "slow" and "fast"
operations helps; this article gives the numbers.

## 1. A Budget per Packet

Start from what the hardware must sustain:

| | |
|---|---|
| link | 1,000,000,000 bit/s |
| full-size frame on the wire | 1538 bytes (1500 payload, headers, preamble, gap) |
| frames per second | about 81,000 per direction |
| CPU clock | 716 MHz |
| **cycles per frame, one CPU** | **about 8,800** |

Everything a forwarded packet costs (receive, IP, NAT, transmit, interrupts, locks) has
to fit in that budget, or be spread over more CPUs. A useful unit for all that follows is
therefore **cycles per packet**.

## 2. Measuring: a Cycle Counter in the Driver

Each Cortex-A7 core counts its clock cycles in a register (`PMCCNTR`, read by
`cpu_counter32()` in NetBSD). Reading it costs a few cycles. The driver reads it at the
start and end of its receive and transmit functions and adds the difference to per-CPU
totals:

```c
const uint32_t t0 = cpu_counter32();
... receive a batch ...
pr->rx_cyc  += cpu_counter32() - t0;
pr->rx_pkts += n;
```

`sysctl hw.qcomedma0.prof=1` prints the totals per CPU and clears them:
cycles per received packet, per sent packet, and cycles spent waiting for the transmit
lock.

This is **instrumentation**: exact, cheap, but limited to the places someone thought of.
Its counterpart, sampling the whole kernel, is the subject of
[the tprof article](profiling-packet-forwarding-with-tprof.md); the two together are what found most of
the problems below.

## 3. Transmit at 6,400 Cycles

### 3.1. Measurement

The first working transmit path cost about **6,400 cycles per packet**, most of the
budget, for the driver's transmit alone.

### 3.2. Where It Went

Two things stood out in the code:

1. **Every packet was copied** into a buffer the driver had prepared for the card.
   Copying 1,500 bytes is not just 1,500 byte moves: the data passes through the cache,
   and the copy must later be flushed for the card to see it (section 6).
2. **Every packet read two device registers**: the card's consumer index (to reclaim
   finished descriptors), and the producer index (read, modify, write it back).

The second is less obvious. A device register is not memory. Reading it sends a request
across the chip's interconnect to the device and **the CPU waits** until the answer
comes back; it cannot be cached, and the CPU cannot run ahead of it. Such a read costs
hundreds of cycles. A write, by contrast, can be posted: the CPU hands it off and
continues.

### 3.3. Fix

- **Zero copy**: map the packet's own memory (the mbuf) for the card
  (`bus_dmamap_load_mbuf`), and let the card read it where it is. For small frames,
  such as TCP acknowledgements, setting up a DMA mapping costs more than copying 64
  bytes, so those are still copied into a prepared buffer.
- **Write, don't read**: the driver knows where its producer index is; it writes it
  without reading it first (Linux's driver does the same).
- **Reclaim lazily**: look at the card's consumer index only when the ring is nearly
  full, not for every packet.

Transmit cost was halved.

> [!NOTE]
> A rough scale of costs, on this CPU: a register-to-register instruction, 1 cycle; a
> load from the L1 cache, a few; from main memory, around a hundred; **a device register
> read, hundreds**; a system call, thousands. Java hides most of this, but the same
> hierarchy is why, for example, a `volatile` read in a hot loop can matter.

## 4. Several CPUs, One Transmit Lock

### 4.1. Measurement

With several flows, the time spent **waiting for the transmit lock** rose from about
450 to over 1,100 cycles per acquisition. The driver had one transmit ring, protected by
one mutex, and four CPUs forwarding into it.

### 4.2. Why Contention Is Expensive Even When the Lock Is Held Briefly

A lock is a word in memory. Each core has its own cache, and a cache line can be
written by only one core at a time. When core 1 takes the lock, the line moves to core
1's cache; when core 2 tries next, the line must move to core 2, and so on. Each move
costs about as much as a trip to memory. Under contention the lock word ping-pongs between
caches, and every acquisition pays for it, however short the critical section is. (How
a waiting CPU spins is in [the tprof article, section 6](profiling-packet-forwarding-with-tprof.md#6-background-what-a-mutex-does-on-a-multiprocessor).)

### 4.3. Fix

The engine has 16 transmit queues. The driver now uses four, one per CPU, through
NetBSD's `if_transmit` interface: a packet leaves through the queue of the CPU that
forwards it. No CPU waits for another, and no lock line moves between caches.

This is the general pattern of scalable multiprocessor code: **give each CPU its own
copy** (queue, counter, cache) instead of sharing one, and combine only when needed. In
Java, `LongAdder` versus `AtomicLong` is the same idea.

## 5. Four Flows on Two CPUs: the RSS Hash

### 5.1. Symptom

With receive side scaling (RSS) on, four flows kept only two CPUs busy; the other two
were idle.

### 5.2. Background: How the Card Picks a Queue

The card computes a hash over the packet's headers and uses it to choose one of the
receive queues. Which headers go into the hash is configurable:

- **2-tuple**: source and destination IP address;
- **4-tuple**: the addresses plus the TCP or UDP ports.

All test flows ran between the same two machines. With the 2-tuple, they all had the
same addresses, so the same hash: every packet of one direction landed in one queue,
on one CPU.

### 5.3. Fix

Hash the 4-tuple only (`RSS_TYPE` = 0x1e; adjustable with `sysctl
hw.qcomedma0.rss_type`). The ports differ per flow, so flows spread out.

### 5.4. A Caveat: Hashing Is Random

Even with a good hash, four flows do not always land on four different queues. The
chance that four random flows hit four distinct queues out of four is 4!/4⁴, about
**9%**. Most of the time two or more flows share a CPU. That is why throughput with
four flows varies from run to run (section 7 met this again), and why tests run several
times and report ranges.

## 6. Flushing 32 KB to Publish 16 Bytes: Cache and DMA

### 6.1. Symptom

Changing how the descriptor rings were allocated (from a fixed address to
`bus_dmamem_alloc()`) left everything working, but forwarding dropped by about 25%.
The cycle counts showed transmit up from about 2,700 to **6,700 cycles per packet**;
receive was unchanged.

### 6.2. Background: The Card Does Not See the Cache

The CPU keeps recently used memory in its data cache and, with a write-back cache,
writes changes to memory only later. The card reads and writes memory directly and does
not look into the CPU's cache. So, before and after the card touches a buffer, the
driver must reconcile the two:

| direction | operation | NetBSD |
|---|---|---|
| CPU wrote, card will read | **clean**: write the cached lines back to memory | `bus_dmamap_sync(..., PREWRITE)` |
| card wrote, CPU will read | **invalidate**: drop the cached lines, so the next read goes to memory | `bus_dmamap_sync(..., POSTREAD)` |

Cache maintenance works on cache lines (64 bytes on the Cortex-A7), one line at a time.
Its cost is proportional to the **range** passed, not to how much was actually changed.

### 6.3. Cause

Before telling the card about a new packet, the driver synced the **whole 32 KB ring**
with `PREWRITE`: 512 cache lines, for one 16-byte descriptor that fits in one line. On
the old fixed-address ring the call had been nearly free; on memory from
`bus_dmamem_alloc()` it did real work. Receive syncs once per batch, so the cost hid
there; transmit synced once per packet and paid about 4,000 cycles each time.

### 6.4. Fix

Sync only the descriptors just written (usually one line). Transmit went back to about
2,950 cycles per packet.

> [!NOTE]
> `bus_dmamap_sync()` is how portable NetBSD drivers stay correct on machines whose
> caches are not coherent with DMA. It is never free by assumption: **give it the exact
> range.**

## 7. "VLAN Is Slow": the Same Bottleneck, Seen Differently

### 7.1. Symptom

Using a switch port as WAN puts its traffic in a VLAN (`vlan2`). Forwarding between LAN
and WAN was slower than between two LAN ports: one flow about 500 instead of 550-600
Mbit/s, several flows 600-760 instead of 860-910. No retransmissions anywhere.

### 7.2. First Round: Ruled Out

| idea | result |
|---|---|
| RSS does not hash tagged frames | it does: all four queues receive |
| `vlan(4)` takes a global lock | its transmit path does not |
| the CPU is not saturated | per-packet costs, measured, were the same as without VLAN |
| software tagging is slow | tagging in hardware helped one direction (WAN→LAN 380 → 471 Mbit/s, 676 → 750 with several flows), not the gap |

No retransmissions but limited throughput suggested something that slows senders down
without dropping: **Ethernet flow control**.

### 7.3. Background: Pause Frames

IEEE 802.3x lets a receiver whose buffers are filling up send a **pause frame**: "stop
sending to me for a moment". The sender's network card obeys, and TCP sees only a
slightly slower link. Nothing is lost, so nothing is retransmitted, and no error counter
moves. Unless you look at pause counters, flow control is invisible.

### 7.4. Second Round: The Switch's Counters

The switch keeps per-port statistics (MIB counters). Once the switch driver read them,
they showed the switch sending the PC **2,000-2,700 pause frames every 8 seconds**. The
switch's buffers for the CPU port were filling up because the CPU did not take frames
off fast enough.

But the default, non-VLAN mode showed **just as many pause frames**. The bottleneck was
the same in both modes: the CPU's receive rate. What differed between runs was the RSS
distribution (section 5.4): in one VLAN run, the four flows had landed on two CPUs, one
of which processed an average of 235 frames per run of its receive thread, never
catching up.

Two more facts closed the case:

- the CPU already ran at its top frequency, 716 MHz (set by U-Boot), so frequency
  scaling could not help;
- a saturated CPU handled 35,000-40,000 packets a second, about **20,000 cycles per
  packet**, of which the driver's own measured paths were only about 5,600. Most of the
  time was in the network stack.

That last number is where [the tprof article](profiling-packet-forwarding-with-tprof.md) picks up.

## 8. Lessons

- **Turn performance into a budget.** Cycles per packet against the cycles available
  says at once whether a design can work.
- **Measure before and after every change, with the same counters.** The 25% regression
  in section 6 came from a change that "only" moved memory; without the cycle counts it
  would have been blamed on something else.
- **Know the expensive operations**: device register reads, cache maintenance over large
  ranges, cache lines bouncing between cores. They do not show up as "work" in the code.
- **Prefer per-CPU structures** to shared ones under a lock.
- **Missing errors are not proof of no problem.** Flow control slows traffic without a
  single error or retransmission. Find the counters that would show it.
- **When two modes differ, compare all their counters.** The VLAN mode looked guilty
  until the default mode turned out to have the same pause frames.

## References

- `sys/arch/arm/qcom/ipq4019_edma.c`: `edma_rx()`, the transmit path, `hw.qcomedma0.prof`
- [Section 6.8, Ethernet](../06-how-the-drivers-work.md#68-ethernet-ipq4019_edmac-qcomipq4019-ess-edma)
  and [Section 6.9, Switch](../06-how-the-drivers-work.md#69-switch-ipq4019_essc-qcaipq4019-qca8337n)
- NetBSD manual pages: `bus_dma(9)`, `cpu_counter(9)`, `ifnet(9)` (`if_transmit`)
- ARM, *Cortex-A7 MPCore Technical Reference Manual*: caches, the performance monitor
