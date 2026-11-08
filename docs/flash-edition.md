# Step 6: the flash edition

NetBSD in the router's NAND: it boots without a PC, TFTP or a USB stick and works as a
router on its own.

## No U-Boot change needed

U-Boot's boot command (`printenv`):

```
bootcmd=run fritzboot
fritzboot=run nandboot || run tftpboot
nandboot=ubi part ubi && ubi read 0x85000000 kernel && bootm
```

U-Boot boots the FIT image in the UBI volume named `kernel`. Our images are FIT images, so
**no U-Boot variable changes and no `saveenv`**: a UBI volume named `kernel` holding the
NetBSD image is all it takes.

The NAND (128 MB, 128 KB blocks, 2 KB pages):

| partition | size | |
|---|---|---|
| SBL1, MIBIB, QSEE, CDT, QSEE_B | about 2 MB | Qualcomm boot and secure firmware: **never touched** |
| urlader0/1 | 2 x 256 KB | AVM's EVA, the way back: **never touched** |
| nand-tffs | 8.3 MB | AVM's configuration |
| uboot0/1 | 2 x 4 MB | U-Boot |
| ubi | 109 MB | UBI (OpenWrt put its volumes here) |

With OpenWrt installed, the UBI volumes were `kernel` (3.6 MB, OpenWrt's FIT), `rootfs`
(5.5 MB), `avm_config` and `avm_userdata` (vendor data, left alone) and `rootfs_data`
(81 MB, OpenWrt's writable overlay).

## The image: one gzipped kernel with the root file system inside

NetBSD had no driver for the IPQ4019's NAND controller (QPIC; see [nand.md](nand.md) for
the one written since), so the root file system cannot live in the NAND under NetBSD's
control. It is a **memory disk** inside the kernel, the way NetBSD's install kernels do it:

- `FRITZ7530_FLASH`: `FRITZ7530` without `MEMORY_DISK_DYNAMIC` (a memory disk handed over
  by the boot loader), with `MEMORY_DISK_ROOT_SIZE=81920` (a 40 MB array `md_root_image` in
  the kernel) and `MEMORY_DISK_IS_ROOT`;
- `tools/mkflash.sh` fills the array with the root image (`mdsetimage -s`), turns the
  kernel into a raw binary, gzips it, and makes a FIT whose only kernel is marked
  `compression = "gzip"` (`tools/mkfit.py` does that for `*.gz`); U-Boot unpacks it;
- programs and libraries compress well: a 35 MB root file system and a 4 MB kernel become
  a **14.5-15 MB** image.

The root file system:

| | |
|---|---|
| `flash/files.list` | the programs, data files and directories it holds |
| `flash/root/` | our own files: `/etc/rc` (a boot script that fits on one page), `router.conf` (the router's settings), `npf.conf` (NAT), `dhcpd.conf`, `ttys`, `ssh/sshd_config`, `saveconfig`, `rc.settings`, `/root/.profile` |
| `flash/mkroot-sets.py` | builds the image from NetBSD's official binary sets, on any host: files from the list, the shared libraries they need (`readelf`), device nodes (the sets' `MAKEDEV -s`), users (`pwd_mkdb`), owners and modes in an mtree spec for `makefs` (no root needed). The CI uses it |
| `flash/mkroot.sh` | the same, run on a NetBSD system (e.g. the USB stick's), with `ldd` |

About 217 files and 31 MB of data; the largest are libcrypto (5 MB, for ssh), the DHCP
server (3.4 MB) and libc (2.3 MB). About 180 MB of RAM stay free after boot.

**No ssh host keys are in the image** (it is public; everyone would have the same keys):
`/etc/rc` creates them at the first boot and `saveconfig` keeps them. Root has no password
on the serial console; ssh accepts keys only (`/root/.ssh/authorized_keys`, saved with
`saveconfig`).

```
KERNEL=FRITZ7530_FLASH tools/build.sh
SETS=1 tools/mkflash.sh                          # -> tftp/netbsd-flash.itb
SETS=1 AUTHORIZED_KEYS=~/.ssh/id_ed25519.pub tools/mkflash.sh   # with your ssh key
ITB=netbsd-flash.itb tools/nb-cycle.sh           # try it from RAM first (TFTP)
```

## Settings

The root is in RAM: changes are lost at reboot unless saved. `/etc/router.conf`:

```
HOSTNAME=fritzbox
LAN_ADDR="192.168.77.1/24"   # qcomedma0: every switch port but the WAN one
WAN_PORT=3                   # switch port number, 0 = no WAN
WAN_MODE=static              # static or dhcp
WAN_ADDR="192.168.78.1/24"
WAN_GW=""
NAT=no                       # masquerade the LAN behind the WAN (/etc/npf.conf)
DHCPD=no                     # DHCP server on the LAN (/etc/dhcpd.conf)
NTP=no
```

`saveconfig` saves the files in `/etc/saveconfig.list` (router.conf, npf.conf,
dhcpd.conf, passwords, ssh host keys, root's authorized keys, ...) as a tar archive into
the NAND, in the UBI volume `config` ([nand.md](nand.md)); `/etc/rc` unpacks it at boot,
over the image's copies.

```
vi /etc/router.conf
saveconfig
reboot
```

The volume is made once, from U-Boot (it uses free space in the UBI partition and touches
no other volume):

```
ubi part ubi
ubi create config 0x3e000                # two LEBs, 248 KB
```

Without that volume, or while it holds no settings, a USB stick is used instead:
`saveconfig -u` copies the files to `/fritz/` on the stick (FFS `sd0a`, or FAT
`sd0e`/`sd0i`), and `/etc/rc` waits up to 8 s for a USB disk and copies `/fritz/*` back.
`saveconfig -n` shows what would be saved. `/etc/rc.settings` holds the shell functions
both scripts use: it finds the volume's numbers in the boot messages and makes
`/dev/ubivolN`.

## Installing it

From the U-Boot prompt (`UBOOT_ONLY=1 tools/nb-cycle.sh` gets there), with the image on
the TFTP server. **This writes the flash.**

First time, next to OpenWrt (OpenWrt's writable overlay `rootfs_data` has to go to make
room; back up OpenWrt's settings first with `sysupgrade -b`):

```
ubi part ubi
ubi remove rootfs_data
ubi create netbsd 0x1000000              # 16 MB
setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
tftpboot 0x85000000 netbsd-flash.itb
ubi write 0x85000000 netbsd ${filesize}
ubi read 0x88000000 netbsd ${filesize}; cmp.b 0x85000000 0x88000000 ${filesize}
ubi read 0x85000000 netbsd; bootm 0x85000000    # try it
```

Then make it the default by swapping volume names, from OpenWrt (U-Boot cannot rename):

```
ubirename /dev/ubi0 kernel openwrt netbsd kernel
```

Updating later (U-Boot):

```
ubi part ubi
tftpboot 0x85000000 netbsd-flash.itb
ubi write 0x85000000 kernel ${filesize}
```

| to | do |
|---|---|
| boot OpenWrt | U-Boot: `ubi part ubi; ubi read 0x85000000 openwrt; bootm 0x85000000` |
| make OpenWrt the default again | OpenWrt: `ubirename /dev/ubi0 kernel netbsd openwrt kernel` |
| boot a development kernel | `tools/nb-cycle.sh` as before (it stops U-Boot's countdown) |

From power-on to the login prompt takes about 45 s (28 s from `bootm`).

## Pitfalls

- **`ubi read` without a size reads the whole volume.** With a 48 MB volume that ran from
  0x85000000 to 0x88013000 and overwrote U-Boot itself (it sits between about 0x875F0000
  and 0x88013000): the board reset while U-Boot checked the CRC (reset status 0x33; a normal
  reboot shows 0x10). U-Boot's default `nandboot` reads without a size, so the volume
  holding the default image must stay below about 39 MB. That is why the image is gzipped
  and the volume is 16 MB.
- The hardware watchdog was not the cause (disabling it changed nothing).
- After `ubi remove rootfs_data`, OpenWrt does not recreate its overlay by itself: it runs
  without saved settings until `ubimkvol /dev/ubi0 -N rootfs_data -m`.

Building the root image met a series of small problems, each now handled:

| symptom | cause | fix |
|---|---|---|
| image 49 MB | `cp` followed symbolic links (libc stored twice); `/etc/services` is 3.4 MB | links kept as links; a short `services` |
| `/etc/rc: ... read-only file system` | init mounts the root read-only | `mount -uw /` first |
| login prompt shows as `loi:` | no `/etc/gettytab`, getty's defaults use parity | add gettytab |
| console login: "pam_start failed" | `login` uses PAM; the modules are loaded at run time, `ldd` misses them | copy `/etc/pam.d` and `/usr/lib/security`; resolve libraries of everything copied |
| `ping: not found` after login | no `/root/.profile`, `/sbin` not in PATH | add it; `SetEnv PATH` in sshd_config for ssh commands |
| ssh key login refused | files owned by the build user: sshd's StrictModes found `/root` not owned by root | owners from an mtree spec (root:wheel) |
| reading a `.tgz` member by member was very slow | each read decompresses from the start | unpack the sets once |
