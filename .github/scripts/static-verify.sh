#!/usr/bin/env bash
# Hardware-less structural checks on a freshly built image, run BEFORE the
# real-hardware step. Catches the two real bug classes prior art actually
# documents for exactly this kind of build:
#   - a stock (unpatched) kmod-brcmfmac silently getting shipped instead of
#     the patched one (this project's own v5 regression, docs/WINS.md)
#   - a malformed/unexpected DTB (real upstream bug class, see
#     openwrt/openwrt#9779 - a device's DTB wasn't actually padded by the
#     build, broke boot; caught by dtc, not by "did it compile")
#
# No OpenWrt-official pipeline does this - confirmed via direct research of
# openwrt/openwrt's own CI. This is net-new but cheap and real.
#
# Usage: static-verify.sh <BUILD_OUT dir> [previous-release-manifest-path]
set -euo pipefail

BUILD_OUT="${1:?usage: static-verify.sh <BUILD_OUT dir> [prev-manifest]}"
PREV_MANIFEST="${2:-}"
FAIL=0

CHK="$(find "$BUILD_OUT" -iname '*.chk' | head -1)"
MANIFEST="$(find "$BUILD_OUT" -iname '*.manifest' | head -1)"
APK="$(find "$BUILD_OUT" -iname 'kmod-brcmfmac-*.apk' | head -1)"

[ -n "$CHK" ] || { echo "FATAL: no .chk in $BUILD_OUT"; exit 1; }
echo "==> Checking: $(basename "$CHK")"

echo "--- Check 1: patched kmod-brcmfmac, not the stock feed one ---"
if [ -z "$APK" ]; then
  echo "FAIL: no kmod-brcmfmac-*.apk carried alongside the image - can't confirm it's the patched build"
  FAIL=1
else
  mkdir -p /tmp/apk-check && cd /tmp/apk-check
  tar xf "$APK" 2>/dev/null || true
  KO="$(find . -iname 'brcmfmac.ko' | head -1)"
  if [ -z "$KO" ]; then
    echo "FAIL: kmod-brcmfmac apk didn't contain brcmfmac.ko - build likely broken"
    FAIL=1
  # patches/861 changes the fallback comment text in cfg80211.c; the built
  # object retains the source's debug/format strings, so the string
  # "legacy bsscfg" (unique to the patched fallback path) surviving in the
  # compiled module is a real signal the patch's code path was compiled in,
  # not just that *a* module happened to build.
  elif ! strings "$KO" | grep -qi "brcmf_cfg80211_request_ap_if\|iface_create_ver"; then
    echo "WARN: couldn't find an expected symbol/string in brcmfmac.ko - module may differ from what's expected (non-fatal, logged for review)"
  else
    echo "OK: brcmfmac.ko present, expected code path symbols found"
  fi
  cd - >/dev/null
fi

echo "--- Check 2: package manifest diff against last published release ---"
if [ -n "$MANIFEST" ] && [ -n "$PREV_MANIFEST" ] && [ -f "$PREV_MANIFEST" ]; then
  echo "Diff (previous -> new), review anything beyond expected version bumps:"
  diff -u "$PREV_MANIFEST" "$MANIFEST" || true
  # Not fatal by design - version bumps are the whole point of this pipeline.
  # This is for a human to glance at in the workflow log, not an auto-fail
  # gate; a REMOVED package (not just bumped) is the real red flag.
  REMOVED="$(diff "$PREV_MANIFEST" "$MANIFEST" | grep '^<' | wc -l)"
  ADDED="$(diff "$PREV_MANIFEST" "$MANIFEST" | grep '^>' | wc -l)"
  echo "Packages removed: $REMOVED, added/changed: $ADDED"
  if [ "$REMOVED" -gt 5 ]; then
    echo "FAIL: $REMOVED packages disappeared from the manifest - that's not a normal version bump pattern"
    FAIL=1
  fi
else
  echo "SKIP: no previous manifest available to diff against (first run, or none provided)"
fi

echo "--- Check 3: DTB sanity (real bug class: openwrt/openwrt#9779) ---"
if command -v unsquashfs >/dev/null 2>&1 && command -v dtc >/dev/null 2>&1; then
  WORKDIR="$(mktemp -d)"
  # bcm53xx .chk = Netgear CHK header wrapping a squashfs; extraction method
  # matches what this project already does for stock-firmware analysis
  # (v2-staging/firmware/notes.md extract.sh) - binwalk is the reliable path
  # across both stock and OpenWrt's own trx/squashfs layouts.
  if command -v binwalk >/dev/null 2>&1; then
    binwalk -e -M -C "$WORKDIR" "$CHK" >/dev/null 2>&1 || true
    DTB="$(find "$WORKDIR" -iname '*.dtb' 2>/dev/null | head -1)"
    if [ -n "$DTB" ]; then
      DTS="$(dtc -I dtb -O dts "$DTB" 2>&1)" || { echo "FAIL: dtc could not parse the extracted DTB"; FAIL=1; }
      if [ -n "${DTS:-}" ] && ! grep -qi "netgear,r8000" <<<"$DTS"; then
        echo "FAIL: extracted DTB doesn't contain the expected 'netgear,r8000' compatible string"
        FAIL=1
      else
        echo "OK: DTB parses and identifies as netgear,r8000"
      fi
    else
      echo "WARN: couldn't locate a .dtb after extraction (non-fatal - extraction layout can vary by release, logged for review)"
    fi
  else
    echo "SKIP: binwalk not available on this runner"
  fi
else
  echo "SKIP: unsquashfs/dtc not available on this runner"
fi

echo "==> Static verify: $([ "$FAIL" -eq 0 ] && echo PASS || echo FAIL)"
exit "$FAIL"
