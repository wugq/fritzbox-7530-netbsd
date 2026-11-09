# A Serial Console Driver: from Polling to Interrupts

## Abstract

The serial console is the first driver of any port and the last resort when everything
else fails. On this chip it is a Qualcomm UARTDM, which works differently from the
16550-style UART most systems have. This article follows its driver through four
problems:

- reading received bytes too early returned garbage;
- a burst of 256 characters made the console deaf until power-off;
- a patch modelled on Linux held back the end of every line;
- the interrupt-driven rewrite panicked under heavy output, in a process that had done
  nothing wrong.

Along the way: what a UART and its FIFO are, polling versus interrupts, why a driver
splits work between a hardware and a software interrupt, how the terminal layer shares
its queues between CPUs, and why a crash report often points at the victim rather than
the culprit.

What you should know: threads and locks from any language. [Receiving Packets without
Drowning](receiving-packets-without-drowning.md), sections 1.2 and 2, covers interrupts and contexts.

## 1. Background: a UART

A UART turns bytes into a serial bit stream and back: at 115200 bit/s with 8 data bits,
no parity and one stop bit, 10 bits per character, so one character every 87 µs.

The CPU does not handle every bit. The UART has **FIFOs**, small hardware queues for
received and to-be-sent characters. On the receive side the question is how the driver
learns that characters are waiting:

- **Polling**: check a status register regularly. Simple, but either wasteful (checking
  often) or slow (checking rarely); and the kernel's earliest boot code must work this
  way, before interrupts exist.
- **Interrupts**: the UART interrupts the CPU when the FIFO holds data, or holds more
  than a **watermark** level.

## 2. The UARTDM Packs Characters

The 16550 hands out one character per register read. The UARTDM packs **four received
characters into each 32-bit FIFO word**. A partial word (one to three characters) waits
in a packing register until a **stale event**: the line has been idle for a moment, the
UART decides the transfer has ended, pushes out the partial word, and records in
`RX_TOTAL_SNAP` how many bytes the transfer had.

So the driver cannot simply "read what is there". It must know where a transfer ends.

### 2.1. First Trap: Reading Too Early

The first, polled driver estimated the number of bytes in the FIFO and read that many.
The result: NUL characters and reordered bytes. Words read before the stale event are
not reliable.

The fix, as Linux's `msm_serial` does it: read only after a stale event, exactly
`RX_TOTAL_SNAP` bytes.

### 2.2. Second Trap: Forcing a Stale Event

To be safe against a missed stale event, the driver forced one (`FORCE_STALE`) after a
second of idle time. With no data in the FIFO, that **stopped reception for good**: the
console never accepted input after boot. Reverted; the driver forces a stale event only
when data is waiting and no stale event has come.

(The reason for the change was a "deaf console" under network load. That turned out to
be a bug in the Ethernet driver, see [Receiving Packets without
Drowning](receiving-packets-without-drowning.md#5-the-half-dead-router-work-queued-twice). The
serial driver had been changed without evidence; the change made things worse.)

## 3. The 256-Character Wall

### 3.1. Symptom

The automated test sent a long command, about 260 characters, to the router's console.
Then nothing: no echo, no response to Ctrl-C, until power-off. The network still worked
and the watchdog did not fire, so the kernel was alive. The console's receiver was
dead.

### 3.2. Finding the Boundary

A hang that depends on input length invites a search for the exact boundary:

| characters in one burst | result |
|---|---|
| 246 | fine |
| 256 | console dead |

256 bytes is exactly the receive FIFO: 64 words of 4 characters. **A number like that is
a clue in itself.**

### 3.3. Cause

The driver read the FIFO only after a stale event, that is, after the line went idle.
A burst of 256 characters or more filled the FIFO **before** the line went idle. The
FIFO overflowed, and after an overflow the UART produced no stale event anymore. The
driver waited for an event that would never come.

### 3.4. A Wrong Turn

Linux's driver also uses a watermark interrupt (RXLEV) to read complete words before the
stale event. Copying that path made reading continuous, but the last one to three
characters of each line stayed in the packing register until the *next* input arrived:
type a command, and its end appeared only with the next keystroke. That is the first
trap again (section 2.1) from the other side. Reverted.

### 3.5. Stopgap, Then a Different Design

The stopgap: detect the overflow (`SR_OVERRUN`) and reset the receiver. The line is
lost, but the console survives.

The real fix came from FreeBSD's driver, which sidesteps the whole problem. The UARTDM
has a **single-character mode** (`RX_SC_ENABLE`): every received character gets its own
FIFO word. No packing, no partial words, no stale events, no `RX_TOTAL_SNAP`. With the
receive watermark at 0, an interrupt comes as soon as one character is in the FIFO.

After the rewrite, a burst of 1,000 characters arrived complete.

> [!NOTE]
> When a design keeps producing edge cases, look for a mode of the hardware, or a design,
> in which the edge cases do not exist. Three patches on the packed mode each traded one
> bug for another; one change of mode removed them all.

## 4. Two Levels: Hardware and Software Interrupt

The interrupt-driven driver does its work in two steps:

```
character arrives → interrupt → qcom_uart_intr():   drain the FIFO into a 1 KB ring,
                                                     schedule the soft interrupt
                                qcom_uart_softintr(): hand the characters to the tty
                                                      layer (l_rint)
```

Two reasons:

- the hardware interrupt must be short (the general rule, see [Receiving Packets without
  Drowning](receiving-packets-without-drowning.md#34-fix-do-little-in-the-interrupt));
- the terminal layer may not be called at the UART's interrupt priority (`IPL_SERIAL`),
  because it takes locks that code at lower priorities also takes. Being interrupted
  while holding such a lock, by code that wants the same lock, would deadlock the CPU.

Transmit changed too. The polled driver waited for each character to leave: 87 µs of a
spinning CPU per character, 90 ms per kilobyte. Now the driver writes up to 128
characters into the transmit FIFO at once and asks for an interrupt when the FIFO drains
below a watermark, then writes the next batch.

## 5. A Panic in an Innocent Process

### 5.1. Symptom

Testing heavy output with `jot 5000` (print the numbers 1 to 5000), the first interrupt
driver crashed around line 2,700:

```
Fatal kernel mode data abort
... in memcpy, writing to a NULL pointer
process: jot
```

### 5.2. Reading the Crash Correctly

`jot` only writes to its terminal. The crash was in `memcpy`, called from the terminal
layer while copying `jot`'s output into the terminal's output queue. Nothing in `jot`
was wrong, and `memcpy` is certainly not. The question is **who broke the data that
`memcpy` was given**.

A crash in the middle of correct code, working on a data structure, usually means that
someone else corrupted the structure earlier. The crash report shows the victim.

### 5.3. Background: the Terminal's Output Queue

Characters written to a terminal go into its output queue, a **clist**: a linked list of
small blocks of characters. Two sides use it:

- the writer, here `jot`'s `write()` system call: `ttwrite()` appends with `b_to_q()`;
- the driver's transmit path: `qcom_uart_start()` takes characters out with `q_to_b()`
  and puts them into the FIFO.

On a multiprocessor these can run on different CPUs at the same time. NetBSD's terminal
layer protects every terminal's queues with one global lock, **`tty_lock`**. The tty
code calls into the driver with that lock held: `ttwrite()` holds it when it calls
`ttstart()`, which calls the driver's start function.

### 5.4. Cause

When a batch had been sent, the driver's soft interrupt called the line discipline's
`l_start` to send more, **without taking `tty_lock`**. So `q_to_b()` on one CPU and
`b_to_q()` on another changed the same clist at the same time. Each did its pointer
updates assuming nobody else was there; interleaved, they left a block pointer NULL, and
the next `memcpy` into it crashed.

It is the textbook race, the same as two Java threads calling `add()` and `remove()` on
an unsynchronised `LinkedList`, except that a kernel has no exception to catch it: it
crashes.

### 5.5. Fix

The soft interrupt takes `tty_lock` around clearing the busy flag and calling
`l_start`, just as the tty layer itself does. `jot 5000` then printed all 5,000 lines,
complete and in order, in 2.7 seconds (the line rate allows about 2.1).

> [!NOTE]
> When a driver calls back into a subsystem from its own context (a soft interrupt, a
> timer), it must follow the subsystem's locking rules, which the subsystem normally
> follows for it. Look at how the subsystem itself calls the same function, and do the
> same.

## 6. The Kernel's Own printf

Kernel messages (`printf`, the debugger) go out through `cnputc()`, one character at a
time, polling. They can come from anywhere, including a CPU that is panicking. The
interrupt-driven transmit path writes batches into the same FIFO.

Both take one **spin lock** (`__cpu_simple_lock`), so a batch and a kernel message do
not mix within one transfer. A normal mutex cannot be used: the first kernel `printf`
happens before mutexes work. And `cnputc()` takes the lock **with a timeout**: if a CPU
dies while holding it, a panic message on another CPU must still get out.

## 7. Lessons

- **Boundaries are clues.** 256 = FIFO size pointed straight at the cause.
- **A crash shows the victim.** Ask who could have changed the data the crashing code
  was given.
- **Follow the subsystem's locking rules from every context you call it from.**
- **Change the design when patches keep trading bugs.**
- **Do not fix the visible symptom without evidence.** The "deaf console" that started
  the serial changes was in the Ethernet driver.
- **The console must survive everything else.** That is why kernel output bypasses the
  interrupt path, and why its lock has a timeout.

## References

- `sys/arch/arm/qcom/qcom_uart.c`
- [Section 7.4, Serial Console](../07-how-the-drivers-work.md#74-serial-console-qcom_uartc-qcommsm-uartdm-v14)
- FreeBSD: `sys/arm/qualcomm/uart_dev_msm.c` (single-character mode)
- Linux: `drivers/tty/serial/msm_serial.c`
- NetBSD: `sys/kern/tty.c` (`ttwrite()`, `ttstart()`, `tty_lock`), `sys/kern/tty_subr.c`
  (clists); manual pages `tty(4)`, `cons(9)`
