# NetBSD on the FRITZ!Box 7530: the Handbook

Part I is for running NetBSD on the router. Part II explains how the port works, for
reading along with the code; each chapter builds on the ones before it.

Commands are shown with the prompt of the place they run in: `%` on the PC, `#` as root
on the router (NetBSD or OpenWrt), `(FRITZ7530) #` at the U-Boot prompt.

## Part I. Using

1. [Installing U-Boot](01-installing-uboot.md)
   - how the box boots; the serial console; backing up the flash; U-Boot and OpenWrt; recovery
2. [Installing NetBSD](02-installing-netbsd.md)
   - the release images; the TFTP server; trying NetBSD from RAM; installing into the flash
3. [Configuring the Router](03-configuring-the-router.md)
   - ssh; a home router behind a modem; keeping settings; `router.conf`; LEDs, buttons, logs
4. [Updating and Recovery](04-updating-and-recovery.md)
   - updating; booting OpenWrt; a router that does not boot

## Part II. How It Works

5. [The Hardware](05-the-hardware.md)
   - the board; the IPQ4019 system-on-chip; the Cortex-A7 and how it compares; the memory
     map; which driver serves which hardware block
6. [Building from Source and Testing](06-building-and-testing.md)
   - the repository over NetBSD's tree; the device tree; building; booting from TFTP and a
     USB stick; the test bench
7. [How the Drivers Work](07-how-the-drivers-work.md)
   - the NetBSD driver pattern; platform and SMP; serial; clocks and resets; USB; GPIO;
     Ethernet; the switch
8. [The Board Kernel Configuration](08-board-kernel-configuration.md)
   - `FRITZ7530`; hidden dependencies; profiling with `tprof`
9. [How the Flash Edition Works](09-how-the-flash-edition-works.md)
   - booting from the NAND; the image; the root file system; the boot script; settings
10. [NAND and UBI](10-nand-and-ubi.md)
   - the QPIC controller without DMA; page layout and ECC; UBI

## Articles

Case studies of problems met during the port: the symptom, the investigation with its
wrong turns, the cause, the fix, and the machinery behind it. Each stands on its own.

- [Profiling Packet Forwarding with tprof: Where Do the Cycles Go?](articles/profiling-packet-forwarding-with-tprof.md)
  - performance counters, interrupt controllers, thread priorities, how a mutex behaves
    on a multiprocessor; two NetBSD bugs and a global lock in the forwarding path
- [Receiving Packets without Drowning: Interrupts, Threads and a Hang](articles/receiving-packets-without-drowning.md)
  - descriptor rings, receive livelock and NAPI, why TCP retransmits reordered packets,
    a work item queued twice
- [Counting Cycles: Making an Ethernet Driver Fast](articles/counting-cycles.md)
  - a cycle budget per packet, device register reads, lock contention and per-CPU queues,
    RSS hashing, cache maintenance for DMA, Ethernet flow control
- [Inherited Hardware: When the Boot Loader Was There First](articles/inherited-hardware.md)
  - what survives a hand-over, undo versus reset, a serial link that must lock on, the
    NAND controller's DMA mode, overwriting U-Boot in memory
- [A Serial Console Driver: from Polling to Interrupts](articles/a-serial-console-driver.md)
  - FIFOs and watermarks, a 256-character hang, hardware and software interrupts, a race
    on the terminal's queue
- [When It Is Not the Software: the Test Bench](articles/when-it-is-not-the-software.md)
  - back-powering through a serial adapter, loose wires, link downshifts, a checklist
