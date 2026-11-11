# Chapter 3. Configuring the Router

## 3.1. Synopsis

After reading this chapter, you will know:

- what the router does after a fresh installation;
- how to log in with ssh;
- how to turn it into a home router behind a modem;
- how settings are kept across reboots, and every option of `/etc/router.conf`;
- what the LEDs and buttons do, and where the logs are;
- how to run the router from the command line: looking at it, routes, VLANs on the
  switch ports, the firewall (port forwarding, a guest network), the DHCP server, and
  finding problems.

Before reading this chapter, you should have NetBSD running on the router, from the flash
or from RAM ([Chapter 2](02-installing-netbsd.md)).

## 3.2. The First Boot

A fresh installation comes up with these settings:

| | |
|---|---|
| LAN | 192.168.77.1/24 on switch ports 1, 2 and 4 |
| WAN | switch port 3, fixed address 192.168.78.1/24, no default route |
| firewall | on: from the WAN, only ICMP and replies to connections the router or the LAN started get in |
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
FIREWALL=yes
```

`FIREWALL=yes` is already the default; a `router.conf` saved by an older version lacks
the line, and a missing option means `no`.

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

If the provider offers IPv6, add:

```
WAN_IPV6=yes
```

The router then takes an address for the WAN from the provider's router advertisements
and DHCPv6, and asks for a prefix by DHCPv6 prefix delegation. The first /64 of that
prefix goes to the LAN: the clients configure their addresses from it and use the router
as their IPv6 default router. The firewall applies to IPv6 as to IPv4: of new connections
from the internet only ICMP comes in. The LAN hosts are reachable by their global
addresses only through connections they start.

To also have the router as the clients' DNS server over IPv6, give the LAN a unique local
address as well, e.g. `LAN_ADDR6="fd00:77::1/64"`. Without it, the clients still get
the IPv4 address of the resolver by DHCP.

> [!NOTE]
> `WAN_IPV6` works with `WAN_MODE=static` and `dhcp`, not yet with `pppoe`. It has been
> tested against a simulated provider in the lab (FreeBSD with `rtadvd` and ISC
> `dhcpd -6`, see [Section 6.8](06-building-and-testing.md#68-the-test-bench-and-automated-tests)),
> not yet against a real one.

## 3.5. Keeping Settings

The running system is in RAM: every change is lost at reboot unless saved. `saveconfig`
saves the files listed in `/etc/saveconfig.list` (`router.conf`, `npf.conf`,
`dhcpd.conf`, `rc.local`, the password files, the ssh keys, ...) into the `config` volume of the
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
| `WAN_IPV6` | `yes`, `no` | IPv6 from the provider: an address for the WAN, and a prefix for the LAN (DHCPv6 prefix delegation); not with `pppoe` yet |
| `LAN_ADDR6` | address/prefix | an IPv6 address for the LAN, e.g. `fd00:77::1/64`; clients get addresses from router advertisements; with `DNS=yes`, the router announces itself as DNS server at this address |
| `FIREWALL` | `yes`, `no` | of new connections from the WAN, let only ICMP in (`/etc/npf.conf`) |
| `NAT` | `yes`, `no` | masquerade the LAN behind the WAN address (`/etc/npf.conf`) |
| `DHCPD` | `yes`, `no` | DHCP server on the LAN: addresses .100 to .199 of `LAN_ADDR`'s network; fixed addresses and other options in `/etc/dhcpd.conf` |
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

## 3.9. Working from the Command Line

The router has no web interface and no command language of its own: it is NetBSD, and it
is run with NetBSD's commands, `ifconfig`, `route`, `npfctl`, `sysctl` and the rest,
each with its manual page. What a managed switch calls the running configuration and the
startup configuration are here:

| | a managed switch | this router |
|---|---|---|
| change it now | commands in `configure` mode | `ifconfig`, `route`, `npfctl`, `sysctl` at the shell |
| gone at reboot | unless saved | always: the system runs from RAM |
| what the next boot does | the startup configuration | `/etc/router.conf`, the files of each program, `/etc/rc.local` |
| save | `copy running-config startup-config` | edit those files, then `saveconfig` |

There is no command that turns the running state back into a file. Much of it is not a
setting anyway: the provider's address and routes come from DHCP, the IPv6 link-local
addresses from the kernel, and writing them into a file would pin them. So the way to
work is: try a change at the shell; when it does what you want, write the same into the
file it belongs to (the tables below say which), then `saveconfig`.

`/etc/rc.local` is for what has no file of its own: `/etc/rc` runs it at the end of the
boot, after the network, the firewall and the services. Write the commands as you typed
them:

```
# vi /etc/rc.local
route add -net 10.20.0.0/16 192.168.77.254
# saveconfig
```

A command in it that fails prints an error on the console, and the next ones still run.

## 3.10. Looking at the Router

| what | command |
|---|---|
| interfaces, addresses, link | `ifconfig -a`, or `ifconfig vlan2` for one |
| switch ports: link, speed | `sysctl hw.qcomess0.ports` |
| switch VLAN table | `sysctl hw.qcomess0.vlans` |
| per-port counters (frames, errors, pause) | `vmstat -e \| grep qcomess0` |
| routes | `netstat -rn`, `netstat -rn -f inet6` |
| traffic per interface | `netstat -i`; every second: `netstat -w 1 -I vlan2` |
| neighbours (MAC addresses) | `arp -an`, `ndp -an` |
| firewall rules, and what they did | `npfctl show`, `npfctl stats` |
| connections the firewall tracks | `npfctl list` |
| DHCP leases given out | `cat /var/db/dhcpd.leases` |
| PPPoE session | `pppoectl -d pppoe0`, `ifconfig pppoe0` |
| CPU, memory, processes | `top`, `vmstat 1` |
| messages | `dmesg`, `tail /var/log/messages` |

The interfaces: `qcomedma0` is the LAN (switch VLAN 1), `vlan2` the WAN, `pppoe0` the
PPPoE session over it, `vlanN` any other VLAN.

## 3.11. Addresses and Routes

| task | now | at every boot |
|---|---|---|
| LAN address | `ifconfig qcomedma0 inet 192.168.77.1/24` | `LAN_ADDR` in `router.conf` |
| a second address on the LAN | `ifconfig qcomedma0 inet 192.168.79.1/24 alias` | the same line in `rc.local` |
| WAN address and default route | `ifconfig vlan2 inet 192.0.2.2/24`, `route add default 192.0.2.1` | `WAN_MODE=static`, `WAN_ADDR`, `WAN_GW` |
| static route | `route add -net 10.20.0.0/16 192.168.77.254` | the same line in `rc.local` |
| remove a route | `route delete -net 10.20.0.0/16` | remove the line |

## 3.12. The Switch: Ports and VLANs

The four jacks and the CPU are ports of one switch inside the chip. Seen from the front,
the jack farthest from the DSL jack is port 4, then 3, 2 and 1 (port 5 has no jack).
Which port belongs to which network is the switch's VLAN table:

```
# sysctl hw.qcomess0.vlans
hw.qcomess0.vlans = 1:1,2,4,5 2:3
```

That is: VLAN 1 has ports 1, 2, 4 and 5, VLAN 2 has port 3, all untagged. It is what
`WAN_PORT=3` makes. The CPU is in every VLAN, so the router is on every network: VLAN 1
is the interface `qcomedma0` (the LAN), each other VLAN *N* an interface `vlanN` on top
of it, which has to be created. A `t` after a port makes it a tagged member: the frames
on that cable carry the VLAN number, for a device that understands VLANs.

To give port 2 a network of its own, VLAN 10:

```
# sysctl -w hw.qcomess0.vlans="1:1,4,5 2:3 10:2"
# ifconfig vlan10 create
# ifconfig vlan10 vlan 10 vlanif qcomedma0
# ifconfig vlan10 inet 192.168.10.1/24 up
```

A provider that wants the WAN traffic tagged, with VLAN 7 say, gets port 3 as a tagged
member of VLAN 7, and the WAN addresses on `vlan7`:

```
# sysctl -w hw.qcomess0.vlans="1:1,2,4,5 7:3t"
```

(`WAN_MODE` and the firewall rules expect the WAN on `vlan2`; for a tagged WAN, set up
`vlan7` and its addresses in `rc.local`, and name `vlan7` in `/etc/npf.conf`.)

Rules of the table: a port may be untagged in one VLAN only (the switch has to know where
to put the untagged frames it receives); a port in no VLAN takes no part. The driver
refuses a table that breaks a rule. Moving the port your PC is on cuts your ssh session:
change the table from the serial console, or from a PC on another port.

At every boot: the same commands in `rc.local`. `/etc/rc` sets the WAN port first, so
the table in `rc.local` replaces the one `WAN_PORT` made. To undo by hand:
`sysctl -w hw.qcomess0.wan_port=3` brings back the table of `WAN_PORT=3`, and
`ifconfig vlan10 destroy` removes the interface.

## 3.13. The Firewall

The rules are in `/etc/npf.conf` (`npf.conf(5)`). `/etc/rc` does not load that file as
it is: it adds `$lan_net` (from `LAN_ADDR`), leaves out the lines that end in `# NAT` or
`# FIREWALL` when `NAT` or `FIREWALL` is `no`, and writes the result to
`/var/run/npf.conf`. To try a change, edit that copy and load it:

```
# vi /var/run/npf.conf
# npfctl reload /var/run/npf.conf
# npfctl show
```

When it works, make the same change in `/etc/npf.conf`, and `saveconfig`. If a reload
went wrong, reload the previous file, or reboot.

The rules are grouped by interface: `group "wan" on vlan2` for what comes from the
provider, `group default` for everything else. In a group the first matching rule marked
`final` decides.

**Port forwarding**, the WAN address's port 2222 to port 22 of the host 192.168.77.10: at
the top of the file,

```
map vlan2 dynamic proto tcp 192.168.77.10 port 22 <- ifaddrs(vlan2) port 2222
```

and in `group "wan"`, before `block in final all`:

```
	pass in final proto tcp to ifaddrs(vlan2) port 2222
```

The rule names the address and port the packet arrived *with*, not the host it goes on
to: NPF filters incoming packets before translating them. A rule for 192.168.77.10 port
22 lets nothing through.

**A guest network** on VLAN 10 (set up as in Section 3.12) that may use the internet but
not reach the LAN: a group for its interface, before `group default`,

```
group "guest" on vlan10 {
	block in final to $lan_net
	pass final all
}
```

and, with `NAT=yes`, a translation for its addresses next to the LAN's:

```
map vlan2 dynamic 192.168.10.0/24 -> ifaddrs(vlan2)		# NAT
```

The guests can still reach the router's own services at 192.168.10.1; to keep them off
ssh, add `block in final proto tcp to 192.168.10.1 port 22` before the `pass`. At boot
`npfctl` warns `unknown interface 'vlan10'`, because the firewall is loaded before
`rc.local` creates the interface; the group applies as soon as the interface exists.

## 3.14. The DHCP Server

With `DHCPD=yes` the LAN gets addresses .100 to .199. More goes into `/etc/dhcpd.conf`
(`dhcpd.conf(5)`), for instance a fixed address for one host:

```
host printer {
	hardware ethernet 00:11:22:33:44:55;
	fixed-address 192.168.77.20;
}
```

Restart the server to read it: `kill $(cat /var/run/dhcpd.pid)`, then
`dhcpd -q -cf /var/run/dhcpd.conf qcomedma0`. The leases are in `/var/db/dhcpd.leases`.

The guest network of Section 3.13 needs a server of its own. In `rc.local`, after its
interface:

```
cat > /var/run/dhcpd-guest.conf <<'END'
authoritative;
subnet 192.168.10.0 netmask 255.255.255.0 {
	range 192.168.10.100 192.168.10.199;
	option routers 192.168.10.1;
	option domain-name-servers 9.9.9.9;
}
END
touch /var/db/dhcpd-guest.leases
dhcpd -q -cf /var/run/dhcpd-guest.conf -lf /var/db/dhcpd-guest.leases \
    -pf /var/run/dhcpd-guest.pid vlan10
```

The router's DNS resolver answers the LAN only, so the guests are given a public one.

## 3.15. Finding Problems

| question | command |
|---|---|
| does the other side answer? | `ping 192.168.77.2`, `ping6 fe80::1%qcomedma0` |
| which way does a packet go? | `route get 192.0.2.7` |
| what is on the wire? | `tcpdump -ni vlan2`, `tcpdump -ni qcomedma0 icmp`, `tcpdump -ni vlan2 port 67` |
| does a port see frames? | `vmstat -e \| grep "qcomess0 port3"`, twice, and compare |
| did the firewall drop it? | `npfctl stats` (the block counters) |
| is a service listening? | `netstat -an \| grep LISTEN` |

A port whose counters do not move, though its link is up, is a problem below NetBSD: the
cable, the other side, or the switch.
