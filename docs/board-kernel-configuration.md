# Chapter 7. The Board Kernel Configuration

## 7.1. Synopsis

GENERIC must boot on every armv7 board NetBSD supports, so it carries a dozen SoCs'
drivers, displays, sound, PCI, SD cards, hundreds of USB devices... This board uses about
twenty kinds of device. `FRITZ7530` is a kernel configuration for it alone.

| | GENERIC | FRITZ7530 |
|---|---|---|
| kernel image (`netbsd-*.bin`) | 10.15 MB | **3.90 MB** |
| text / data / bss | 7.7 / 2.5 / 1.3 MB | 3.0 / 0.8 / 0.8 MB |
| object files built | 1747 | 677 |

Functions and speed are the same (all `tools/hwtest.sh` checks pass, routing as fast).
It is the base of the flash edition ([how-the-flash-edition-works.md](how-the-flash-edition-works.md)).

After reading this chapter, you will know:

- why a board configuration, and how one is written;
- what building it revealed about hidden dependencies;
- how to profile the kernel with `tprof`, and what that found in the forwarding path.

Before reading this chapter, you should:

- have built and booted a kernel ([Chapter 5](building-and-testing.md)).

## 7.2. Writing a Board Configuration

NetBSD's habit: GENERIC for all boards, small configurations for specific ones (the
Raspberry Pi's `RPI`, ...). A configuration is input for `config(1)`:

- `include "std.xxx"`: the machine's standard options (here `std.generic`: FDT, modules,
  memory-disk root, the kernel's load address, ...);
- `include "files.xxx"`: which source files exist (see below);
- `options XXX`: compile-time switches (`INET6`, `DDB`, `MULTIPROCESSOR`, ...);
- `file-system XXX`, `pseudo-device xxx` (no hardware behind it: `vlan`, `npf`, `pty`, ...);
- `device* at parent?`: where drivers attach, e.g. `qcomedma* at fdt?`.

**Picking the devices**: every `xxx0 at yyy0` line of the boot log is a device that
attached. Their GENERIC lines are copied, plus the pseudo-devices and options a router
needs. What stays and what goes is commented in `sys/arch/evbarm/conf/FRITZ7530`:

- **kept**: every device on the board (buttons, LEDs, CPU frequency, USB storage, NAND);
  IPv4/IPv6, `GATEWAY` fast forwarding, npf, vlan, bridge, PPPoE, tun/tap/gif/gre, carp;
  FFS/FAT/tmpfs/NFS client; ddb with the symbol table (worth it when something panics);
  `tprof` for sampling where the CPUs spend their time;
- **left out**: other SoCs, display/sound/PCI/SD, most USB devices, RAID/cgd/vnd, the NFS
  server, quotas, DTrace, `COMPAT_*` (NetBSD 11 userland needs none), IPsec;
- to get something back, copy its line from GENERIC or GENERIC.common (USB network
  adapters, e.g. `ure*`, `axe*`, are in `dev/usb/usbdevices.config`).

## 7.3. What the First Build Revealed

A small configuration exposed dependencies GENERIC had hidden:

1. **"has no configurations!"**: a configuration needs `config netbsd root on ? type ?`
   (the root comes from the boot arguments, `root=sd0a`, or a memory disk).
2. **`files.generic` cannot be used**: it includes every SoC's files, and Broadcom's
   `bcm283x_platform.c` stops with `#error` unless `SOC_BCM2835/2836` is set; only GENERIC
   uses it. `files.fritz7530` is `files.generic` with only Qualcomm's `files.qcom`.
3. **`tprofattach` without a prototype**: `armpmu` (performance counters) needs
   `pseudo-device tprof`; in GENERIC the two lines are next to each other.
4. **Undefined `psci_call_smc`, `psci_fdt_reset`**: the platform code borrowed two PSCI
   functions; GENERIC happened to include the PSCI driver.
   - `psci_fdt_reset` was the reset fallback, but this board has no PSCI firmware: it
     could only print "PSCI: reset failed". Reboot is done by `qcom_pshold`. Removed.
   - `psci_call_smc` is one `smc #0` instruction, used to tell the secure firmware where
     the secondary cores start. Replaced by `ipq4019_scm_call()` (Linux's
     `qcom_scm-legacy.c` register convention: command in r0, arguments in r1-r3, result
     in r0).
5. **Undefined `wskbd_input`**: `gpiokeys` reports keys as keyboard events: `wskbd` and
   `wsmux` are needed.

## 7.4. Using It

```
KERNEL=FRITZ7530 tools/build.sh                    # -> tftp/netbsd-usb-FRITZ7530.itb
ITB=netbsd-usb-FRITZ7530.itb tools/nb-cycle.sh
ITB=netbsd-usb-FRITZ7530.itb tools/hwtest.sh "notes"
```

Kernels other than GENERIC get their name in the image names, so both kinds can sit in
the TFTP directory side by side.

## 7.5. Profiling with tprof

The board kernels have `armpmu` and `tprof`, and the flash edition carries `tprof(8)`:

```
tprof list                                         # events: CPU_CYCLES, INST_RETIRED, ...
tprof monitor -e CPU_CYCLES -o /tmp/p.prof sleep 8 # sample all CPUs for 8 seconds
tprof analyze -s /tmp/p.prof | head -40            # hottest functions
```

`tprof analyze` columns: percentage, samples, then one column per event, then pid, lwp,
cpu and k (1: kernel).

Two NetBSD problems kept it from seeing forwarding; patches 0005 and 0006 fix them:

1. **One CPU of four.** The PMU interrupt is a PPI, set up from `config_interrupts()`
   after the secondary CPUs started. PPI priorities are banked per CPU, and the GIC driver
   wrote the priority on the establishing CPU only: the others kept 0xff, which is never
   signalled. Symptom: `vmstat -i` shows `irq 23` on one CPU only.
2. **The forwarding CPUs counted nothing.** `tprof` started its counters with
   low-priority cross-calls, run by each CPU's `xcall` thread (PRI_XCALL). Under
   forwarding load the EDMA receive workqueue (PRI_SOFTNET) keeps a CPU 100% busy and
   the `xcall` thread never runs there, so the counters stayed at their reset value.
   Found by having the driver program a counter of its own around `edma_rx()`: it
   counted 99% of the cycles while tprof's counter 0 did not move.

Remaining caveat: the PMU interrupt still arrives late in parts of the receive path, so
samples there are fewer than the cycle count implies and some land on `splx`.

What the first profiles showed (4 flows): about 40% of the samples are softnet threads
spinning in `mutex_enter`: `ipflow_fastforward()` holds the global `ipflow_lock` across
the whole forward, `if_output_lock()` included, so the fast path forwards one packet at
a time however many CPUs receive.

Patch 0007 sends without the lock (the route is held by a reference, the destination
copied, since the flow may be freed once the lock is dropped). With 4 flows:

| | before | patch 0007 |
|---|---|---|
| throughput | 816 Mbit/s | 914 Mbit/s (line rate) |
| cycles, 4 CPUs, 8 s | 16.2 G | 12.9 G |
| cycles per Mbit/s | 19.9 M | 14.1 M (-29%) |
| samples spinning on a lock | 39% | 4% |

hwtest: all checks pass; LAN to WAN (vlan2) 1 flow 563 -> 626, 4 flows 818 -> 914 Mbit/s.
