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
./scripts/feeds update -a >/dev/null
./scripts/feeds install -a >/dev/null

MAC80211_PATCH_DIR="package/kernel/mac80211/patches/brcm"
mkdir -p "$MAC80211_PATCH_DIR"
cp "$REPO_ROOT/patches/861-brcmfmac-r8000-legacy-mbss-fallback.patch" "$MAC80211_PATCH_DIR/"

make defconfig >/dev/null
make package/kernel/mac80211/{clean,download,prepare,compile} -j"$(nproc)" V=s

APK="$(find bin/packages -iname 'kmod-brcmfmac-*.apk' | head -1)"
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
sha256sum "$OUT"/*.chk > "$OUT/sha256sums"

echo "==> Built: $(basename "$CHK")"
cat "$OUT/sha256sums"
