# Where Do the Cycles Go? Profiling Packet Forwarding with tprof

## Abstract

The router forwarded one TCP flow at 557 Mbit/s and four flows at 816 Mbit/s, short of
the 941 Mbit/s a gigabit link can carry. To find out why, we sampled the kernel with
NetBSD's profiler, `tprof`. It failed twice before it showed anything useful, and each
failure was a bug in NetBSD:

- **Samples came from one CPU only.** An interrupt priority was set on one CPU, but
  each CPU has its own copy of it.
- **The forwarding code was invisible.** The command that starts the profiler on each
  CPU was queued behind the network threads, and under load it never ran.

The working profile then showed four CPUs taking turns on one global lock. Releasing that
lock before sending raised four flows to line rate, at 29% fewer cycles per megabit.

This article follows the whole process, including the wrong turns, and explains the
machinery along the way: performance counters, interrupt controllers, kernel thread
priorities, and what a mutex really does on a multiprocessor.

What you should know: how to use a Unix shell, and the ideas of threads and locks from
any language (Java's `synchronized` is enough). No kernel experience is assumed.

## 1. The Question

The test bench: a PC on one switch port, a second machine on another, the router routing
between them. `iperf3` measures TCP throughput:

| flows | throughput | |
|---|---|---|
| 1 | 557 Mbit/s | |
| 4 | 816 Mbit/s | 1.46 times one flow, with four CPUs |
| line rate | about 941 Mbit/s | 1000 Mbit/s minus Ethernet, IP and TCP headers |

Two questions follow. Why is one flow limited to 557? And why do four flows, which the
network card spreads over four CPUs, gain only 46%?

"Slow" has many possible causes, at every layer a packet passes through: the link, the
network card's DMA, the interrupts, the driver, the IP stack, the firewall, the
transmit path, the other machine. You can guess and test one cause at a time, or you can
**measure where the time goes**. For the second you need a profiler.

Two kinds of measurement exist:

- **Instrumentation**: put timers into the code. The Ethernet driver already did this
  (`sysctl hw.qcomedma0.prof`, cycles per received and sent packet). It is exact, but it
  only measures what you thought of measuring.
- **Sampling**: interrupt the CPU at regular intervals and note where it was. Over a few
  seconds, the places that use the most time collect the most samples. It finds what you
  did not think of. `tprof` is NetBSD's sampling profiler.

## 2. Background: How a Sampling Profiler Works

### 2.1. Performance Counters

Every Cortex-A7 core has a **Performance Monitoring Unit (PMU)**: a cycle counter and four
event counters. An event counter counts what you choose: CPU cycles, instructions,
cache misses, exceptions taken. They are ordinary 32-bit registers that count up by
themselves while the core runs.

When a counter wraps around from 0xffffffff to 0, the PMU raises an **overflow interrupt**.
That is the whole trick of sampling:

1. `tprof` loads the counter with a negative number, for example 0xfffee800 = -71680.
2. The counter counts CPU cycles. After 71680 cycles it overflows.
3. The overflow interrupt stops whatever the core was doing. The handler records the
   **program counter** (which instruction was running), the thread, and the CPU. Then
   it loads -71680 again.

At 716 MHz that is about 10,000 samples per second per CPU. A function that takes 30% of
the time collects about 30% of the samples.

### 2.2. How an Interrupt Reaches a CPU

An interrupt does not go straight from a device to a core. On this chip it passes through
the **GIC** (Generic Interrupt Controller), which decides which core gets it and when:

```
 devices ──► GIC distributor ──► CPU interface 0 ──► core 0
                 │          ├──► CPU interface 1 ──► core 1
                 │          ├──► ...
   per interrupt: enabled?  │
   priority (0 = highest,   └─ per core: priority mask
   0xff = lowest)              (only interrupts more urgent than the mask pass)
```

The GIC numbers its interrupts in three ranges:

| numbers | kind | example |
|---|---|---|
| 0-15 | SGI, software-generated: one core interrupts another | "run this function" |
| 16-31 | **PPI, private peripheral**: each core has its own | the timer, **the PMU** (number 23) |
| 32 and up | SPI, shared peripheral: one source for the whole chip | Ethernet, UART, USB |

A PPI is private: core 2's PMU interrupt is not core 0's. For that reason the GIC keeps
**one copy per core** of the registers for numbers 0-31, the enable bits and the
priorities. The ARM manuals call such registers **banked**: the same address, but each
core reading or writing it sees its own copy.

Keep that word in mind; it is the first bug.

### 2.3. Where Code Runs in the NetBSD Kernel

Kernel code runs in one of three kinds of context, from most to least urgent:

1. **Hardware interrupt handlers**: run the moment the GIC delivers an interrupt; must be
   short.
2. **Software interrupts (softints)**: work an interrupt handler defers, run as soon as
   the CPU leaves the hardware interrupt. In NetBSD each softint level has its own thread
   on each CPU (`softnet/1` is the network softint on CPU 1).
3. **Kernel threads**: ordinary threads with a priority, chosen by the scheduler, like
   Java threads but inside the kernel.

The scheduler always runs the highest-priority runnable thread on a CPU. The router's
own `ps` shows the kernel's threads with their priorities (larger is more urgent); these
are the ones on CPU 1:

```
# ps -axs -o lid,pri,cpuid,lname
 LID PRI CPUID LNAME
  21 223     1 softser/1
 101 222     1 qcomedma0/1
  18 222     1 softnet/1
  19 221     1 softbio/1
  20 220     1 softclk/1
  22 127     1 xcall/1
```

| thread | what it is |
|---|---|
| `softser/1`, `softnet/1`, `softbio/1`, `softclk/1` | software interrupts: serial, network, disk, clock |
| `qcomedma0/1` | our Ethernet driver's receive workqueue |
| `xcall/1` | runs cross-calls (section 4) |
| user processes | priorities 0-63, below all of these |

A forwarded packet passes through two of these threads on the CPU that received it:

```
Ethernet interrupt (hardware)
  └► wakes qcomedma0/N            the driver takes packets off the receive ring
       └► if_percpuq_enqueue ──► softnet/N
                                    ether_input → ip_input → ipflow_fastforward
                                    → ether_output → vlan → edma transmit
```

## 3. First Attempt: One CPU of Four

### 3.1. Symptom

While forwarding, `tprof monitor` collected 138 samples in 8 seconds, all from CPU 3.
Four busy loops, one per CPU, gave the same: only CPU 3. At 10,000 samples per second
per CPU it should have been hundreds of thousands.

### 3.2. Following the Evidence

A first misreading cost some time: `tprof analyze` prints the columns `pid lwp cpu k`,
and the last one (`k`, 1 for kernel code) was taken for the CPU number. The real CPU
column said 3 everywhere.

Then the question: is the counter not counting, or is the interrupt not arriving?
`vmstat -i` lists the interrupts each CPU took:

```
armgic (cpu3) irq 23     ...
```

The PMU interrupt (number 23) appeared for CPU 3 only, while the timer (`irq 20`) had a
line for every CPU.

So the interrupt was enabled on CPU 3 only, or blocked on the others. The timer is also a
PPI, so PPIs in general worked. What was
different about the PMU?

The boot messages gave the hint. The PMU driver attached at 1.0 s, but announced its
interrupt at 1.4 s:

```
[     1.000003] armpmu0 at simplebus0: Performance Monitor Unit
[     1.416820] armpmu0: interrupting on GIC irq 23
```

NetBSD lets a driver postpone setting up its interrupt (`config_interrupts()`) until the
other CPUs are running. The timer's interrupt is set up before that. So the difference
is timing: before or after the secondary CPUs started.

### 3.3. Cause

Reading the GIC driver (`sys/arch/arm/cortex/gic.c`) in that order:

1. When a secondary CPU starts, it gives every interrupt nobody uses yet the priority
   0xff, the lowest. With the CPU's priority mask at 0xff, an interrupt of priority 0xff
   never passes.
2. Later, on CPU 3, the PMU's interrupt is set up. The driver writes its priority, on the
   CPU it runs on: CPU 3. The priority register is banked, so CPUs 0-2 keep 0xff.
3. Then it enables the interrupt on every CPU, correctly, with a cross-call.

Enabled everywhere, but deliverable only on CPU 3. The counters on the other CPUs did
count, but their overflow interrupts were never delivered.

### 3.4. Fix

Patch 0005: when the GIC driver enables a private interrupt on a CPU, it also writes
the priority there. It already runs on each CPU for the enable, so the priority is set
in the same step. After it, each CPU collected about 87,000 samples in 8 seconds under
four busy loops.

> [!NOTE]
> **The general lesson:** on ARM, any code that writes interrupt 0-31 registers once, on
> one CPU, is suspect. Banked registers are a hardware form of per-thread state, like
> Java's `ThreadLocal`: writing it in one thread does not set it in the others.

## 4. Second Attempt: Forwarding Is Invisible

### 4.1. A Contradiction

With all four CPUs sampling, a forwarding profile showed... almost nothing: a few million
cycles counted per CPU per second (of 716 million), no samples from the driver or the
network stack. Yet the CPUs were clearly busy:

- `vmstat` reported 88% system time;
- four busy loops took 24 seconds during forwarding, against 9 seconds when idle;
- the driver's own cycle counter (`hw.qcomedma0.prof`) measured about 2,400 cycles per
  received packet.

Receiving or sending traffic on the router itself, or a `dd` to `/dev/null`, all
profiled normally. Only forwarding was invisible. **When two measurements disagree, one
of them is wrong, and finding out which one is the next step.**

### 4.2. Hypotheses

Each candidate explanation, and how it was ruled out:

| hypothesis | test | result |
|---|---|---|
| The spinning code uses `WFE` (wait for event), where the core stops and does not count cycles | read NetBSD/arm's locks | they do not use it |
| The driver writes the PMU registers itself and resets tprof's counter | read the driver | it only reads the cycle counter |
| The GIC merges PMU interrupts while the CPU is busy | compute: even merged, the counts would add up | they did not |
| The secure firmware (TrustZone) hides the cycles | busy loops are counted normally | not it |

### 4.3. The Decisive Experiment

Instead of reasoning about tprof, test the hardware without it. A temporary change to
the driver (removed afterwards) took the PMU's fourth counter for itself, set it to count
CPU cycles, and read it before and after every call of the receive function `edma_rx()`:

1. **Without tprof**: counter 3 counted 99% of the cycles. The hardware counts fine
   during forwarding.
2. **With tprof running**: tprof's counter 0 never overflowed inside `edma_rx()`. So it
   was not an interrupt held back.
3. **Reading tprof's counter 0 directly**: during calls lasting over a million cycles it
   stayed at exactly 0xfffee800, the value tprof loads before starting. It had been
   configured but **never started**.

Why would tprof not start a counter? Reading `sys/dev/tprof/tprof.c`: it starts the
counters with

```c
xc = xc_broadcast(0, tprof_start_cpu, tb, ...);
```

### 4.4. Cause: Cross-Calls and Thread Priorities

The PMU's registers belong to each core, so the code that starts a counter must run *on*
that core. NetBSD's tool for this is the **cross-call** (`xcall`): "run this function on
every CPU and tell me when all are done". It comes in two kinds:

- **Low priority** (`flags = 0`, used here): the function is queued for the thread
  `xcall/N` on each CPU. That thread has priority **127**.
- **High priority** (`XC_HIGHPRI`): the function runs in a software interrupt, which
  takes the CPU from any thread.

Under forwarding load, the receive workqueue `qcomedma0/N` runs at priority **222**. One
`edma_rx()` call takes about 6 ms; at the end it queues itself again and runs once more.
The scheduler always picks the highest priority, so `xcall/N` at 127 never runs on that
CPU while traffic lasts. tprof's start request waits on CPUs 0-3 until the traffic ends,
which is exactly when there is nothing left to measure.

The busy loops were counted because, with them present, the forwarding CPUs had idle
moments in which `xcall/N` could run.

### 4.5. Fix

Patch 0006: tprof uses `XC_HIGHPRI`. The functions only read and write PMU registers,
which is allowed in a software interrupt.

> [!NOTE]
> **Priority starvation** is the general mechanism here. A thread that requeues itself
> to "be fair" is fair only to threads of the same or higher priority; everything below
> it waits. In Java the scheduler is the operating system's and thread priorities are
> mostly hints, so you rarely see this. In a kernel, priorities are strict.

## 5. Reading the Profile

### 5.1. One Flow

```
  10.2%  splx                 (softnet/1)
   6.5%  splx                 (qcomedma0/1)
   3.6%  armgic_set_priority
   3.5%  _splraise
   2.8%  ether_input
   2.7%  edma_rx_work
   2.3%  edma_tx_drain
   2.2%  pfil_run_hooks
   2.1%  armv7_dcache_wb_range
   2.1%  ether_output
   ...
```

CPU 1 used 5.7 billion cycles in 8 seconds: 100% of 716 MHz. One flow is limited by
one CPU. No single function dominates; the cost is spread over the whole path:

- **Interrupt priority changes**, about 30% (`splx`, `_splraise`, `armgic_set_priority`).
  Kernel code raises the "interrupt priority level" (IPL) around critical sections and
  lowers it afterwards; lowering it can mean writing the GIC's priority mask register,
  a device access. Part of
  this number is an artefact (section 7).
- **Cache maintenance and DMA**, about 12% (`armv7_dcache_*`, `_bus_dmamap_sync`). The
  network card reads and writes memory directly, bypassing the CPU's cache, so the
  driver must flush or discard cache lines for every packet.
- **The stack**, a few percent per layer (`ether_input`, `pfil_run_hooks`, `ipflow`,
  `vlan`, `ether_output`).
- **The driver**, about 6%.

To make one flow much faster, every layer would have to get cheaper. There is no single
fix.

### 5.2. Four Flows

```
  12.1%  mutex_enter          (softnet/2)
  11.2%  mutex_enter          (softnet/1)
   4.9%  devnullop            (softnet/2)
   4.6%  devnullop            (softnet/1)
   2.2%  mutex_enter          (softnet/3)
   2.1%  mutex_enter          (softnet/0)
   ...
```

About 40% of all samples are `softnet` threads in `mutex_enter`, plus a strange
`devnullop`. These threads are not working: they are **waiting for a lock**.

A flat profile like this one says where the time goes, not why. It lists functions, not
who called them. So the next step was to read the code these threads run (section 2.3)
and list every lock taken on the way. One stood out: `ipflow_fastforward()` takes the
global mutex `ipflow_lock` on entry, and releases it only after `if_output_lock()` has
sent the packet. Everything from the routing decision to the driver's transmit
function ran with one global lock held.

## 6. Background: What a Mutex Does on a Multiprocessor

### 6.1. Taking a Free Lock

A NetBSD mutex is one machine word holding the owner (a pointer to the thread) or 0 for
free. `mutex_enter()` tries one **atomic compare-and-swap**: "if the word is 0, put my
thread pointer in it". On ARMv7 that is a pair of instructions, `ldrex`/`strex`, that
fails and retries if another core touched the word in between. Java's `synchronized` and
`AtomicReference.compareAndSet` do the same thing underneath.

A free lock costs a few dozen cycles. The trouble starts when it is taken.

### 6.2. Waiting for a Taken Lock: Spin or Sleep

`ipflow_lock` is an **adaptive mutex** (`MUTEX_DEFAULT`). When it is taken, the waiting
thread looks at the owner:

- **The owner is running on another CPU**: it will probably release the lock soon.
  Sleeping and waking up would cost more than waiting, so the waiter **spins**: it
  checks the lock word again and again, with exponentially growing pauses between
  checks. The pause is a loop calling an empty function: that is `devnullop` in the
  profile (`SPINLOCK_BACKOFF` in `sys/lock.h`, whose hook is `nullop`, an alias of
  `devnullop`).
- **The owner is not running**: spinning would be pointless, so the waiter goes to
  sleep on a queue (a "turnstile") and is woken when the lock is released.

(HotSpot does the same for Java monitors; it calls it adaptive spinning.)

Here the owner was always running, another `softnet` thread busy sending a packet, so
the waiters spun. **Spinning is CPU time that does no work**, and that is what tprof
showed.

### 6.3. Why One Lock Limits Four CPUs

If every packet spends time *L* inside the lock and *T* in total, at most *T/L* CPUs can
be busy at once; the rest wait. Sending (`if_output_lock`: Ethernet header, VLAN tag, the
driver's transmit) was a large part of the per-packet work, and it was inside the lock.
So four CPUs received packets in parallel, then queued to forward them one by one. That
is why four flows were only 1.46 times one.

## 7. The Fix and Its Alternatives

### 7.1. Options

| option | expected | outcome |
|---|---|---|
| Turn the fast path off (`sysctl net.inet.ip.maxflows=0`) | no global lock at all | measured: **599 Mbit/s**, worse. The normal forwarding path costs more per packet than the parallelism gains |
| Finer locks: one per hash bucket of the flow table | flows in different buckets in parallel | send would still be inside a lock; a larger change to NetBSD |
| One flow table per CPU | no sharing at all | larger change; flows would be learnt once per CPU |
| **Release the lock before sending** | only the table lookup is serialized | small change; chosen |

### 7.2. Why the Lock Covered the Send

The obvious fix, moving `mutex_exit()` up, is not safe as it stands. Two things need the
lock to stay valid:

1. **The flow entry.** Once the lock is released, another CPU may free the entry
   (`struct ipflow`), so it must not be touched afterwards.
2. **The route.** The entry caches a pointer to the route (`struct rtentry`). The route
   must not be freed while the packet is being sent through it. The cache keeps it alive
   with a **passive reference** (`psref`). A psref is very cheap, because it is recorded
   on the current CPU only, with no shared counter for the CPUs to fight over. The price
   is a strict rule: a psref is tied to one CPU and one thread at a time. The psref here
   is stored in the flow entry, so two CPUs could not use the same entry at once, and the
   global lock enforced that.

### 7.3. The Patch

Patch 0007, in `ipflow_fastforward()` (`sys/netinet/ip_flow.c`), after all checks are
done and still under the lock:

```c
sockaddr_copy(&dst_store.sa, sizeof(dst_store), dst);  /* 1. copy the destination */
rt_ref(rt);                                             /* 2. a normal reference */
rtcache_unref(rt, &ipf->ipf_ro);                        /* 3. drop the psref */
mutex_exit(&ipflow_lock);                               /* 4. release the lock */
if ((error = if_output_lock(rt->rt_ifp, rt->rt_ifp, m, &dst_store.sa, rt)) != 0)
	IP_STATINC(IP_STAT_CANTFORWARD);                /* 5. send, unlocked */
rt_unref(rt);                                           /* 6. drop the reference */
```

1. The destination address lived in the flow entry, which may be freed after step 4: a
   copy on the stack survives.
2. `rt_ref()` is an ordinary reference count on the route (an atomic increment), not
   tied to a CPU. Freeing a route waits until the count is back to zero. It was a
   `static` function in `route.c`; the patch makes it public.
3. With a normal reference held, the per-flow psref is no longer needed.
5. Send errors used to be counted in the flow entry, which may now be gone. They go to
   the global counter `IP_STAT_CANTFORWARD` instead, where the entry's counts ended up
   anyway when it was freed.

The cost: two atomic operations per packet on a shared counter in the route, instead of
a lock held for the whole send.

### 7.4. Results

Four flows, 8 seconds of profile:

| | before | patch 0007 |
|---|---|---|
| throughput | 816 Mbit/s | **914 Mbit/s** (line rate) |
| cycles, four CPUs | 16.2 billion | 12.9 billion |
| cycles per Mbit/s | 19.9 million | 14.1 million (**-29%**) |
| samples spinning on a lock | 39% | 4% |

Faster *and* cheaper: the cycles that went into spinning are gone. Through the VLAN WAN
port, one flow went from 563 to 626 Mbit/s and four flows from 818 to 914. The automated
hardware test (`tools/hwtest.sh`) passed all checks.

## 8. What Is Left

- **`splx` is overcounted.** Even with patch 0006, a fully loaded CPU collects about one
  seventh of the samples its cycle count implies. The PMU interrupt still arrives late in
  parts of the receive path, presumably where interrupts are disabled. When they are
  enabled again, inside `splx()`, the waiting interrupt is taken, so the sample lands on
  `splx`. This profiler error is called **skid**. Not investigated further.
- **The receive workqueue starves every lower-priority thread** on its CPU while traffic
  lasts (section 4.4). Other low-priority cross-calls wait just like tprof's did.
- **Why** forwarding was already faster on this day than the day before has not been
  answered.

## 9. A Method for "The Network Is Slow"

The path this investigation took, generalized:

1. **Measure the symptom precisely.** One flow and several flows, both directions, with
   and without the firewall. How the numbers scale (here, 1.46 times for four flows)
   already narrows the cause.
2. **Check that the work is spread.** `vmstat -i` (interrupts per CPU), `top`, and
   `ps -axs` (threads per CPU). One CPU at 100% and the others idle is a different
   problem from all four at 100%.
3. **Look for errors on both ends.** `netstat -s` on the receiver, not just the router.
   (An earlier problem in this port, packets reordered by the transmit queues, showed up
   only there.)
4. **Profile**, and before trusting the profile, check it against an independent
   measurement. Here the cycle count disagreed with `vmstat`, and that led to two bugs.
5. **Read the flat profile by thread**, then read the code those threads run. Large
   `mutex_enter` or spin-wait entries mean a lock: find which one and what it covers.
6. **List several fixes, measure the cheap ones first.** Turning the fast path off cost
   one `sysctl` and ruled out the simplest idea in a minute.
7. **Measure again** with the same test, and compare against the numbers from step 1.

## References

- Patches: `patches/0005-arm-gic-banked-ppi-priority.diff`,
  `patches/0006-tprof-highpri-xcalls.diff`, `patches/0007-ipflow-send-unlocked.diff`
- [Section 7.5, Profiling with tprof](../07-board-kernel-configuration.md#75-profiling-with-tprof):
  the commands
- NetBSD manual pages: `tprof(8)`, `xcall(9)`, `mutex(9)`, `psref(9)`, `workqueue(9)`,
  `spl(9)`
- ARM, *Generic Interrupt Controller Architecture Specification* (banked registers,
  priority and priority mask); *Cortex-A7 MPCore Technical Reference Manual*, the PMU
  chapter
