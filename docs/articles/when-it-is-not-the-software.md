# When It Is Not the Software: the Test Bench

## Abstract

Several of the hardest problems in this port were not in the code at all. A router that
"hung" was half powered through a serial cable. A dead console was a loose jumper wire.
A slow network was a broken network port on the test machine. Each cost hours because the
first suspect was the newest code.

This article collects them, with the observations that gave each one away, and ends
with a checklist of things to rule out before debugging software.

What you should know: nothing in particular. [Chapter 1](../01-installing-uboot.md#13-connecting-the-serial-console)
shows how the serial console is connected.

## 1. The Router That Would Not Come Back: Back-Powering

### 1.1. Symptom

Three times in one day: after a power cycle, the router's power LED was on, but the
serial console printed nothing and the network ports were dead. Unplugging and plugging
in the power supply again often gave the same result.

### 1.2. The Observation

The owner noticed it: **when the router's power was pulled, the USB serial adapter's LED
stayed lit for a few seconds** before going out. The adapter is powered by the PC, not by
the router, so its LED should not care about the router's power at all.

### 1.3. Cause

The adapter's transmit line, connected to the router's receive pin, sits at 3.3 V when
idle. Chips have protection diodes from their pins to their supply rail. With the
router's supply removed, current can flow from the adapter's transmit line through such
a diode into the router's supply rail, enough to keep parts of the chip half alive. The
LED staying on was that current. When power returned, a half-powered chip did not get a
clean power-on reset, and the board did not start. (Connecting the adapter's VCC pin
would make this much worse; it must stay disconnected.)

### 1.4. Fix

A procedure, not a patch: for a full power cycle, **unplug the adapter too**. Pull the
router's power, unplug the adapter from the PC, wait ten seconds, plug the router in,
then the adapter. The next power cycle worked at once, and this is now in
[Chapter 1](../01-installing-uboot.md#13-connecting-the-serial-console) and
[Chapter 4](../04-updating-and-recovery.md#45-when-the-router-does-not-boot).

> [!NOTE]
> **A detail that does not fit is often the answer.** An LED that stays on without power
> said "there is another power path" more clearly than any log.

## 2. The Console That Stopped Mid-Line: Loose Wires

The serial logger was running and the device existed, but not a byte arrived; or the
output stopped halfway through a line. The jumper wires on the router's pin header had
worked loose, pulled by the cable when the USB end was replugged.

Software cannot see this. A UART with nothing on its receive pin simply receives
nothing, and a wire making intermittent contact produces partial output or noise. The
fix: press each wire, TX, RX and GND, back onto its pin, and check the wires first next
time.

## 3. EVA Stops at Its Prompt: Noise at Power-On

Sometimes the router did not boot at all: the power LED blinked slowly and the console's
last line was `Eva_AVM >`. Once even that line was missing, and the log ended in the
middle of the NAND messages, which looked like a broken serial line.

AVM's boot loader, EVA, stops when it receives a key press in a short window at
power-on. The test scripts send spaces to stop U-Boot's countdown, so they were the
first suspect. But EVA stopped also when no script was running. The likely cause is
noise on the receive line when the router powers up, interpreted as a key. Typing `go`
continues the boot, and `tools/nb-cycle.sh` does that whenever it sees the prompt.

## 4. A Test Machine That Ran at 100 Mbit/s

The first second test machine, an HP t5730 thin client, never got above 100 Mbit/s. The
router's PHY reported:

```
Downshift occurred from negotiated speed 1Gbps to actual speed 100Mbps
```

Gigabit Ethernet needs all four wire pairs; 100 Mbit/s needs two. A PHY that cannot get
a clean signal on all four pairs **downshifts**. Three different cables gave the same
result, so the thin client's own port was faulty. (Flat cables cause the same, since
some have only two pairs.) It was replaced by a ThinkCentre M93p.

## 5. A PC That Fell to 10 Mbit/s

After a router reboot, the PC sometimes could not reach it at all. The router reported
its port at 10 Mbit/s; the PC reported no carrier, and its kernel log said
`e1000e ... Failed to enable ULP`.

The PC's Intel network card enters an **ultra low power** mode when the link drops,
which is exactly what a router reboot does. In that mode it negotiates only 10 Mbit/s,
and then believes it has no link. Re-activating the connection (`nmcli con up
fritz-netbsd`) brings it back to 1000 Mbit/s. `tools/nb-cycle.sh` now does that between
TFTP attempts.

## 6. Measuring the Measuring Tool

On OpenWrt, BusyBox's `nc` was first used to measure throughput, and it filled a CPU
core by itself: it reads and writes in small blocks. The number measured was `nc`'s
speed, not the network's. From then on `iperf3` was used on both sides, for every
system, so the numbers could be compared.

## 7. The Logger That Read a Dead Device

The test scripts read the router's console from a log file written by a logger process.
After the router was power-cycled, or the adapter replugged, the log stopped growing,
while the logger kept running without an error.

When a USB serial adapter is replugged, Linux removes `/dev/ttyUSB0` and creates a new
one. A process that had the old one open keeps a file descriptor to a device that no
longer exists. Reads just block; nothing tells it to reopen. The logger
(`tools/serial-log.sh`) now checks every two seconds whether the device node's inode
has changed, and if so reopens it.

(Related: two programs reading one serial port split the data between them. A terminal
program left open next to the logger makes the log miss characters.)

## 8. A Kill Command That Killed Itself

`pkill -f pattern` matches the pattern against every process's full command line. A
script that runs `pkill -f 'serial-log'` from a shell whose own command line contains
`serial-log` kills that shell too; the script dies with exit code 144.
Kill by process ID, or choose a pattern that cannot match the caller.

## 9. A Checklist Before Debugging Software

When something fails that worked before, and especially when it fails intermittently:

1. **Power**: was it a clean power cycle? Is anything else (a serial adapter, a USB
   device) feeding current into the board?
2. **Wires**: are the serial wires on their pins? Does the network link show the
   expected speed on both ends (`ifconfig`, the switch's port status)?
3. **The other end**: is the PC's or the test machine's port healthy, at the right
   speed, with the right routes and firewall?
4. **The tools**: is the logger still reading the current device? Is the measuring tool
   itself the bottleneck?
5. **The boot path**: did it stop somewhere before your code (EVA, U-Boot)?

Only then: what changed in the software?

## References

- [Section 1.3, Connecting the Serial Console](../01-installing-uboot.md#13-connecting-the-serial-console)
- [Section 6.6, Automation](../06-building-and-testing.md#66-automation): `nb-cycle.sh`,
  `serial-log.sh`
