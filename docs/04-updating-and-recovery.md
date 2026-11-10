# Chapter 4. Updating and Recovery

## 4.1. Synopsis

After reading this chapter, you will know:

- how to update NetBSD to a new release over ssh, and go back to the previous one;
- how to update it from U-Boot;
- how to boot OpenWrt, or make it the default again;
- how to recover a router that does not boot.

Before reading this chapter, you should have installed NetBSD into the flash
([Chapter 2](02-installing-netbsd.md)) and be able to log in with ssh
([Section 3.3](03-configuring-the-router.md#33-allowing-ssh-logins)). Updating from U-Boot also
needs the serial console and the TFTP server
([Section 2.4](02-installing-netbsd.md#24-setting-up-the-tftp-server), [Section 2.5](02-installing-netbsd.md#25-trying-netbsd-from-ram)).

## 4.2. Updating NetBSD

Check the download ([Section 2.3](02-installing-netbsd.md#23-the-release-images)), then send it
to the router's `sysupgrade`:

```
% ssh root@192.168.77.1 sysupgrade - < netbsd-flash.itb
sysupgrade: image 15780770 bytes, SHA256 fdaf317d...
sysupgrade: writing /dev/ubivol6 ("upgrade")
sysupgrade: read back, same SHA256
sysupgrade: the new system is "kernel"; the old one is "upgrade"
sysupgrade: rebooting
```

It takes about 25 seconds, and the router is back about 45 seconds later with the new
system. With `-n` it does not reboot. It refuses a file that is not a flash edition
image (`netbsd-usb-FRITZ7530.itb`, a truncated download).

What it does: the flash holds the running system in the UBI volume named `kernel`, the
one U-Boot boots. `sysupgrade` writes the new image into a second volume, `upgrade`
(made the first time, the size of `kernel`), reads it back and compares the SHA-256,
and then **swaps the two volumes' names**. The swap is a single write of UBI's volume
table, so if the power fails at any moment, the router boots either the old system or
the new one, never a half-written image. The settings in the `config` volume are not
touched.

**Going back.** The previous system stays in `upgrade`. To make it the one that boots
again:

```
# sysupgrade -r
```

`sysupgrade -r` once more goes forward again. To try the previous system once without
changing anything, at the U-Boot prompt: `ubi part ubi`, `ubi read 0x85000000 upgrade`,
`bootm 0x85000000`.

> [!NOTE]
> `sysupgrade` is in the images from v0.1.4 on. To update from an older system, boot
> the new image from RAM ([Section 2.5](02-installing-netbsd.md#25-trying-netbsd-from-ram)),
> log in, and run `sysupgrade` there: it is the running system's `sysupgrade` that
> writes the flash. Or update from U-Boot ([Section 4.3](#43-updating-from-u-boot)).

To try a new image before installing it, boot it from RAM
([Section 2.5](02-installing-netbsd.md#25-trying-netbsd-from-ram)) first.

### 4.2.1. Settings Saved by an Older System

At boot the console lists the saved files that replace one of the image's:

```
flash edition: saved, not the image's: /etc/router.conf /root/.ssh/authorized_keys
```

If the list names a file you never edited, it is an old copy that hides the new
image's version. Drop it by copying the image's version back and saving:

```
# cp /var/run/image/etc/npf.conf /etc/npf.conf
# saveconfig
```

## 4.3. Updating from U-Boot

Without ssh, at the U-Boot prompt, with the new image on the TFTP server:

> [!WARNING]
> This writes the flash.

```
(FRITZ7530) # ubi part ubi
(FRITZ7530) # setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
(FRITZ7530) # tftpboot 0x85000000 netbsd-flash.itb
(FRITZ7530) # ubi write 0x85000000 kernel ${filesize}
(FRITZ7530) # ubi read 0x89000000 kernel ${filesize}
(FRITZ7530) # cmp.b 0x85000000 0x89000000 ${filesize}
(FRITZ7530) # reset
```

This overwrites the running system in place: unlike `sysupgrade`, a power failure
during `ubi write` leaves no bootable system in the flash (U-Boot and the TFTP server
still work: write it again). The saved settings stay in the `config` volume.

## 4.4. Booting OpenWrt

If NetBSD was installed next to OpenWrt
([Section 2.7](02-installing-netbsd.md#27-next-to-openwrt)), OpenWrt stays in the volume
`openwrt`. To boot it once, at the U-Boot prompt:

```
(FRITZ7530) # ubi part ubi
(FRITZ7530) # ubi read 0x85000000 openwrt
(FRITZ7530) # bootm 0x85000000
```

To make it the default again, from OpenWrt:

```
# ubirename /dev/ubi0 kernel netbsd openwrt kernel
```

OpenWrt then runs without saved settings, since its `rootfs_data` volume was removed
([Section 2.7](02-installing-netbsd.md#27-next-to-openwrt)). To give it one again, from
OpenWrt: `ubimkvol /dev/ubi0 -N rootfs_data -m` (it takes all the free space).

## 4.5. When the Router Does Not Boot

**Power LED on, no output on the serial console after a power cycle.** The USB serial
adapter can keep the board half powered through its data line, so it never resets
properly. Unplug the router and the adapter, wait ten seconds, plug in the router first,
then the adapter.

**The info LED stays red.** NetBSD started but its boot script did not finish. The
serial console shows the last message. Boot a known-good image from RAM
([Section 2.5](02-installing-netbsd.md#25-trying-netbsd-from-ram)) and update the flash from there.

**U-Boot does not start, or the flash is damaged.** EVA, AVM's boot loader, is never
changed by these instructions. It can always start U-Boot from RAM again, and from there
NetBSD or OpenWrt: see [Section 1.9](01-installing-uboot.md#19-recovery).
