# Chapter 2. Installing NetBSD

## 2.1. Synopsis

This chapter installs the NetBSD router system into the flash of a FRITZ!Box 7530 and
makes it the system the router boots.

After reading this chapter, you will know:

- how U-Boot finds the system to boot;
- which release image to use;
- how to try NetBSD from RAM without changing the flash;
- how to make room in the flash and install NetBSD there;
- how to install it next to OpenWrt instead, if OpenWrt is installed.

Before reading this chapter, you should:

- have installed U-Boot ([Chapter 1](01-installing-uboot.md));
- have the serial console connected and a terminal program on it.

## 2.2. How the Router Boots

```
power on → Qualcomm firmware → EVA (AVM) → U-Boot → NetBSD (volume "kernel")
                                                  ↘ or a file from the PC (TFTP)
```

EVA, AVM's boot loader, starts U-Boot, which [Chapter 1](01-installing-uboot.md)
installed. U-Boot looks in the flash's UBI partition for the volume named `kernel` and
boots it; if there is none, it waits for an image over TFTP. Installing NetBSD means
putting it into a volume named `kernel`; U-Boot's own settings are never changed.

UBI divides the partition into volumes, like a partition table on a disk
([Chapter 10](10-nand-and-ubi.md)). On a box that had AVM's system, the volumes are
AVM's and fill the whole partition: two copies of AVM's system take 90 MB of the
109 MB. They have to go to make room, and U-Boot cannot remove volumes (it can create
them); NetBSD can.

U-Boot can also load an image from a PC over TFTP and boot it from RAM. That is how
NetBSD is tried, and how it removes AVM's volumes, before anything of it is in the
flash.

## 2.3. The Release Images

A release ([releases page](../../../releases)) has these files:

| file | contents |
|---|---|
| `netbsd-flash.itb` | the router system: kernel and root file system in one 16 MB image. This is the one to install. |
| `netbsd-usb-FRITZ7530.itb` | a kernel whose root file system is on a USB stick, for development ([Chapter 6](06-building-and-testing.md)) |
| `SHA256` | checksums |

Check the download:

```
% sha256sum -c SHA256 --ignore-missing
netbsd-flash.itb: OK
```

## 2.4. Setting up the TFTP Server

The PC serves the image to U-Boot over TFTP. Connect the PC's wired port to a LAN port of
the 7530 and give it the address 192.168.77.2; the router will use 192.168.77.1. With
the image in the current directory (`enp0s31f6` is the PC's wired port):

```
% sudo ip addr add 192.168.77.2/24 dev enp0s31f6
% sudo dnsmasq --no-daemon --port=0 --enable-tftp --tftp-root="$PWD" --interface=enp0s31f6
```

Keep it running in its own terminal. A firewall on the PC must let TFTP in from
192.168.77.0/24 (with ufw: `sudo ufw allow in on enp0s31f6 from 192.168.77.0/24`).

> [!TIP]
> In a clone of this repository, `sudo tools/tftp-serve.sh` does the same for the
> directory `tftp/`. Coming straight from [Chapter 1](01-installing-uboot.md),
> `prepare/flash.sh` is already doing it for `prepare/tftp/`, if it is still running.

## 2.5. Trying NetBSD from RAM

Nothing is written to the flash in this section.

1. Power the router on. When the serial console shows `Hit any key to stop autoboot`,
   press a key. The U-Boot prompt appears:

   ```
   (FRITZ7530) #
   ```

2. Load the image and boot it:

   ```
   (FRITZ7530) # setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
   (FRITZ7530) # tftpboot 0x85000000 netbsd-flash.itb
   (FRITZ7530) # bootm 0x85000000
   ```

   If `tftpboot` reports every port "Down", the link was not up yet: run it again.

3. After about 30 seconds the console shows `login:`. Log in as `root`; there is no
   password on the console.

Look around: `ifconfig -a`, `dmesg`, `cat /etc/router.conf`. A power cycle returns the
router to the system in its flash.

> [!WARNING]
> Never type `saveenv` at the U-Boot prompt. The settings above are meant to last until
> the next reset only; this guide never stores anything in U-Boot.

## 2.6. Installing into the Flash

> [!WARNING]
> This section writes the flash. AVM's system is deleted; its settings volumes
> (`avm_config`, `avm_userdata`, 15 MB) are kept, unused. The backup of
> [Section 1.5.3](01-installing-uboot.md#153-backing-up-the-whole-flash) has
> everything.

If OpenWrt is installed (U-Boot was installed the other way,
[Section 1.7](01-installing-uboot.md#17-the-other-way-through-openwrt)), follow
[Section 2.7](#27-next-to-openwrt) instead.

1. Boot NetBSD from RAM ([Section 2.5](#25-trying-netbsd-from-ram)), log in on the
   console, and remove AVM's two system volumes:

   ```
   # sysctl -w hw.qcomqpic0.ubi_remove=avm_filesys_0
   # sysctl -w hw.qcomqpic0.ubi_remove=avm_filesys_1
   # dmesg | tail -2
   qcomqpic0: UBI volume 0 "avm_filesys_0": removed (0)
   qcomqpic0: UBI volume 1 "avm_filesys_1": removed (0)
   ```

   Each takes a few seconds: the driver clears the volume's entry in the volume table
   and erases its 364 eraseblocks
   ([Section 10.5](10-nand-and-ubi.md#105-removing-a-volume)).

2. Reboot, and stop U-Boot on the console (`Hit any key to stop autoboot`):

   ```
   # reboot
   ...
   (FRITZ7530) #
   ```

3. Create the two volumes NetBSD uses:

   ```
   (FRITZ7530) # ubi part ubi
   (FRITZ7530) # ubi create kernel 0x1000000
   (FRITZ7530) # ubi create config 0x3e000
   ```

   `kernel` (16 MB) will hold the image; `config` (248 KB) will hold NetBSD's saved
   settings ([Chapter 3](03-configuring-the-router.md)). `ubi part ubi` prints the
   available eraseblocks: 728 after step 1, the two volumes' 364 each.

4. Load the image, write it, read it back and compare:

   ```
   (FRITZ7530) # setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
   (FRITZ7530) # tftpboot 0x85000000 netbsd-flash.itb
   (FRITZ7530) # ubi write 0x85000000 kernel ${filesize}
   (FRITZ7530) # ubi read 0x89000000 kernel ${filesize}
   (FRITZ7530) # cmp.b 0x85000000 0x89000000 ${filesize}
   Total of 15734498 byte(s) were the same
   ```

   The number is the image's size. If the bytes differ, write it again.

   > [!CAUTION]
   > Read back to 0x89000000 as shown, not to a lower address: U-Boot itself sits just
   > below 0x88013000, and reading over it resets the board.

5. Reset. U-Boot now finds the `kernel` volume and boots it; NetBSD is up in about 45
   seconds.

   ```
   (FRITZ7530) # reset
   ```

Continue with [Chapter 3](03-configuring-the-router.md) to set the router up.

## 2.7. Next to OpenWrt

With OpenWrt installed ([Section 1.7](01-installing-uboot.md#17-the-other-way-through-openwrt)),
the volume named `kernel` is OpenWrt's. NetBSD goes into a volume of its own, and the
names are swapped afterwards; OpenWrt stays, as a second system to boot
([Section 4.4](04-updating-and-recovery.md#44-booting-openwrt)).

> [!WARNING]
> This writes the flash. OpenWrt keeps its kernel and root file system, but its
> saved settings are deleted: its writable volume `rootfs_data` (81 MB) is removed to
> make room. Back them up from OpenWrt first (`sysupgrade -b /tmp/backup.tar.gz`, then
> copy the file to the PC).

1. Make room by removing `rootfs_data`: from OpenWrt,
   `ubirmvol /dev/ubi0 --name=rootfs_data`, or from NetBSD in RAM,
   `sysctl -w hw.qcomqpic0.ubi_remove=rootfs_data` (U-Boot has no command to remove a
   volume). Then, at the U-Boot prompt, create the two volumes:

   ```
   (FRITZ7530) # ubi part ubi
   (FRITZ7530) # ubi create netbsd 0x1000000
   (FRITZ7530) # ubi create config 0x3e000
   ```

2. Write the image into `netbsd` and check it, as in step 4 of
   [Section 2.6](#26-installing-into-the-flash), with `netbsd` in place of `kernel`.

3. Boot OpenWrt (power cycle, about two minutes), log in (`ssh root@192.168.1.1`, or the
   serial console), swap the names and reboot:

   ```
   # ubirename /dev/ubi0 kernel openwrt netbsd kernel
   # reboot
   ```

The router now boots NetBSD by itself. OpenWrt stays installed as `openwrt`.

Continue with [Chapter 3](03-configuring-the-router.md) to set the router up.
