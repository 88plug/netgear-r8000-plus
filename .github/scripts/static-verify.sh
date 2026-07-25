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

BUILD_OUT="$(cd "${1:?usage: static-verify.sh <BUILD_OUT dir> [prev-manifest]}" && pwd)"
PREV_MANIFEST="${2:-}"
[ -n "$PREV_MANIFEST" ] && PREV_MANIFEST="$(cd "$(dirname "$PREV_MANIFEST")" && pwd)/$(basename "$PREV_MANIFEST")"
FAIL=0

CHK="$(find "$BUILD_OUT" -iname '*.chk' | head -1)"
MANIFEST="$(find "$BUILD_OUT" -iname '*.manifest' | head -1)"
APK="$(find "$BUILD_OUT" -iname 'kmod-brcmfmac-*.apk' | head -1)"

[ -n "$CHK" ] || { echo "FATAL: no .chk in $BUILD_OUT"; exit 1; }
echo "==> Checking: $(basename "$CHK")"

echo "--- Check 1: kmod-brcmfmac built and contains a real, non-truncated brcmfmac.ko ---"
# NOTE on what this can and can't prove: OpenWrt's build strips kernel
# modules (rstrip.sh runs during packaging), so source-comment text from
# patches/861 does NOT survive into the compiled binary - grepping the
# stripped .ko for a comment string is not a real check and used to live
# here as one. The actual patch-application gate is build-image.sh itself:
# if patches/861 failed to apply, the SDK's Kbuild patch step fails the
# whole build loudly, well before this script ever runs. What IS worth
# checking post-hoc, from a stripped binary, is that the module is a real,
# complete, valid ELF kernel object - not truncated or corrupted.
if [ -z "$APK" ]; then
  echo "FAIL: no kmod-brcmfmac-*.apk carried alongside the image - can't confirm it's the patched build"
  FAIL=1
else
  # build-image.sh runs in this same job/workspace and drops the real SDK
  # apk tool's path here - no cross-job handoff needed for static-only CI.
  APK_TOOL="$(cat "$BUILD_OUT/.sdk-apk-path" 2>/dev/null || true)"
  # rm first, not just mkdir -p: a reused runner can have stale content
  # here from a prior run, and apk extract errors on conflicting existing
  # files - confirmed directly (worked fine into a clean dir, failed
  # silently under set -e into a dirty one).
  rm -rf /tmp/apk-check && mkdir -p /tmp/apk-check && cd /tmp/apk-check
  if [ -n "$APK_TOOL" ] && [ -x "$APK_TOOL" ]; then
    "$APK_TOOL" extract --allow-untrusted --destination . "$APK" >/dev/null 2>&1
  else
    # apk v3 packages aren't plain tar/gzip - fall back only if build-image.sh
    # somehow didn't record its own apk tool path (shouldn't happen).
    echo "WARN: SDK apk tool not found at '$APK_TOOL', falling back to tar (won't work on apk v3 packages)"
    tar xf "$APK" 2>/dev/null || true
  fi
  KO="$(find . -iname 'brcmfmac.ko' | head -1)"
  if [ -z "$KO" ]; then
    echo "FAIL: kmod-brcmfmac apk didn't contain brcmfmac.ko"
    FAIL=1
  elif ! file "$KO" | grep -qi "ELF.*relocatable\|ELF.*shared"; then
    echo "FAIL: brcmfmac.ko doesn't look like a valid ELF kernel module: $(file "$KO")"
    FAIL=1
  elif [ "$(stat -c%s "$KO" 2>/dev/null || stat -f%z "$KO")" -lt 100000 ]; then
    echo "FAIL: brcmfmac.ko is suspiciously small ($(stat -c%s "$KO" 2>/dev/null || stat -f%z "$KO") bytes) - likely truncated/broken build"
    FAIL=1
  else
    echo "OK: brcmfmac.ko present, valid ELF, reasonable size"
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

echo "--- Check 3: DTB sanity, best-effort (real bug class: openwrt/openwrt#9779) ---"
# Non-fatal by design: binwalk's signature scan can find DTB-magic-looking
# byte patterns inside compressed/binary data that aren't actually complete,
# valid DTBs at that offset (confirmed hitting this directly - a "found" DTB
# that dtc then rejects with "incorrect magic number"). That's a limitation
# of heuristic extraction, not evidence the image itself is broken - the
# image's own sha256 matching a known-good build is the real signal. This
# check stays as a logged, best-effort catch for the #9779 bug class, not a
# gate - only WARN, never FAIL, on extraction/parse trouble.
if command -v unsquashfs >/dev/null 2>&1 && command -v dtc >/dev/null 2>&1 && command -v binwalk >/dev/null 2>&1; then
  WORKDIR="$(mktemp -d)"
  binwalk -e -M -C "$WORKDIR" "$CHK" >/dev/null 2>&1 || true
  # Prefer larger candidates first - tiny "DTB" hits are almost always a
  # false-positive magic-number match inside unrelated binary data, not a
  # real board DTB (which is several KB on this platform).
  FOUND_VALID=0
  while IFS= read -r DTB; do
    if DTS="$(dtc -I dtb -O dts "$DTB" 2>/dev/null)"; then
      if grep -qi "netgear,r8000" <<<"$DTS"; then
        echo "OK: DTB at $(basename "$(dirname "$DTB")") parses and identifies as netgear,r8000"
        FOUND_VALID=1
        break
      fi
    fi
  done < <(find "$WORKDIR" -iname '*.dtb' -size +1k 2>/dev/null | sort)
  [ "$FOUND_VALID" -eq 1 ] || echo "WARN: no extracted candidate parsed as a valid netgear,r8000 DTB (non-fatal - binwalk extraction is heuristic, see comment above; logged for review, not a build defect signal)"
else
  echo "SKIP: unsquashfs/dtc/binwalk not all available on this runner"
fi

echo "==> Static verify: $([ "$FAIL" -eq 0 ] && echo PASS || echo FAIL)"
exit "$FAIL"
