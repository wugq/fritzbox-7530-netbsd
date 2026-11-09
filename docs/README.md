# NetBSD on the FRITZ!Box 7530: the Handbook

Part I is for running NetBSD on the router. Part II explains how the port works, for
reading along with the code; each chapter builds on the ones before it.

Commands are shown with the prompt of the place they run in: `%` on the PC, `#` as root
on the router (NetBSD or OpenWrt), `(FRITZ7530) #` at the U-Boot prompt.

## Part I. Using

1. [Installing U-Boot](uboot.md)
   - how the box boots; the serial console; backing up the flash; U-Boot and OpenWrt; recovery
2. [Installing NetBSD](install.md)
   - the release images; the TFTP server; trying NetBSD from RAM; installing into the flash
3. [Configuring the Router](config.md)
   - ssh; a home router behind a modem; keeping settings; `router.conf`; LEDs, buttons, logs
4. [Updating and Recovery](maintenance.md)
   - updating; booting OpenWrt; a router that does not boot

## Part II. How It Works

5. [Building from Source and Testing](build-and-boot.md)
   - the repository over NetBSD's tree; the device tree; building; booting from TFTP and a
     USB stick; the test bench
6. [How the Drivers Work](drivers.md)
   - the NetBSD driver pattern; platform and SMP; serial; clocks and resets; USB; GPIO;
     Ethernet; the switch
7. [The Board Kernel Configuration](kernel-config.md)
   - `FRITZ7530`; hidden dependencies; profiling with `tprof`
8. [How the Flash Edition Works](flash-edition.md)
   - booting from the NAND; the image; the root file system; the boot script; settings
9. [NAND and UBI](nand.md)
   - the QPIC controller without DMA; page layout and ECC; UBI
