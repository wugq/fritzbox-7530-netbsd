# Chapter 9. How the Flash Edition Works

## 9.1. Synopsis

The flash edition is the NetBSD system that runs from the router's own flash and works as
a router without a PC: the `netbsd-flash.itb` image of a release. Installing and using
it is in [Chapter 2](02-installing-netbsd.md); this document explains how it is made and why it is
made that way.

After reading this chapter, you will know:

- how the system boots from the NAND without any change to U-Boot;
- how the kernel and the whole root file system fit in one 16 MB image;
- what the root file system holds and how it is built;
- what the boot script does, and how settings are kept across reboots.

Before reading this chapter, you should:

- know how the system is used ([Chapter 2](02-installing-netbsd.md), [Chapter 3](03-configuring-the-router.md));
- know the board kernel configuration ([Chapter 8](08-board-kernel-configuration.md)).

## 9.2. Booting from the NAND without Changing U-Boot

U-Boot, as OpenWrt builds it, boots like this (`printenv`):

```
bootcmd=run fritzboot
fritzboot=run nandboot || run tftpboot
nandboot=ubi part ubi && ubi read 0x85000000 kernel && bootm
```

It reads the UBI volume named `kernel` and boots it as a FIT image. Our images are FIT
images too, so a UBI volume named `kernel` holding the NetBSD image is all it takes: **no
U-Boot variable changes and no `saveenv`**. That is why installing is "create a volume
named `kernel` and write the image into it"
([Section 2.6](02-installing-netbsd.md#26-installing-into-the-flash)).

The NAND (128 MB, 128 KB blocks, 2 KB pages):

| partition | size | |
|---|---|---|
| SBL1, MIBIB, QSEE, CDT, QSEE_B | about 2 MB | Qualcomm boot and secure firmware: never touched |
| urlader0/1 | 2 x 256 KB | AVM's EVA, the way back: never touched |
| nand-tffs | 8.3 MB | AVM's configuration |
| uboot0/1 | 2 x 4 MB | U-Boot |
| ubi | 109 MB | UBI volumes |

The UBI volumes, installed without OpenWrt
([Section 2.6](02-installing-netbsd.md#26-installing-into-the-flash)):

| volume | size | |
|---|---|---|
| `kernel` | 16 MB | the NetBSD image |
| `config` | 248 KB | NetBSD's saved settings |
| `avm_config`, `avm_userdata` | 15 MB | vendor data, left alone |

AVM's two system copies, `avm_filesys_0` and `avm_filesys_1` (44 MB each), were removed
to make room. Installed next to OpenWrt
([Section 2.7](02-installing-netbsd.md#27-next-to-openwrt)), there are also `openwrt`
(3.6 MB, OpenWrt's kernel, named `kernel` before the swap) and `rootfs` (5.5 MB, its
squashfs root); OpenWrt's writable overlay `rootfs_data` (81 MB) was removed instead.

## 9.3. The Image: One Gzipped Kernel with the Root File System Inside

When the flash edition was made, NetBSD had no driver for the IPQ4019's NAND controller,
so the root file system could not live in the NAND. It is a **memory disk inside the
kernel**, the way NetBSD's installation kernels do it. (A NAND driver came later, see
[Chapter 10](10-nand-and-ubi.md); it is used for the settings, the root stays in RAM.)

- `FRITZ7530_FLASH` is the board kernel `FRITZ7530` ([Chapter 8](08-board-kernel-configuration.md))
  with `MEMORY_DISK_IS_ROOT` and `MEMORY_DISK_ROOT_SIZE=81920`: a 40 MB array
  `md_root_image` in the kernel. It also has `QCOM_ESS_ISOLATE`: the switch ports start
  apart, so that the WAN jack is not on the LAN while the router boots
  ([Chapter 7](07-how-the-drivers-work.md#vlans-and-a-wan-port)).
- `tools/mkflash.sh` fills the array with the root image (`mdsetimage -s`), turns the
  kernel into a raw binary, gzips it, and makes a FIT whose kernel is marked
  `compression = "gzip"`; U-Boot unpacks it at `bootm`.
- Programs and libraries compress well: a 37 MB root file system and a 4 MB kernel become
  a **16 MB** image.

The size matters: U-Boot's `nandboot` reads the whole volume, and a volume over about
39 MB would overwrite U-Boot itself in RAM (see the pitfalls).

Consequences of a root in RAM: nothing writes the flash during normal operation, a power
cut cannot damage anything, and every change is lost at reboot unless saved (below).
About 180 MB of the 256 MB RAM stay free.

```
KERNEL=FRITZ7530_FLASH tools/build.sh
SETS=1 tools/mkflash.sh                                         # -> tftp/netbsd-flash.itb
SETS=1 AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub tools/mkflash.sh  # with your ssh key in it
ITB=netbsd-flash.itb tools/nb-cycle.sh                          # boot it from RAM (TFTP)
```

## 9.4. The Root File System

It is NetBSD 11.1's own userland, a selection of about 230 files:

| | |
|---|---|
| `flash/files.list` | the programs, data files and directories it holds |
| `flash/root/` | our own files: `/etc/rc`, `router.conf`, `npf.conf`, `dhcpd.conf`, `saveconfig`, `rc.settings`, the button script, ... |
| `flash/mkroot-sets.py` | builds it from NetBSD's official binary sets on any host: the listed files, the shared libraries they need (`readelf`), device nodes (`MAKEDEV -s`), users (`pwd_mkdb`), owners and modes in an mtree spec for `makefs` (no root privileges needed). The CI uses it |
| `flash/mkroot.sh` | the same, run on a NetBSD system (e.g. the USB stick's), with `ldd` |

The largest parts are libcrypto (5 MB, for ssh), the DHCP server (3.4 MB) and libc
(2.3 MB).

**No secrets are in the image**, since it is published: no ssh host keys (`/etc/rc`
makes them at the first boot), no password, no authorized ssh keys unless
`AUTHORIZED_KEYS` is given to a local build.

## 9.5. The Boot Script

NetBSD's full system starts services with `rc.subr` and the scripts in `/etc/rc.d`. The
flash edition has one `/etc/rc` that does the same jobs in order, so the whole boot can be
read in one page:

1. make the root writable, mount the RAM file systems (`tmpfs`, `ptyfs`, `kernfs`,
   `procfs`), light the info LED red;
2. restore the saved settings (below) and read `/etc/router.conf`;
3. network: the LAN on `qcomedma0`; the WAN port as `vlan2` (the switch tags it, see
   [Chapter 7](07-how-the-drivers-work.md#vlans-and-a-wan-port)) with a static address, `dhcpcd` or
   `pppoe0`; then the switch ports, apart since the kernel attached the switch, may talk
   to each other (`hw.qcomess0.isolate=0`); IPv6 from the WAN with `dhcpcd` and on the LAN
   with `rtadvd`;
4. services by option: `npf` (firewall and NAT), `dhcpd`, `unbound`, `ntpd`; always `syslogd`,
   `sshd` (host keys made if missing), `powerd` (buttons);
5. info LED green.

## 9.6. How Settings Are Kept

`saveconfig` writes the files listed in `/etc/saveconfig.list` as a tar archive into
the UBI volume `config`, through the NAND driver ([Chapter 10](10-nand-and-ubi.md#settings-in-config)).
It writes whole LEBs (124 KB) at a time, so each save is one flash update per LEB.

At boot, `/etc/rc`:

1. copies the image's own versions of the listed files to `/var/run/image`;
2. if `config` starts with a tar header, unpacks it over the image's files;
   otherwise copies `/fritz/*` from a USB stick, if one is plugged in (`saveconfig -u`
   writes there; it was the way before the NAND driver);
3. prints the saved files that replaced one of the image's.

`saveconfig` leaves out every file that is the same as its copy in `/var/run/image`.
Without this, an old saved copy of a file the owner never edited (say `npf.conf`) would
hide the newer version an updated image brings. With nothing left to save, it clears the
volume.

`/etc/rc` treats a missing option in `router.conf` as "no" or empty, so a router.conf
saved before an option existed keeps working.

`/etc/rc.settings` holds the shell functions `/etc/rc` and `saveconfig` share. It finds
the `config` volume's device numbers in the boot messages (the driver prints them) and
makes `/dev/ubivolN`.

## 9.7. Services

- **Firewall and NAT** (`npf.conf`): on the WAN interface (`vlan2` or `pppoe0`),
  NPF tracks every connection that leaves (`pass stateful out`) and lets its replies
  back in; of anything else coming in, only ICMP passes (ping, and the "too big" errors
  path MTU discovery depends on). So the router's ssh and NTP, and the LAN, are not
  reachable from the WAN. One more rule lets DHCPv6's replies in (UDP from port 547 to
  port 546 of a link-local address): the request goes to the multicast address
  `ff02::1:2` and the reply comes from the server's own address, so the state of the
  request does not match it. NAT masquerades the LAN behind the WAN address. On `pppoe0`
  the TCP MSS is also clamped to 1452, since PPPoE's MTU is 1492 and LAN hosts send
  1500-byte packets. `/etc/rc` loads a copy of `npf.conf`: it adds `$lan_net` from
  `LAN_ADDR` and leaves out the lines marked `# NAT` or `# FIREWALL` whose option is
  off. Measured cost of the stateful firewall on forwarding: within the run-to-run
  variation (one flow about 550 Mbit/s, four flows 900-914 Mbit/s, with or without it).
- **DHCP** (`DHCPD=yes`): ISC dhcpd on the LAN. `/etc/rc` writes the subnet declaration
  from `LAN_ADDR` (addresses .100 to .199, the router as gateway, and as DNS server
  with `DNS=yes`) into `/var/run/dhcpd.conf`, which includes `/etc/dhcpd.conf` for
  lease times and fixed addresses. So changing `LAN_ADDR` is enough. An
  `/etc/dhcpd.conf` with a `subnet` declaration of its own is used unchanged.
- **DNS**: unbound, a recursive resolver, on the LAN addresses and localhost, answering
  the LAN only. `/etc/rc` writes the addresses into `/var/run/unbound.local.conf`;
  `<HOSTNAME>.lan` resolves to the router. No DNSSEC validation: it needs a maintained
  trust anchor and a set clock, and the router has no battery-backed clock.
- **IPv6 from the provider** (`WAN_IPV6=yes`): `dhcpcd` on `vlan2` does three jobs.
  It solicits router advertisements, and with them configures an address (SLAAC) and
  the default route; it asks DHCPv6 for an address (`ia_na`); and it asks for a prefix
  (`ia_pd 2 qcomedma0/0/64/1`: of the delegated prefix, the /64 number 0 goes on
  `qcomedma0`, with the address `::1`). For the rest of the delegated prefix it adds a
  reject route on `lo0`, so that packets to addresses no LAN uses do not go back and
  forth between the router and the provider. `/etc/rc` writes this into
  `/var/run/dhcpcd.conf`. The provider recognises the router by its DUID. The default
  kind, DUID-LLT, contains the time it was made and would be made anew at each boot,
  since `/var/db` is in RAM, and each boot could bring a new prefix. `duid ll` makes it
  from the MAC address instead, the same at every boot.
- **IPv6 on the LAN**: `rtadvd` announces the prefixes on `qcomedma0`: the delegated one
  and `LAN_ADDR6`'s. With `DNS=yes` and `LAN_ADDR6` set, it announces the router as DNS
  server (RDNSS). Without IPv6 on the WAN the router does not announce itself as a
  default router (router lifetime 0), and the clients keep their IPv6 internet traffic
  elsewhere. The delegated prefix arrives seconds after `rtadvd` has started, and may
  change later; `rtadvd` does not follow such changes by itself (see Pitfalls). So
  `/etc/dhcpcd.exit-hook`, which dhcpcd runs after each event (and `/etc/rc` once before
  it starts `rtadvd`), compares the LAN's prefixes with the last ones it saw. On a change
  it writes `rtadvd.conf` with the prefixes listed (`addr0`, `prefixlen0`, ...) and sends
  `rtadvd` a `SIGHUP`. `rtadvd` then announces the old set a few times with router
  lifetime 0, and the new one. A prefix that went away stays in the list, the last eight
  of them, announced with lifetimes 0: the hosts deprecate their addresses in it at once,
  and drop them within 2 hours (RFC 4862 does not let an advertisement cut a valid
  lifetime below 2 hours).
- **PPPoE**: `pppoe0` over `vlan2`, configured with `pppoectl`. Tested against FreeBSD's
  `pppoed` (session, CHAP, address from IPCP); NAT over it not yet end to end.
- **Time** (`NTP=yes`): `ntpd` with NetBSD's pool servers (`/etc/ntp.conf`), started
  with `-g`: the board has no battery-backed clock, so each boot starts at the time
  stored in the root file system, from when the image was made. Until ntpd has
  synchronised, log times are off.
- **Buttons**: `gpiokeys` reports the buttons as key events and `powerd` runs
  `/etc/powerd/scripts/hotkey_button` with the button's name and "pressed" or
  "released". The script times the two to tell a short press from a long one.
- **Logs**: `/etc/syslog.conf` sends `info` and up to `/var/log/messages`, `authpriv`
  to `/var/log/authlog`, errors also to the console. Without a `syslog.conf`, syslogd
  drops everything.

## 9.8. Pitfalls

- **`ubi read` without a size reads the whole volume.** With a 48 MB volume it ran from
  0x85000000 to 0x88013000 and overwrote U-Boot itself (it sits between about 0x875F0000
  and 0x88013000): the board reset while U-Boot checked the CRC (reset status 0x33; a
  normal reboot shows 0x10). U-Boot's `nandboot` reads without a size, so the `kernel`
  volume must stay below about 39 MB. That is why the image is gzipped and the volume is
  16 MB, and why the read-back check goes to 0x89000000.
- The image is 15.7 MB; the 16 MB volume holds 133 LEBs (16.9 MB).
- After removing `rootfs_data`, OpenWrt runs without saved settings until its overlay
  is recreated (`ubimkvol /dev/ubi0 -N rootfs_data -m`, which takes the rest of the
  space).
- **`rtadvd` does not pick up a prefix added after it started.** The kernel sends the
  `RTM_ADD` of the new interface route (`route monitor` shows it, with a link-level
  gateway and a netmask, as `rtadvd`'s filter wants), and `rtadvd -d -d -D` logs that it
  received it, but its prefix list does not change. Why is still open. It would not help
  much anyway: `add_prefix()` in `config.c` announces a prefix found this way without the
  autonomous flag, so the hosts would not make addresses in it. Hence the dhcpcd hook
  above, which lists the prefixes in the configuration.
- **`rtadvd` reads its configuration again inside its chroot.** On `SIGHUP` it opens
  the file given with `-c` after it has chrooted to `/var/chroot/rtadvd`. Without a copy
  there, it falls back to the defaults and drops `rltime#0` and `rdnss`. `/etc/rc`
  puts a copy at `/var/chroot/rtadvd/var/run/rtadvd.conf`.
- **A changed prefix has to be withdrawn.** Left out of the announcements, the old
  prefix stays usable on the hosts for `rtadvd`'s default preferred lifetime, 7 days,
  and connections from their addresses in it fail. The hook announces it with lifetimes
  0 instead. It remembers gone prefixes only until the router reboots; with `duid ll`
  the provider normally delegates the same prefix again after a reboot.
- **NetBSD's `awk` takes no newline in `-v var=value`** ("newline in string"); GNU awk
  does, so the hook passed a test on the PC and failed on the router. It passes the
  list in the environment (`ENVIRON`).
- **dhcpcd's hooks.** Without `/libexec/dhcpcd-run-hooks` and its
  `dhcpcd-hooks/20-resolv.conf`, dhcpcd never writes the provider's DNS servers into
  `/etc/resolv.conf` (it logs "No such file or directory" for the script). With
  `DNS=yes` that does not matter, since the router resolves by itself. `saveconfig`
  leaves out a `resolv.conf` dhcpcd wrote, as it does the one `/etc/rc` writes.
- NetBSD's unbound is built for a chroot: without `-c` it looks for
  `/var/chroot/unbound/etc/unbound/unbound.conf`. rtadvd chroots to `/var/chroot/rtadvd`,
  which must exist. NPF loads `normalize` from `/lib/npf/ext_normalize.so`.

Building the root image met a series of small problems:

| symptom | cause | fix |
|---|---|---|
| image 49 MB | `cp` followed symbolic links (libc stored twice); `/etc/services` is 3.4 MB | links kept as links; a short `services` |
| `/etc/rc: ... read-only file system` | init mounts the root read-only | `mount -uw /` first |
| login prompt shows as `loi:` | no `/etc/gettytab`, getty's defaults use parity | add gettytab |
| console login: "pam_start failed" | `login` uses PAM; the modules are loaded at run time, `ldd` misses them | copy `/etc/pam.d` and `/usr/lib/security`; resolve libraries of everything copied |
| `ping: not found` after login | no `/root/.profile`, `/sbin` not in PATH | add it; `SetEnv PATH` in sshd_config for ssh commands |
| ssh key login refused | files owned by the build user: sshd's StrictModes found `/root` not owned by root | owners from an mtree spec (root:wheel) |
| reading a `.tgz` member by member was very slow | each read decompresses from the start | unpack the sets once |
