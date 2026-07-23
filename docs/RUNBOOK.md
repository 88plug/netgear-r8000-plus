# R8000 OpenWRT — Operations Runbook

Everything needed to access, flash, recover, and rebuild the operator's R8000.
Bench: Manjaro/Arch host → USB-ethernet dongle → router LAN port.

## 1. Host / bench network (the load-bearing gotcha)

The dongle is `enp103s0f3u1` (Realtek USB NIC). **Do NOT put it on DHCP** — the
router's DHCP injects a default route + DNS, and since the router has no WAN this
hijacks the host's internet and breaks name resolution (`downloads.openwrt.org`
resolved to `192.168.1.1`). Correct config = **static, link-only, no gateway, no
DNS**:

```bash
CON=$(nmcli -g GENERAL.CONNECTION device show enp103s0f3u1)
sudo nmcli connection modify "$CON" \
  ipv4.method manual ipv4.addresses 192.168.1.2/24 \
  ipv4.gateway "" ipv4.dns "" ipv4.never-default yes ipv6.method disabled
sudo nmcli connection up "$CON"
```

This reaches `192.168.1.1` on-link and never touches the host's Starlink WiFi
(`wlp3s0`) default route. Verify: `ping 192.168.1.1` works AND `ping 1.1.1.1`
(via WiFi) still works.

## 2. Accessing the router

### Stock firmware → root shell (Nighthawk telnet backdoor)
Stock has no SSH/telnet by default. Enable telnet with the `telnetenable` magic
UDP packet (Netgear's own mechanism), then log in:

```bash
# tool: github.com/bkerler/netgear_telnet  (pure py3, Blowfish, AMBIT scheme)
python telnet-enable.py 192.168.1.1 <MAC-with-dashes> admin <admin-password>
# → port 23 opens; telnet in as admin / <admin-password> → root BusyBox shell
```
Default reset creds are `admin` / `password`. The router's LAN MAC is on its
label / `ip neigh show 192.168.1.1`. Note: stock `debug.htm`'s "Enable Telnet"
checkbox does NOT work on the Nighthawk — use the magic packet.

### OpenWRT → SSH
```bash
# fresh OpenWrt = root, blank password (dropbear)
sshpass -p '' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
  -o PubkeyAuthentication=no root@192.168.1.1
```
Set a root password after first boot (`passwd`) to lock it down.

## 3. Flashing

Single `.chk` = both factory-flash AND sysupgrade image (bcm53xx convention).

- **Stock GUI (from stock firmware):** browser → `http://192.168.1.1/UPG_upgrade.htm`
  (bypasses the genie setup-wizard, which loops when WAN is down) → Browse →
  select `.chk` → Upload → OK. ~2 min, do not cut power.
- **sysupgrade (from running OpenWrt, easiest):**
  ```bash
  scp openwrt-...-netgear_r8000-squashfs.chk root@192.168.1.1:/tmp/
  ssh root@192.168.1.1 'sysupgrade -v /tmp/openwrt-...-squashfs.chk'   # add -n to wipe config
  ```
- **nmrpflash (headless recovery / from a brick):**
  ```bash
  sudo nmrpflash -i enp103s0f3u1 -f <image.chk> -a 192.168.1.252 -A 192.168.1.253
  # then power-cycle the router; WAIT for "Reboot your device now" (flash-write
  # can take 15+ min after "Uploading ... OK" — do not power-cycle early)
  ```
  Set the dongle `managed no` first so NM can't re-IP it mid-flash:
  `sudo nmcli device set enp103s0f3u1 managed no` (restore with `... yes`).

## 4. Recovery / unbrick (the safety net — prepared BEFORE flashing)

On hand in `images/`:
- OpenWrt image(s) `.chk` (+ `sha256sums`, verified against OpenWrt's signed sums)
- **Stock revert firmware** `R8000-V1.0.4.88_10.1.88.chk` (from Netgear support)

Brick → method:
| Symptom | Method |
|---|---|
| OpenWrt still SSH/LuCI reachable | `sysupgrade -F` (force) the target image |
| Dead / boot-loop, LEDs cycling | **nmrpflash** (CFE NMRP listens every boot) |
| nmrpflash no response | reset-button-held CFE TFTP recovery (static `192.168.1.10/24`, `tftp -m binary 192.168.1.1 -c put <img>`) |
| TFTP dead too | serial console (3.3V TTL, 115200 8N1) → CFE prompt |

`nmrpflash` is installed (built from source; the AUR `-bin` AppImage is broken
here). `tftp-hpa` installed.

## 5. Building images

- **Source patch (upstreamable):** edit `openwrt/` tree, capture diff to
  `patches/`. Our fix: `patches/0001-nvram-bcm53xx-add-netgear-r8000-43602.patch`.
- **ImageBuilder (fast, no toolchain compile):**
  ```bash
  cd openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64
  make image PROFILE=netgear_r8000 \
    PACKAGES="<base + extras>" \
    FILES=/home/andrew/netgearr8000/image-files \
    EXTRA_IMAGE_NAME=r8000plus
  # output: bin/targets/bcm53xx/generic/openwrt-...-r8000plus-...-squashfs.chk
  ```
  `image-files/` overlays FILES into the rootfs (e.g. patched `/etc/init.d/nvram`,
  firmware blobs in `/lib/firmware/brcm/`, `/etc/config/*`). Verify a FILES
  override took: `unsquashfs -n <root.squashfs> <path>` and grep it.
