# Chapter 2. Installing NetBSD

## 2.1. Synopsis

This chapter installs the NetBSD router system into the flash of a FRITZ!Box 7530, next
to OpenWrt, and makes it the system the router boots.

After reading this chapter, you will know:

- how the 7530 boots and where NetBSD fits in;
- which release image to use;
- how to try NetBSD from RAM without changing the flash;
- how to install it into the flash and make it the default.

Before reading this chapter, you should:

- have installed U-Boot ([Chapter 1](01-installing-uboot.md));
- have the serial console connected and a terminal program on it.

## 2.2. How the Router Boots

```
power on → Qualcomm firmware → EVA (AVM) → U-Boot → NetBSD (volume "kernel")
                                                  ↘ or a file from the PC (TFTP)
```

EVA, AVM's boot loader, starts U-Boot, which [Chapter 1](01-installing-uboot.md) installed. U-Boot
loads the volume named `kernel` from the flash's UBI partition and boots it. With
OpenWrt installed, that volume is OpenWrt's. Installing NetBSD means writing it into a
volume of its own and then giving that volume the name `kernel`; U-Boot's own settings
are never changed.

U-Boot can also load an image from a PC over TFTP and boot it from RAM. That is how
NetBSD is tried before it is installed.

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
> directory `tftp/`.

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
> This section writes the flash. OpenWrt keeps its kernel and root file system, but its
> saved settings are deleted: its writable volume `rootfs_data` (81 MB) is removed to
> make room. Back them up from OpenWrt first (`sysupgrade -b /tmp/backup.tar.gz`, then
> copy the file to the PC).

1. With the TFTP server running, stop U-Boot as in [Section 2.5](#25-trying-netbsd-from-ram)
   and create the two volumes NetBSD uses:

   ```
   (FRITZ7530) # ubi part ubi
   (FRITZ7530) # ubi remove rootfs_data
   (FRITZ7530) # ubi create netbsd 0x1000000
   (FRITZ7530) # ubi create config 0x3e000
   ```

   `netbsd` (16 MB) will hold the image; `config` (248 KB) will hold NetBSD's saved
   settings ([Chapter 3](03-configuring-the-router.md)).

2. Load the image and write it:

   ```
   (FRITZ7530) # setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
   (FRITZ7530) # tftpboot 0x85000000 netbsd-flash.itb
   (FRITZ7530) # ubi write 0x85000000 netbsd ${filesize}
   ```

3. Read it back and compare:

   ```
   (FRITZ7530) # ubi read 0x89000000 netbsd ${filesize}
   (FRITZ7530) # cmp.b 0x85000000 0x89000000 ${filesize}
   Total of 15726606 byte(s) were the same
   ```

   The number is the image's size. If the bytes differ, write it again.

   > [!CAUTION]
   > Read back to 0x89000000 as shown, not to a lower address: U-Boot itself sits just
   > below 0x88013000, and reading over it resets the board.

4. Boot the installed copy:

   ```
   (FRITZ7530) # ubi read 0x85000000 netbsd ${filesize}
   (FRITZ7530) # bootm 0x85000000
   ```

## 2.7. Making NetBSD the Default

U-Boot boots the volume named `kernel`, which is still OpenWrt's. Renaming the two
volumes makes NetBSD the default. U-Boot cannot rename volumes; OpenWrt can.

1. Power cycle the router. OpenWrt boots (about two minutes).
2. Log in to OpenWrt: `ssh root@192.168.1.1` from a PC with an address in
   192.168.1.0/24, or on the serial console.
3. Swap the names and reboot:

   ```
   # ubirename /dev/ubi0 kernel openwrt netbsd kernel
   # reboot
   ```

The router now boots NetBSD by itself, in about 45 seconds. OpenWrt stays installed as
`openwrt`; [Chapter 4](04-updating-and-recovery.md) shows how to boot it.

Continue with [Chapter 3](03-configuring-the-router.md) to set the router up.
