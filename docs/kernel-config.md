# Step 5: a kernel configuration for this board (`FRITZ7530`)

GENERIC must boot on every armv7 board NetBSD supports, so it carries a dozen SoCs'
drivers, displays, sound, PCI, SD cards, hundreds of USB devices... This board uses about
twenty kinds of device. `FRITZ7530` is a kernel configuration for it alone.

| | GENERIC | FRITZ7530 |
|---|---|---|
| kernel image (`netbsd-*.bin`) | 10.15 MB | **3.90 MB** |
| text / data / bss | 7.7 / 2.5 / 1.3 MB | 3.0 / 0.8 / 0.8 MB |
| object files built | 1747 | 677 |

Functions and speed are the same (all `tools/hwtest.sh` checks pass, routing as fast).
It is the base of the flash edition ([flash-edition.md](flash-edition.md)).

## Writing a board configuration

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

## What the first build revealed

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

## Use

```
KERNEL=FRITZ7530 tools/build.sh                    # -> tftp/netbsd-usb-FRITZ7530.itb
ITB=netbsd-usb-FRITZ7530.itb tools/nb-cycle.sh
ITB=netbsd-usb-FRITZ7530.itb tools/hwtest.sh "notes"
```

Kernels other than GENERIC get their name in the image names, so both kinds can sit in
the TFTP directory side by side.
