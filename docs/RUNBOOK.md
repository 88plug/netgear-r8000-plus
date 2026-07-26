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
- **Also first build only:** `v2-files/lib/firmware/brcm/brcmfmac43602-pcie.bin`
  (the 2021 firmware upgrade, `docs/FINDINGS.md` §20/`docs/WINS.md` v18) is a
  binary and gitignored by the repo's blanket `*.bin` rule — won't exist on a
  fresh clone either. Its source (`hwoffload-research/blob-analysis/rootfs/`,
  the extracted stock-firmware rootfs) is *also* gitignored (vendor binary
  rule) — both are "kept on disk, not in git" per this repo's convention, same
  as the stock `.chk`/`.zip` in `images/` (see "Recovery net" above for
  obtaining the stock firmware if it's not already on disk). If the rootfs
  extraction is gone, re-extract `R8000-V1.0.4.88_10.1.88.chk` (squashfs) to
  get `dhd.ko` back, then:
  ```bash
  DHD=hwoffload-research/blob-analysis/rootfs/lib/modules/2.6.36.4brcmarm+/kernel/drivers/net/dhd/dhd.ko
  SEC_OFF=$(readelf -S "$DHD" | awk '/\.init\.data/{print "0x"$5; exit}')
  SYM=$(readelf -s "$DHD" | awk '/dlarray_43602a1/{print $2, $3; exit}')
  SYM_VAL=$(echo "$SYM" | awk '{print "0x"$1}')
  SYM_SIZE=$(echo "$SYM" | awk '{print strtonum("0x"$2)}')
  ABS_OFF=$(( SEC_OFF + SYM_VAL ))
  mkdir -p v2-files/lib/firmware/brcm
  dd if="$DHD" of=v2-files/lib/firmware/brcm/brcmfmac43602-pcie.bin bs=1 skip=$ABS_OFF count=$SYM_SIZE
  # sanity check: should print "version 7.10.274.3.REBASE.R493518 ... Date: Wed 2021-06-02"
  strings v2-files/lib/firmware/brcm/brcmfmac43602-pcie.bin | grep -i "43602a1-roml.*Version:"
  ```
  Confirmed reproducible (byte-identical container/header format to the
  stock-loaded blob, correct chip stepping match to `BCM43602/1` in dmesg).
- **ImageBuilder (fast, no toolchain compile) — the actual recipe used for
  v7 through the current shipping image**, reconstructed and verified
  2026-07-24 against the live router's own `apk list --installed` (ground
  truth, not the stale doc this replaces):
  ```bash
  cd openwrt-imagebuilder-25.12.5-bcm53xx-generic.Linux-x86_64
  make image PROFILE=netgear_r8000 \
    PACKAGES="-wpad-basic-mbedtls wpad-mbedtls hostapd-utils wpa-cli wireless-regdb \
      luci usteer sqm-scripts ethtool relayd \
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
  - **`relayd`** — required by `etc/init.d/eufy-repeater` (the WiFi repeater
    feature, see `docs/FINDINGS.md` §15 and `wireless.example`'s repeater
    section). Confirmed 2026-07-24: `relayd` is a real, pre-built package in
    the 25.12.5 feed (no compilation needed) — but note it ships the older
    standalone-init-script implementation (`/etc/init.d/relayd`, direct
    `relayd -I ... -I ...` invocation), not the newer netifd `proto=relay`
    style some online docs describe (`/lib/netifd/proto/relay.sh` does not
    exist in this package). `eufy-repeater` invokes the `relayd` binary
    directly and does not depend on either init mechanism.
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

Automates §5's build recipe against every new OpenWrt point release. Runs
entirely on GitHub-hosted runners — no self-hosted runner, no LAN access to
the router, no persistent SSH credential on it. `.github/workflows/track-openwrt.yml`
(daily detector) → `.github/workflows/release.yml` (build → static-verify →
publish) → `.github/scripts/*.sh` (the actual logic, kept as plain scripts
so they're testable by hand).

**What this actually proves, stated honestly:**

| Stage | Proves | Does NOT prove |
|---|---|---|
| `build-image.sh` | The exact §5 recipe (SDK rebuild of `patches/861` + ImageBuilder assembly) reproduces cleanly against a new OpenWrt version | Nothing about correctness — a broken patch can still "compile" |
| `static-verify.sh` | The patched module (not the stock feed one) got embedded; package manifest didn't silently lose packages; the DTB parses and identifies as this board | **Anything about runtime behavior — this is the ceiling.** No real radio ever negotiates against this build in CI. |

This project deliberately ran a self-hosted-runner variant of this pipeline
earlier (a machine on the operator's own LAN doing a live-swap-and-revert
test against the real router before publishing) and it worked — confirmed via
a real green run that swapped the built module onto the actual router, all
3 radios came up clean, and reverted. **Traded back down to static-only by
explicit choice**, not because the hardware test failed: running everything
on GitHub's default infrastructure was worth more than the extra verification
tier, and no persistent SSH credential sits on the router as a result. If
that tradeoff ever needs revisiting, the hardware-verify approach (git history
around the `hardware-verify.sh` script, now removed) is the known-working
starting point, not something to redesign from scratch.

Publishing (`auto-<version>`, marked `--latest`) happens whenever static-verify
passes — meaning "compiles clean and looks structurally sane," not "verified
on real hardware." `v11` (hand-flashed, lived with on the real router) remains
the reference for what real-hardware verification looked like.

**One-time setup required (cannot be automated from here):**

**`WIRELESS_CONFIG_B64` repo secret** — `v2-files/etc/config/wireless` is
gitignored (real passphrases), so CI needs it supplied out-of-band:
```bash
base64 -w0 v2-files/etc/config/wireless | gh secret set WIRELESS_CONFIG_B64 \
  --repo 88plug/netgear-r8000-plus
```
Already set as of this writing.

**Test the pipeline manually before trusting the scheduled trigger**:
`gh workflow run release.yml --repo 88plug/netgear-r8000-plus -f openwrt_version=25.12.5`
(the version already shipping — should build and static-verify clean, giving
a known-good dry run before the tracker ever fires on a real new release).

**Branch-pattern reminder:** `track-openwrt.yml`'s `OPENWRT_BRANCH_PATTERN`
is deliberately pinned to `25.12.x` and does not auto-widen to a new
minor/major OpenWrt release — see the "Tracking scope" decision this project
made when this pipeline was designed. A new minor/major bump needs a human
to bump the pattern after actually reviewing what changed upstream (kernel
version, config schema, package availability can all shift in ways a point
release doesn't).
