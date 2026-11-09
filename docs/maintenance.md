# Chapter 4. Updating and Recovery

## 4.1. Synopsis

After reading this chapter, you will know:

- how to update NetBSD to a new release;
- how to boot OpenWrt, or make it the default again;
- how to recover a router that does not boot.

Before reading this chapter, you should have installed NetBSD into the flash
([Chapter 2](install.md)) and know how to reach the U-Boot prompt and run the TFTP server
([Section 2.4](install.md#24-setting-up-the-tftp-server), [Section 2.5](install.md#25-trying-netbsd-from-ram)).

## 4.2. Updating NetBSD

Try the new image from RAM first ([Section 2.5](install.md#25-trying-netbsd-from-ram)).
Then, at the U-Boot prompt, with the new image on the TFTP server:

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

The saved settings stay in the `config` volume and apply to the new system.

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

## 4.3. Booting OpenWrt

OpenWrt stays installed in the volume `openwrt`. To boot it once, at the U-Boot prompt:

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
([Section 2.6](install.md#26-installing-into-the-flash)). To give it one again, from
OpenWrt: `ubimkvol /dev/ubi0 -N rootfs_data -m` (it takes all the free space).

## 4.4. When the Router Does Not Boot

**Power LED on, no output on the serial console after a power cycle.** The USB serial
adapter can keep the board half powered through its data line, so it never resets
properly. Unplug the router and the adapter, wait ten seconds, plug in the router first,
then the adapter.

**The info LED stays red.** NetBSD started but its boot script did not finish. The
serial console shows the last message. Boot a known-good image from RAM
([Section 2.5](install.md#25-trying-netbsd-from-ram)) and update the flash from there.

**U-Boot does not start, or the flash is damaged.** EVA, AVM's boot loader, is never
changed by these instructions. It can always start U-Boot and OpenWrt from RAM again, and
OpenWrt can repair the flash: see [Section 1.7](uboot.md#17-recovery).
