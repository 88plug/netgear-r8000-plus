#!/usr/bin/env bash
# Build a netgear-r8000-plus image against a given OpenWrt point release.
# Mirrors docs/RUNBOOK.md §5 exactly - same two-stage recipe (SDK rebuild of
# the patched kmod-brcmfmac, then ImageBuilder assembly), just parametrized
# by version and run headless in CI instead of by hand.
#
# Usage: build-image.sh <openwrt-version, e.g. 25.12.6>
# Output: BUILD_OUT/<image>.chk, BUILD_OUT/<image>.manifest, BUILD_OUT/sha256sums
set -euo pipefail

VERSION="${1:?usage: build-image.sh <openwrt-version>}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
WORK="$(pwd)/openwrt-build-${VERSION}"
OUT="$(pwd)/BUILD_OUT"
BASE_URL="https://downloads.openwrt.org/releases/${VERSION}/targets/bcm53xx/generic"

mkdir -p "$WORK" "$OUT"
cd "$WORK"

echo "==> Discovering exact SDK/ImageBuilder filenames for ${VERSION} (gcc version varies per release, don't hardcode it)"
LISTING="$(curl -fsSL "${BASE_URL}/")"
SDK_TARBALL="$(grep -oE 'openwrt-sdk-[^"]+\.tar\.(zst|xz)' <<<"$LISTING" | head -1)"
IB_TARBALL="$(grep -oE 'openwrt-imagebuilder-[^"]+\.tar\.(zst|xz)' <<<"$LISTING" | head -1)"
[ -n "$SDK_TARBALL" ] || { echo "FATAL: no SDK tarball found for ${VERSION} at ${BASE_URL}/"; exit 1; }
[ -n "$IB_TARBALL" ] || { echo "FATAL: no ImageBuilder tarball found for ${VERSION} at ${BASE_URL}/"; exit 1; }
echo "SDK: $SDK_TARBALL"
echo "ImageBuilder: $IB_TARBALL"

echo "==> Downloading + verifying against upstream sha256sums"
curl -fsSL -O "${BASE_URL}/${SDK_TARBALL}"
curl -fsSL -O "${BASE_URL}/${IB_TARBALL}"
curl -fsSL -O "${BASE_URL}/sha256sums"
sha256sum --ignore-missing -c sha256sums

SDK_DIR="${SDK_TARBALL%.tar.*}"
IB_DIR="${IB_TARBALL%.tar.*}"
tar xf "$SDK_TARBALL"
tar xf "$IB_TARBALL"

echo "==> Stage 1: rebuild kmod-brcmfmac with patches/861 applied (SDK - module only, never a full kernel rebuild)"
cd "$WORK/$SDK_DIR"

# Deliberately NOT running ./scripts/feeds update/install -a: confirmed by
# direct testing that an SDK release tarball ships package/kernel/ with only
# linux/ present - mac80211 is a "base" feed package (feeds.conf.default's
# src-git --root=package base entry) that a fresh SDK does NOT carry, but
# feeds install -a pulls in every OTHER feed package's Kconfig too and, on
# 25.12.5, that made package/kernel/mac80211/clean's target resolution
# non-deterministic (sometimes failed outright) - never needed for building
# one base-tree kernel module. Fetching mac80211 directly, sparse and
# shallow, from the exact commit feeds.conf.default itself pins, is
# deterministic and doesn't touch anything else in package/.
BASE_FEED_COMMIT="$(awk '/^src-git --root=package base /{print $NF}' feeds.conf.default | sed 's/.*\^//')"
[ -n "$BASE_FEED_COMMIT" ] || { echo "FATAL: couldn't find the base feed's pinned commit in feeds.conf.default - format may have changed upstream"; exit 1; }
echo "base feed pinned at: $BASE_FEED_COMMIT"

MAC80211_SRC="$(mktemp -d)"
(
  cd "$MAC80211_SRC"
  git init -q
  git remote add origin https://git.openwrt.org/openwrt/openwrt.git
  git config core.sparseCheckout true
  echo "package/kernel/mac80211/*" > .git/info/sparse-checkout
  git fetch --depth 1 origin "$BASE_FEED_COMMIT"
  git checkout FETCH_HEAD -- package/kernel/mac80211
)
rm -rf package/kernel/mac80211
cp -r "$MAC80211_SRC/package/kernel/mac80211" package/kernel/
rm -rf "$MAC80211_SRC"

MAC80211_PATCH_DIR="package/kernel/mac80211/patches/brcm"
mkdir -p "$MAC80211_PATCH_DIR"
PATCH="$REPO_ROOT/patches/861-brcmfmac-r8000-legacy-mbss-fallback.patch"
cp "$PATCH" "$MAC80211_PATCH_DIR/"

# make defconfig must run AFTER mac80211 is in place - its Kconfig symbols
# (CONFIG_PACKAGE_kmod-brcmfmac etc) don't exist in .config otherwise, which
# silently breaks the clean/compile targets later with a confusing
# "No rule to make target" error that has nothing to do with the patch.
make defconfig >/dev/null
grep -q '^CONFIG_PACKAGE_kmod-brcmfmac=' .config || { echo "FATAL: CONFIG_PACKAGE_kmod-brcmfmac missing from .config after defconfig - mac80211 didn't get picked up"; exit 1; }

make package/kernel/mac80211/{clean,download,prepare,compile} -j"$(nproc)" V=s

APK="$(find bin -iname 'kmod-brcmfmac-*.apk' | head -1)"
[ -n "$APK" ] || { echo "FATAL: kmod-brcmfmac apk not produced - patch may have failed to apply, check the log above"; exit 1; }
echo "Built: $APK"

echo "==> Stage 2: ImageBuilder assembly with the patched package + v2-files overlay"
cd "$WORK/$IB_DIR"
mkdir -p packages
cp "$WORK/$SDK_DIR/$APK" packages/

# First-clone safety net: build fails loudly, not silently on a stock module,
# if this ever regresses (this exact mistake shipped v5 with the unpatched
# driver - see docs/WINS.md v5->v6). Confirm the patched .apk is really staged.
ls packages/kmod-brcmfmac-*.apk >/dev/null

if [ ! -f "$REPO_ROOT/v2-files/etc/config/wireless" ]; then
  echo "FATAL: v2-files/etc/config/wireless missing (gitignored, real passphrases) - CI needs it provided as a secret-backed file, not built from wireless.example placeholders"
  exit 1
fi

make image PROFILE=netgear_r8000 \
  PACKAGES="-wpad-basic-mbedtls wpad-mbedtls hostapd-utils wpa-cli wireless-regdb \
    luci usteer sqm-scripts ethtool \
    kmod-brcmfmac brcmfmac-firmware-43602a1-pcie kmod-usb-ohci kmod-usb2 \
    kmod-phy-bcm-ns-usb2 kmod-usb-ledtrig-usbport kmod-usb3 kmod-phy-bcm-ns-usb3" \
  FILES="$REPO_ROOT/v2-files" \
  EXTRA_IMAGE_NAME="r8000plus-auto-${VERSION}"

CHK="$(find bin/targets/bcm53xx/generic -iname '*.chk' | head -1)"
[ -n "$CHK" ] || { echo "FATAL: make image did not produce a .chk"; exit 1; }
MANIFEST="$(find bin/targets/bcm53xx/generic -iname '*.manifest' | head -1)"

cp "$CHK" "$OUT/"
[ -n "$MANIFEST" ] && cp "$MANIFEST" "$OUT/"
cp "$WORK/$SDK_DIR/$APK" "$OUT/"

# static-verify.sh runs in this same job/workspace right after this script,
# so it can use the SDK's own apk tool directly - no need to bundle/tar it
# for a cross-job/cross-runner handoff (that only mattered when a separate
# self-hosted runner did hardware-verify; static-only doesn't need it).
# kmod-brcmfmac-*.apk is apk v3 (ADB format, magic bytes "ADBd"), not a
# plain tar/gzip archive - static-verify.sh needs this path to extract it.
echo "$WORK/$SDK_DIR/staging_dir/host/bin/apk" > "$OUT/.sdk-apk-path"

sha256sum "$OUT"/*.chk > "$OUT/sha256sums"

echo "==> Built: $(basename "$CHK")"
cat "$OUT/sha256sums"
