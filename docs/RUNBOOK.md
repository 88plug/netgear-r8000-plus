# R8000 OpenWRT — Operations Runbook

Everything needed to access, flash, recover, and rebuild the operator's R8000.
Bench: Manjaro/Arch host → USB-ethernet dongle → router LAN port.

**Conventions:** record exact commands, button/LED timings, and IPs used at
the bench — this repo is the reproducible runbook, not a summary. Keep a copy
of every image actually flashed (with its sha256) in `images/` (or alongside
it) so a re-flash is deterministic. Verify package/tool versions and the
current OpenWrt release live rather than trusting stale assumptions.

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

**MANDATORY before every sysupgrade: back up config off-device first.**
Upstream [openwrt/openwrt#21655](https://github.com/openwrt/openwrt/issues/21655)
(confirmed live on this exact device, twice — v5 and v6 flashes both hit it)
means a config-preserving sysupgrade can silently reset
`/etc/config/{wireless,firewall,sqm,system,usteer}` (and possibly others) to
defaults even without `-n`. There is no warning when this happens — the
upgrade reports success either way. This is not optional:

```bash
ssh root@192.168.1.1 'sysupgrade -b /tmp/backup.tar.gz && echo OK'
ssh root@192.168.1.1 'cat /tmp/backup.tar.gz' > backups/pre-flash-config-$(date +%Y%m%d).tar.gz
tar tzf backups/pre-flash-config-*.tar.gz | grep wireless   # sanity check it's non-empty/real
```

This minimal build has no `sftp-server`, so plain `scp`/`sftp` fail — use
`ssh ... "cat file" > local` / `ssh ... "cat > file" < local` for all file
transfer to/from the router (both directions, both image upload and backup
download).

- **Stock GUI (from stock firmware):** browser → `http://192.168.1.1/UPG_upgrade.htm`
  (bypasses the genie setup-wizard, which loops when WAN is down) → Browse →
  select `.chk` → Upload → OK. ~2 min, do not cut power.
- **sysupgrade (from running OpenWrt, easiest):**
  ```bash
  ssh root@192.168.1.1 'cat > /tmp/openwrt-....chk' < openwrt-...-netgear_r8000-squashfs.chk
  ssh root@192.168.1.1 'sha256sum /tmp/openwrt-....chk'   # compare to local sha256sum before flashing
  ssh root@192.168.1.1 'sysupgrade /tmp/openwrt-....chk'   # add -n to wipe config
  ```
  **After it reboots**, check config actually survived before assuming
  success:
  ```bash
  ssh root@192.168.1.1 'wc -c /etc/config/wireless /etc/config/firewall /etc/config/system'
  ```
  If any come back small/default-sized, restore from the pre-flight backup:
  ```bash
  ssh root@192.168.1.1 'cat > /tmp/restore.tar.gz' < backups/pre-flash-config-YYYYMMDD.tar.gz
  ssh root@192.168.1.1 'cd / && tar xzf /tmp/restore.tar.gz && uci commit && /etc/init.d/network restart'
  ```
  A driver/module reload alone was **not** sufficient to fully reset radio
  state after a firmware change on this hardware — if network/wireless
  services look wrong after a config restore, reboot the device fully rather
  than trusting a service restart.
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
  `patches/`. Our fixes: `patches/0001-nvram-bcm53xx-add-netgear-r8000-43602.patch`
  (5GHz nvram init) and `patches/861-brcmfmac-r8000-legacy-mbss-fallback.patch`
  (multi-BSS driver fix).
- **First build only:** `v2-files/etc/config/wireless` is gitignored (real
  passphrases, never committed) and won't exist on a fresh clone —
  `cp v2-files/etc/config/wireless.example v2-files/etc/config/wireless` and
  replace the `CHANGE-ME` placeholders with your own values before running
  `make image` below, or it'll fail (or worse, build with no passphrase set).
- **ImageBuilder (fast, no toolchain compile) — the actual recipe used for
  v7 through the current shipping image**, reconstructed and verified
  2026-07-24 against the live router's own `apk list --installed` (ground
  truth, not the stale doc this replaces):
  ```bash
  cd openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64
  make image PROFILE=netgear_r8000 \
    PACKAGES="-wpad-basic-mbedtls wpad-mbedtls hostapd-utils wpa-cli wireless-regdb \
      luci usteer sqm-scripts ethtool \
      kmod-brcmfmac brcmfmac-firmware-43602a1-pcie kmod-usb-ohci kmod-usb2 \
      kmod-phy-bcm-ns-usb2 kmod-usb-ledtrig-usbport kmod-usb3 kmod-phy-bcm-ns-usb3" \
    FILES=/home/andrew/netgearr8000/v2-files \
    EXTRA_IMAGE_NAME=r8000plus-vN
  # output: bin/targets/bcm53xx/generic/openwrt-...-r8000plus-vN-...-squashfs.chk
  ```
  **`FILES=v2-files`, not `image-files/`.** `image-files/` is the pre-v7 overlay
  and is now stale/historical only — it lacks guest-network isolation,
  `perf-tune`, the `radio-watchdog` rc.d enable symlink, the LED netdev-binding
  uci-defaults script, and `board.d/01_leds`. Building from `image-files/` today
  would silently regress all of those (see `docs/WINS.md`'s v5→v6 and v7
  entries — this exact class of stale-overlay-path drift has already caused two
  real regressions in this project). `v2-files/` has been the actual `FILES=`
  source since commit `1e2baf2` ("Ship v7: consolidated FILES overlay actually
  built and flashed").
  - **`-wpad-basic-mbedtls wpad-mbedtls`, not plain `wpad-basic-mbedtls`.**
    `wpad-basic-mbedtls` lacks `CONFIG_WNM`, so hostapd rejects
    `bss_transition` (802.11v) and every AP interface fails to come up — the
    exact bug that took down all 4 SSIDs on v7's first build. The device
    profile pulls in `wpad-basic-mbedtls` by default, hence the explicit `-`
    exclusion.
  - **The patched `kmod-brcmfmac` must come from this project's own local
    package repo, not the upstream feed.** The ImageBuilder tree's own
    `packages/` directory (auto-used as a local repo by `make image`, no
    `repositories.conf` edit needed) must contain
    `kmod-brcmfmac-<ver>.apk` built from `patches/861` — already present
    there as of this writing. If it's ever missing, rebuild it via the SDK
    (`openwrt-sdk-25.12.5-...`) with `patches/861` applied and copy the
    resulting `.apk` into ImageBuilder's `packages/` before running
    `make image`. This is the exact mistake that silently shipped v5 with
    the stock (unpatched) driver — see `docs/WINS.md`'s v5→v6 entry.
  - `v2-files/` overlays FILES into the rootfs (patched `/etc/init.d/nvram`,
    guest-network config, `perf-tune`, `radio-watchdog`, LED binding, etc).
    Verify a FILES override took: `unsquashfs -n <root.squashfs> <path>` and
    grep it, or after flashing, diff `ssh root@192.168.1.1 'apk list
    --installed'` against the previous known-good version's list.
