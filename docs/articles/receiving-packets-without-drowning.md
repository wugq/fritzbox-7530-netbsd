# Receiving Packets without Drowning: Interrupts, Threads and a Hang

## Abstract

The receive path of a network driver decides what the whole system does under load.
This article follows three problems in the receive path of this port's Ethernet driver
(`sys/arch/arm/qcom/ipq4019_edma.c`):

- **A panic under load**: "softints stuck for 16 seconds". Receiving in the interrupt
  handler let the network stack starve the rest of the kernel.
- **Thousands of retransmissions on a single TCP flow**, with every error counter on the
  router at zero. The packets arrived, but in the wrong order.
- **A half-dead router**: no console, no ssh, but still forwarding at 800 Mbit/s. A
  work item had been queued twice and formed a loop.

Along the way it explains how a network card hands packets to the CPU, why interrupt
handlers must be short, receive livelock and how Linux's NAPI and NetBSD workqueues
avoid it, why TCP treats reordering as loss, and what "queued twice" does to a linked
list.

What you should know: threads, locks, and queues from any language. No kernel
experience is assumed. [The tprof article](profiling-packet-forwarding-with-tprof.md) explains thread
priorities and interrupt contexts in more detail; section 2 here repeats the essentials.

## 1. How a Network Card Hands Packets to the CPU

### 1.1. Descriptor Rings

The CPU does not copy every byte from the network card. The card writes packets
**directly into memory** (DMA, direct memory access), and the two sides coordinate
through a **ring**: an array of descriptors used as a circular queue.

```
            receive ring (256 descriptors)
   ┌────┬────┬────┬────┬────┬────┬────┬────┐
   │ ✓  │ ✓  │ ✓  │    │    │    │    │    │ ...
   └────┴────┴────┴────┴────┴────┴────┴────┘
     ▲              ▲
     │              └ card's index: the next free buffer it will fill
     └ driver's index: the next packet to take
```

- Each descriptor points to an empty buffer the driver gave the card.
- The card fills a buffer with a packet, marks the descriptor done, and advances its
  index (a register the driver can read).
- The driver takes packets from its index up to the card's, gives the card fresh
  buffers, and writes its own index back.

It is a single-producer, single-consumer queue, like a `java.util.concurrent` ring
buffer, except that the producer is hardware and the indices live in device registers.
If the driver falls behind and the ring fills up, the card drops new packets. That is the
right place to drop: it costs the CPU nothing.

### 1.2. Interrupts

The card tells the CPU "there are new packets" by raising an interrupt. The CPU stops
whatever it was doing, even in the middle of another thread, and runs the driver's
**interrupt handler**. While it runs, nothing else runs on that CPU: not threads, not
lower-priority interrupts.

The IPQ4019's Ethernet engine has 16 receive queues, each with its own ring and
interrupt. The driver uses four, one per CPU, and lets the card spread flows over them by
hashing addresses and ports (RSS, receive side scaling).

## 2. Background: Who Runs When

From most to least urgent (the tprof article, section 2.3, has the details):

| context | example | can be interrupted by |
|---|---|---|
| hardware interrupt | `edma_intr()` | more urgent interrupts only |
| software interrupt (softint) | `softnet/N`: the IP stack | hardware interrupts |
| kernel thread, by priority | `qcomedma0/N` (222), `xcall/N` (127) | the above, higher-priority threads |
| user process | `sshd`, `getty`, the shell | everything above |

A lower level runs only when everything above it on that CPU has nothing to do.

## 3. A Panic under Load: Receive Livelock

### 3.1. Symptom

The first version of the driver did all the receive work in the interrupt handler: take
the packets off the ring and pass them to the stack. Sending a flood of traffic to the
router with `iperf3` crashed it:

```
panic: softints stuck for 16 seconds
```

### 3.2. What the Panic Means

NetBSD's `heartbeat(9)` checks, from the clock interrupt, that each CPU still gets
around to running its software interrupts. If one does
not run for 16 seconds, the kernel assumes it is wedged and panics rather than hang
silently.

So the CPU was busy, but only with things more urgent than softints. The interrupt
handler was the only candidate.

### 3.3. Cause

The receive handler passed packets to the IP stack's input queue, and the stack's
softint (`ipintr()`) processes that queue until it is empty. Under a flood:

1. The interrupt handler fills the queue.
2. `ipintr()` starts draining it.
3. More packets arrive; the interrupt **interrupts the softint** and refills the queue.
4. `ipintr()` never sees an empty queue, and the clock softint below it never runs.

This is **receive livelock**: the system is busy all the time, but with accepting work it
will never finish. A paper by Mogul and Ramakrishnan named it in 1997, and Linux's NAPI
was the answer to it.

### 3.4. Fix: Do Little in the Interrupt

The interrupt handler now does almost nothing:

```c
static int
edma_intr(void *arg)
{
	...
	WR4(sc, REG_RX_INT_MASK_Q(rq->rq_hw), 0);   /* mask this queue's interrupt */
	WR4(sc, REG_RX_ISR, __BIT(rq->rq_hw));      /* acknowledge it */
	if (sc->sc_running && !rq->rq_queued) {
		rq->rq_queued = true;
		workqueue_enqueue(sc->sc_rxwq, &rq->rq_wk, NULL);  /* wake the thread */
	}
	...
}
```

The work goes to a **workqueue**, a kernel thread per CPU (`qcomedma0/N`) that runs
queued work items one after the other, like a single-threaded `ExecutorService`. Its
work function takes a batch of packets off the ring and hands them to the stack. If more
are waiting, it queues itself again, so other work at the same priority gets a turn in
between. When the ring is empty, it unmasks the interrupt.

Under overload, the thread cannot keep up, the ring fills, and the card drops packets.
Nothing in the kernel livelocks, because the interrupt stays masked while the thread is
working. The thread runs *below* the softints, so the stack can always drain what it has
been given.

> [!NOTE]
> The general rule: **an interrupt handler should do the least it can, and defer the
> rest to a context that can be preempted.** This is the "top half and bottom half" of
> many kernel books, and it is why NAPI exists.

## 4. Retransmissions with Zero Errors: Reordering

### 4.1. Symptom

With the new receive path, one TCP flow through the router reached about 550 Mbit/s,
but `iperf3` reported 2,000 to 4,000 retransmissions every 10 seconds. On the router,
every counter was zero: no interface errors, no drops in the stack.

### 4.2. Investigation

| idea | test | result |
|---|---|---|
| The ring overflows and the card drops packets | the engine's status register showed "ring running out" (RX_URG); the ring went from 128 to 256 descriptors | retransmissions unchanged |
| The router drops somewhere uncounted | look at the **receiving** machine instead: `netstat -s -p tcp` | many **out-of-order** packets |

The packets were not lost. They arrived in the wrong order.

### 4.3. Background: Why TCP Retransmits Reordered Packets

TCP numbers every byte. When a segment arrives out of order, the receiver acknowledges
the last byte it has in order again: a **duplicate ACK**. Three duplicate ACKs make the
sender assume the next segment was lost and resend it at once ("fast retransmit"), and
also halve its sending rate. TCP cannot tell "lost" from "late", so persistent
reordering looks to it exactly like loss and costs throughput.

### 4.4. Cause

Besides the interrupt, the driver had a timer (`edma_tick()`, ten times a second) as a
backstop in case an interrupt got lost. It processed every receive queue that had
packets waiting, **on the CPU the timer happened to run on**.

A forwarded packet is sent from a transmit queue chosen by the CPU that forwards it.
Normally all packets of a flow arrive on one receive queue, are processed on one CPU, and
leave through one transmit queue, in order. When the timer took some of them, those left
through a different CPU's transmit queue. The engine takes packets from its transmit
queues in turn, so the two queues' packets were interleaved and the order broke.

### 4.5. Fix

The timer no longer receives anything. It only queues the work on the CPU that owns the
receive queue (`rq_ci`, recorded by the interrupt):

```c
if (!rq->rq_queued && edma_rx_cons(sc, rq) != rq->rq_tail) {
	rq->rq_queued = true;
	workqueue_enqueue(sc->sc_rxwq, &rq->rq_wk, rq->rq_ci);
}
```

Retransmissions on a single flow went to zero.

> [!NOTE]
> The rule for multi-queue network cards: **all packets of one flow must take the same
> path through the system**, from receive queue through CPU to transmit queue. Any
> shortcut that moves some of them elsewhere reorders the flow.

## 5. The Half-Dead Router: Work Queued Twice

### 5.1. Symptom

After a few rounds of multi-flow `iperf3` tests:

- the serial console accepted no input;
- `ping` to the router failed; `ssh` completed the TCP handshake but never showed the
  server's greeting;
- **forwarding through the router still ran at over 800 Mbit/s**;
- the hardware watchdog did not fire.

This had happened several times before and been taken for a serial driver problem.

### 5.2. A Wrong Turn

The console was the visible symptom, so the serial driver was changed first: a periodic
"force stale" to unstick its receiver. That made things worse (the console then never
accepted input at all) and was reverted. **Changing code before having evidence
replaced one problem with two.**

### 5.3. Making It Reproducible

Checking the console and `ping` after every test showed the hang always started after
the first multi-flow test. A reliable trigger turns a "random hang" into an experiment.

### 5.4. Looking Inside a Hung System

With user space starved, no command could be typed. But the kernel's own `printf`
writes directly to the console, without any process. A debugging option was added:
`sysctl hw.qcomedma_watch=N` makes the timer print one line of queue state every N
seconds.

After the hang, it showed the receive work function running about **300,000 times a
second per queue**, with no packets to receive. The first guess was an interrupt storm,
and the engine's sticky "ring running out" bit (RX_URG) could cause one. Clearing that
bit was a correct fix for a real problem, but the hang stayed.

Then interrupt counts were added next to the run counts: the work function had run
**hundreds of millions of times**, the interrupt only **thousands**. Something other than
the interrupt kept queueing the work.

> [!TIP]
> Two counters side by side, "how often was it triggered" and "how often did it run",
> rule out whole classes of explanations at once.

### 5.5. Cause: An Intrusive List with a Loop

`workqueue_enqueue()` links the work item (`struct work`, embedded in the driver's queue
structure) into the workqueue's list. The link field is **inside the item itself**.
Such a list is called intrusive; NetBSD uses them everywhere because they need no memory
allocation. The flag `rq_queued` exists so an item is never queued while it is already
on the list.

The work function, in the version with the timer fix of section 4:

```
edma_rx_work():
    lock; rq_queued = false
    edma_rx():
        take packets off the ring
        unlock                       ← hand packets to the stack without the lock
        if_percpuq_enqueue(...)
        lock
    if packets are waiting:
        rq_queued = true; enqueue    ← did not check rq_queued
    unlock
```

While the lock was dropped, the timer ran on another CPU, saw "packets waiting and not
queued", and queued the item. Then the work function queued it again. The same node was
now on the list twice:

```
enqueue appends at the tail:   item.next = nil;  tail.next = item;  tail = item

1st enqueue (timer):      head → [rx] → nil              tail = [rx]
2nd enqueue (work fn):    [rx].next = nil;  tail.next = [rx]
                          but tail is [rx], so [rx].next = [rx]:

                          head → [rx] ─┐
                                  ▲    │
                                  └────┘
```

The workqueue thread takes the head, runs it, follows `next`, and finds the same item
again, forever. That thread is at priority 222, so on every CPU it ran in that loop:

- **user space** (getty on the console, sshd) and anything below 222 never ran again:
  the console and ssh died;
- **forwarding survived**, because the looping work function *is* the receive function:
  each pointless run also took any packets that had arrived;
- the **watchdog** is fed from the clock interrupt, which still ran.

### 5.6. Fix

Before queueing itself again, the work function checks the flag:

```c
if (rq->rq_queued) {
	/* already queued again: that run will unmask */
} else if (cons != rq->rq_tail) {
	rq->rq_queued = true;
	workqueue_enqueue(sc->sc_rxwq, &rq->rq_wk, NULL);
} else
	WR4(sc, REG_RX_INT_MASK_Q(rq->rq_hw), 1);   /* unmask */
```

Eight rounds of full-load tests later, console and `ping` still worked.

The irony: the timer change that fixed the reordering (section 4) opened the window for
this bug. **Whenever code releases a lock in the middle, ask what others can do in that
window**, and recheck every assumption made before it.

## 6. Lessons

- **Partial hangs have a shape.** "Console dead, forwarding alive" means the
  high-priority parts run and the low-priority ones starve. Sort the symptoms by the
  context they run in (section 2) and the cause usually sits on the boundary.
- **Look at both ends.** The router's counters were clean; the receiver's TCP
  statistics showed the problem.
- **Kernel `printf` works when nothing else does.** Plan a way to see inside before the
  system is hung.
- **Count triggers and runs separately.**
- **A fix for one concurrency problem can create the next one.** Test the old symptoms
  again after every fix.

## References

- `sys/arch/arm/qcom/ipq4019_edma.c`: `edma_intr()`, `edma_rx_work()`, `edma_tick()`
- [Chapter 6, section 6.8, Ethernet](../07-how-the-drivers-work.md#78-ethernet-ipq4019_edmac-qcomipq4019-ess-edma):
  the driver as a whole
- NetBSD manual pages: `workqueue(9)`, `heartbeat(9)`, `queue(3)` (the intrusive lists)
- J. Mogul and K. K. Ramakrishnan, *Eliminating Receive Livelock in an Interrupt-Driven
  Kernel*, ACM Transactions on Computer Systems, 1997
