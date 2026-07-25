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

## 6. CI / auto-release pipeline

Automates §5's build recipe against every new OpenWrt point release, but only
publishes a release after it's actually proven against the real router — not
just "it compiled." Three files: `.github/workflows/track-openwrt.yml`
(daily detector), `.github/workflows/release.yml` (build → static-verify →
hardware-verify → publish), `.github/scripts/*.sh` (the actual logic, kept
as plain scripts rather than inline YAML so they're testable by hand).

**What each stage actually proves, stated honestly:**

| Stage | Runs on | Proves | Does NOT prove |
|---|---|---|---|
| `build-image.sh` | GitHub-hosted | The exact §5 recipe (SDK rebuild of `patches/861` + ImageBuilder assembly) reproduces cleanly against a new OpenWrt version | Nothing about correctness — a broken patch can still "compile" |
| `static-verify.sh` | GitHub-hosted | The patched module (not the stock feed one) got embedded; package manifest didn't silently lose packages; the DTB parses and identifies as this board | Anything about runtime behavior |
| `hardware-verify.sh` | Self-hosted, on the operator's LAN | The candidate `brcmfmac.ko` actually loads and negotiates with the **real BCM43602 radios** on the operator's own router — all 3 phys, wireless interfaces up, no firmware/panic errors — then reverts | Long-term stability, throughput, or anything the ~15s test window doesn't exercise. This is a fresh compile-and-swap smoke test on every point release, not the days-of-real-use verification `v11`'s hand-flashed release represents. |

Publishing (`auto-<version>`, marked `--latest`) only happens if hardware-verify
passes. A static-verify failure or a hardware-verify failure both leave the
router untouched (hardware-verify's revert runs via a bash `trap` — happens
even if the test itself fails) and publish nothing.

**One-time setup required (cannot be automated from here):**

1. **Self-hosted runner**, registered on a machine with real LAN access to
   the router (the bench host is the natural choice — it already has proven
   SSH access throughout this project):
   ```bash
   # From the repo settings page (Settings -> Actions -> Runners -> New
   # self-hosted runner) get a fresh registration token, then on the bench host:
   mkdir actions-runner && cd actions-runner
   curl -o actions-runner.tar.gz -L https://github.com/actions/runner/releases/latest/download/actions-runner-linux-x64-<ver>.tar.gz
   tar xzf actions-runner.tar.gz
   ./config.sh --url https://github.com/88plug/netgear-r8000-plus --token <TOKEN> --labels r8000-lan
   ./svc.sh install && ./svc.sh start   # runs as a system service, survives reboot
   ```
   `release.yml`'s hardware-verify job targets `runs-on: [self-hosted, r8000-lan]`
   specifically (not just any self-hosted runner) — the label is the safety
   boundary that keeps this job from ever landing on a machine without real
   access to the router.
2. **SSH deploy key** — a dedicated key (not the interactive blank-password
   login), so the hardware-verify script never needs `sshpass` or an
   interactive prompt:
   ```bash
   ssh-keygen -t ed25519 -N "" -f ~/.ssh/r8000_ci_deploy
   ssh root@192.168.1.1 'mkdir -p /etc/dropbear; chmod 600 /etc/dropbear/authorized_keys' \
     < <(cat ~/.ssh/r8000_ci_deploy.pub) # or just echo the pubkey into that file over SSH
   # then add a Host block to ~/.ssh/config on the runner machine so plain
   # `ssh 192.168.1.1` picks it up automatically (see hardware-verify.sh)
   ```
   Already generated and installed as of this writing (`~/.ssh/r8000_ci_deploy`
   on the bench host, pubkey in the router's `/etc/dropbear/authorized_keys`).
   This is a **persistent** access credential, unlike everything else this
   project does to the router — worth knowing it's there. Rotate/remove it
   from `/etc/dropbear/authorized_keys` the same way it was added if the
   pipeline is ever decommissioned.
3. **`WIRELESS_CONFIG_B64` repo secret** — `v2-files/etc/config/wireless` is
   gitignored (real passphrases), so CI needs it supplied out-of-band:
   ```bash
   base64 -w0 v2-files/etc/config/wireless | gh secret set WIRELESS_CONFIG_B64 \
     --repo 88plug/netgear-r8000-plus
   ```
4. **Test the pipeline manually before trusting the scheduled trigger**:
   `gh workflow run release.yml --repo 88plug/netgear-r8000-plus -f openwrt_version=25.12.5`
   (the version already shipping — should hardware-verify clean, giving a
   known-good dry run before the tracker ever fires on a real new release).

**Branch-pattern reminder:** `track-openwrt.yml`'s `OPENWRT_BRANCH_PATTERN`
is deliberately pinned to `25.12.x` and does not auto-widen to a new
minor/major OpenWrt release — see the "Tracking scope" decision this project
made when this pipeline was designed. A new minor/major bump needs a human
to bump the pattern after actually reviewing what changed upstream (kernel
version, config schema, package availability can all shift in ways a point
release doesn't).
