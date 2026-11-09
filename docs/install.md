# Installing and using NetBSD on a FRITZ!Box 7530

This guide is for someone who has a FRITZ!Box 7530 and wants to run the released NetBSD
images on it. It assumes no knowledge of how the port was made; the other documents in
`docs/` explain that.

## What you get

A NetBSD router: the four LAN ports and one WAN port of the switch, NAT, a DHCP server,
a DNS resolver, IPv6 on the LAN, PPPoE, ssh, the LEDs and buttons. It boots from the
router's own flash and needs no PC once installed.

What it does not do: WiFi, DSL (the DSL jack), DECT telephony. The WAN is one of the
Ethernet ports, so on a DSL line the 7530 needs a separate modem in front of it.

**This is a learning project, not a finished product.** Expect rough edges, keep the
serial console at hand, and keep a way back (below).

## How the pieces fit

The 7530 starts AVM's boot loader (EVA), which can only start AVM's own system. To run
anything else, a second boot loader, **U-Boot**, is installed after EVA. U-Boot loads the
kernel from a volume named `kernel` in the flash, or from a PC over the network (TFTP).

```
power on → Qualcomm firmware → EVA (AVM, never changed) → U-Boot → NetBSD
                                                              └→ or OpenWrt
```

There is no ready-made way to install U-Boot by itself. The one that exists comes with
**OpenWrt**: OpenWrt's installation for this model writes U-Boot and OpenWrt to the flash.
So the order is: install OpenWrt (which brings U-Boot), then add NetBSD next to it.
OpenWrt stays installed as a fallback you can boot at any time.

A release has two images:

| file | what it is | how it runs |
|---|---|---|
| `netbsd-flash.itb` | the router: kernel and the whole system in one 16 MB file | from the flash (or from RAM for a try) |
| `netbsd-usb-FRITZ7530.itb` | a kernel only; the system is on a USB stick | from RAM over TFTP, for development |

Most people want `netbsd-flash.itb`. `SHA256` lists the checksums.

## What you need

- A **3.3 V USB serial adapter** and three jumper wires. U-Boot is driven from its serial
  console, so this is required, not optional. Where to connect it:
  [uboot.md, "Serial console"](uboot.md#serial-console).
- A **Linux PC with a wired Ethernet port**, a terminal program for the serial console
  (`picocom`, `screen`, ...), `dnsmasq` (as TFTP server) and `python3`.
- An Ethernet cable from the PC to a **LAN port** of the 7530.

Throughout this guide the router is 192.168.77.1 and the PC 192.168.77.2 on the LAN.

## 1. Install U-Boot (and OpenWrt)

Follow [uboot.md](uboot.md). It backs up the whole flash first, then installs U-Boot and
OpenWrt with OpenWrt's official method. Afterwards, with the serial console connected,
pressing a key during `Hit any key to stop autoboot` gives the U-Boot prompt:

```
(FRITZ7530) #
```

Every U-Boot command below is typed there.

**Never run `saveenv` in U-Boot.** Nothing in this guide needs it; U-Boot's settings
stay as OpenWrt installed them.

## 2. Try NetBSD from RAM (nothing is written to the flash)

On the PC, put `netbsd-flash.itb` into a directory and serve it over TFTP on the wired
port (`enp0s31f6` here; firewall rules may need to allow TFTP):

```
sudo ip addr add 192.168.77.2/24 dev enp0s31f6
sudo dnsmasq --no-daemon --port=0 --enable-tftp --tftp-root="$PWD" --interface=enp0s31f6
```

(`tools/tftp-serve.sh` in this repository does the same with `tftp/`.)

At the U-Boot prompt:

```
setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
tftpboot 0x85000000 netbsd-flash.itb
bootm 0x85000000
```

After about 30 s NetBSD shows `login:` on the serial console. Log in as `root` (no
password on the console) and look around: `ifconfig -a`, `dmesg`, `cat /etc/router.conf`.
A power cycle brings the router back to whatever is in the flash.

If `tftpboot` reports every port "Down", the link was not up yet: run it again.

## 3. Install NetBSD into the flash

**This writes the flash.** OpenWrt keeps its kernel and system, but loses its saved
settings: its writable overlay (`rootfs_data`, 81 MB) is removed to make room. Back them
up first from OpenWrt with `sysupgrade -b /tmp/backup.tar.gz` and copy the file off.

At the U-Boot prompt, with the TFTP server running:

```
ubi part ubi
ubi remove rootfs_data
ubi create netbsd 0x1000000
ubi create config 0x3e000
setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
tftpboot 0x85000000 netbsd-flash.itb
ubi write 0x85000000 netbsd ${filesize}
ubi read 0x89000000 netbsd ${filesize}
cmp.b 0x85000000 0x89000000 ${filesize}
```

- `netbsd` (16 MB) holds the image, `config` (248 KB) will hold NetBSD's saved settings.
- `cmp.b` must report the whole size as equal. If not, write it again.
- Read back to 0x89000000, not lower: U-Boot itself lives just below 0x88013000.

Try the installed copy:

```
ubi read 0x85000000 netbsd ${filesize}
bootm 0x85000000
```

### Make NetBSD the default

U-Boot boots the volume named `kernel`, which is OpenWrt's. Swapping the names makes
NetBSD the default. U-Boot cannot rename volumes, so this is done from OpenWrt: power
cycle (OpenWrt boots), log in to it (ssh root@192.168.1.1, or the serial console), and:

```
ubirename /dev/ubi0 kernel openwrt netbsd kernel
reboot
```

From now on the router boots NetBSD by itself, in about 45 s.

## 4. First steps on NetBSD

The router comes up with these settings (`/etc/router.conf`):

| | |
|---|---|
| LAN | 192.168.77.1/24 on switch ports 1, 2 and 4 |
| WAN | switch port 3, fixed address 192.168.78.1/24, no default route |
| NAT, DHCP server | off |
| DNS resolver | on, for the LAN |

Switch ports seen from the front: the one farthest from the DSL jack is 4, the next one 3,
then 2 and 1. The PC needs a fixed address (192.168.77.2/24) until the DHCP server is on.

**Allow ssh.** The image contains no keys or passwords (it is public). On the serial
console:

```
mkdir -p /root/.ssh
echo 'ssh-ed25519 AAAA... you@pc' > /root/.ssh/authorized_keys
passwd                      # optional: a root password for the console
saveconfig
```

The ssh host keys are made at the first boot; `saveconfig` keeps them, so they stay the
same afterwards.

**Make it a home router** behind a modem or another router that gives out addresses:

```
vi /etc/router.conf         # WAN_MODE=dhcp  NAT=yes  DHCPD=yes
saveconfig
reboot
```

For a DSL or fibre modem with PPPoE: `WAN_MODE=pppoe` with `PPPOE_USER` and `PPPOE_PASS`
(NAT over PPPoE is not tested against a real provider yet).

## 5. Everyday use

### Settings and `saveconfig`

The running system is in RAM: changes are lost at reboot unless saved. `saveconfig`
saves the files listed in `/etc/saveconfig.list` (router.conf, npf.conf, dhcpd.conf,
passwords, ssh keys, ...) into the `config` volume; at boot they replace the image's
copies. Only files that differ from the image are kept, so an updated image brings its
own new versions of the files you never changed.

```
saveconfig -n       # what would be saved
saveconfig          # save
```

The options in `router.conf`:

| option | |
|---|---|
| `HOSTNAME` | the router's name; `<name>.lan` resolves to it |
| `LAN_ADDR` | the LAN address and prefix |
| `WAN_PORT` | the switch port used as WAN (0: none, every port is LAN) |
| `WAN_MODE` | `static` (`WAN_ADDR`, `WAN_GW`), `dhcp`, or `pppoe` (`PPPOE_USER`, `PPPOE_PASS`, `PPPOE_AUTH`) |
| `LAN_ADDR6` | an IPv6 address for the LAN, e.g. `fd00:77::1/64`; clients get addresses by router advertisements |
| `NAT` | `yes`: masquerade the LAN behind the WAN address |
| `DHCPD` | `yes`: DHCP server for the LAN (`/etc/dhcpd.conf`) |
| `DNS` | `yes`: DNS resolver for the LAN (unbound) |
| `NTP` | `yes`: set the clock from the internet |

A missing option means "no" or empty.

### LEDs and buttons

- **info LED**: red while booting, green when up. Stays red: the boot hung.
- **wlan**, short press: all LEDs off (night mode); again: on.
- **wps**, held 5 s: reboot.
- **dect**, held 5 s: `saveconfig`.

The wps and dect actions are confirmed by three blinks of the info LED (green: done,
red: failed); every press is logged.

### Logs

`/var/log/messages` (and `authlog` for logins). They are in RAM and start empty at
every boot.

## 6. Updating, going back, recovering

**Update NetBSD**: at the U-Boot prompt, with the new image on the TFTP server:

```
ubi part ubi
setenv ipaddr 192.168.77.1; setenv serverip 192.168.77.2
tftpboot 0x85000000 netbsd-flash.itb
ubi write 0x85000000 kernel ${filesize}
ubi read 0x89000000 kernel ${filesize}
cmp.b 0x85000000 0x89000000 ${filesize}
```

The saved settings stay in `config`. Test a new image from RAM first (step 2).

At boot the console lists the saved files that replace one of the image's
(`flash edition: saved, not the image's: ...`). If one of them is a file you never edited,
it is an old copy from an earlier image and hides the new version; drop it with
`cp /var/run/image/etc/npf.conf /etc/npf.conf` (for example) and `saveconfig`.

**Boot OpenWrt once**: at the U-Boot prompt,
`ubi part ubi; ubi read 0x85000000 openwrt; bootm 0x85000000`.

**Make OpenWrt the default again**: from OpenWrt,
`ubirename /dev/ubi0 kernel netbsd openwrt kernel`.

**Nothing boots**: EVA, AVM's loader, is never touched, and it can start U-Boot and
OpenWrt from RAM again: run step 1's `flash.sh`, then repair from OpenWrt
([uboot.md, "Recovery"](uboot.md#recovery)).

**The router seems dead after a power cycle** (power LED on, no serial output): unplug
the USB serial adapter too, wait ten seconds, then plug the router in first. The
adapter can keep the board half powered through its data line.
