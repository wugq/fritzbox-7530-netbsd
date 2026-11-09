# Chapter 3. Configuring the Router

## 3.1. Synopsis

After reading this chapter, you will know:

- what the router does after a fresh installation;
- how to log in with ssh;
- how to turn it into a home router behind a modem;
- how settings are kept across reboots, and every option of `/etc/router.conf`;
- what the LEDs and buttons do, and where the logs are.

Before reading this chapter, you should have NetBSD running on the router, from the flash
or from RAM ([Chapter 2](02-installing-netbsd.md)).

## 3.2. The First Boot

A fresh installation comes up with these settings:

| | |
|---|---|
| LAN | 192.168.77.1/24 on switch ports 1, 2 and 4 |
| WAN | switch port 3, fixed address 192.168.78.1/24, no default route |
| NAT, DHCP server | off |
| DNS resolver | on, for the LAN |
| root | no password on the serial console; ssh only with a key, and none is set |

Seen from the front, the switch port farthest from the DSL jack is port 4, the next one
port 3, then 2 and 1. Until the DHCP server is on, give the PC a fixed address such as
192.168.77.2/24.

The ssh host keys are made at the first boot.

## 3.3. Allowing ssh Logins

The image is public, so it contains no keys or passwords. On the serial console, put
your public key in place and save it:

```
# mkdir -p /root/.ssh
# echo 'ssh-ed25519 AAAA... you@pc' > /root/.ssh/authorized_keys
# saveconfig
```

`saveconfig` keeps the key and the host keys made at the first boot, so the router's
identity stays the same after a reboot. Then, from the PC:

```
% ssh root@192.168.77.1
```

To set a root password for the console, run `passwd`, then `saveconfig`.

## 3.4. A Home Router behind a Modem

For a modem or an upstream router that hands out addresses by DHCP, connected to switch
port 3, edit `/etc/router.conf` and set:

```
WAN_MODE=dhcp
NAT=yes
DHCPD=yes
```

then save and reboot:

```
# saveconfig
# reboot
```

The LAN clients now get addresses from 192.168.77.100 to 192.168.77.199, use the router
as DNS server, and reach the internet through it.

For a DSL or fibre modem that expects PPPoE, set `WAN_MODE=pppoe`, `PPPOE_USER` and
`PPPOE_PASS` instead of `WAN_MODE=dhcp`.

> [!NOTE]
> PPPoE has been tested against a PPPoE server in the lab (session, authentication,
> address), not yet against a real provider.

## 3.5. Keeping Settings

The running system is in RAM: every change is lost at reboot unless saved. `saveconfig`
saves the files listed in `/etc/saveconfig.list` (`router.conf`, `npf.conf`,
`dhcpd.conf`, the password files, the ssh keys, ...) into the `config` volume of the
flash. At boot they replace the image's copies.

```
# saveconfig -n          # show what would be saved
# saveconfig             # save
```

Only files that differ from the image's copy are saved. When an update brings a new
version of a file you never edited, the new version is used.

> [!TIP]
> Without a `config` volume, `saveconfig -u` saves to the directory `/fritz/` on a USB
> stick, which is read back at boot.

## 3.6. `/etc/router.conf`

| option | values | |
|---|---|---|
| `HOSTNAME` | a name | the router's name; `<name>.lan` resolves to it |
| `LAN_ADDR` | address/prefix | the LAN address |
| `WAN_PORT` | 0-4 | the switch port used as WAN; 0: none, every port is LAN |
| `WAN_MODE` | `static`, `dhcp`, `pppoe` | how the WAN gets its address |
| `WAN_ADDR`, `WAN_GW` | address/prefix, address | `static` only: the address and the default route |
| `PPPOE_USER`, `PPPOE_PASS`, `PPPOE_AUTH` | text, text, `chap` or `pap` | `pppoe` only |
| `LAN_ADDR6` | address/prefix | an IPv6 address for the LAN, e.g. `fd00:77::1/64`; clients get addresses from router advertisements |
| `NAT` | `yes`, `no` | masquerade the LAN behind the WAN address (`/etc/npf.conf`) |
| `DHCPD` | `yes`, `no` | DHCP server on the LAN (`/etc/dhcpd.conf`) |
| `DNS` | `yes`, `no` | DNS resolver for the LAN (unbound) |
| `NTP` | `yes`, `no` | set the clock from the internet |

A missing option means `no` or empty. The other files are NetBSD's own formats:
`npf.conf(5)`, `dhcpd.conf(5)`.

## 3.7. LEDs and Buttons

The info LED is red while the router boots and turns green when it is up. If it stays
red, the boot hung; the serial console shows where.

| button | press | action |
|---|---|---|
| wlan | short | night mode: all LEDs off; press again to turn them on |
| wps | held 5 seconds | reboot |
| dect | held 5 seconds | `saveconfig` |

The wps and dect actions are confirmed by three blinks of the info LED: green when done,
red when they failed.

## 3.8. Logs

System messages go to `/var/log/messages`, logins to `/var/log/authlog`, errors also to
the serial console. The logs are in RAM and start empty at every boot.
