#!/usr/bin/env bash
# extract.sh — reproduce the brcmfmac43602-pcie firmware/CLM extraction in this directory.
#
# Produces, in $OUTDIR:
#   brcmfmac43602-pcie.bin       newest upstream STA-mode firmware (linux-firmware, kernel.org)
#   brcmfmac43602-pcie.ap.bin    newest upstream AP-mode firmware  (linux-firmware, kernel.org)
#   brcmfmac43602-pcie.clm_blob  CLM regulatory blob carved from the operator's own stock
#                                 R8000 firmware image (chip BCM43602 rev A1, matching the
#                                 on-device chip per dmesg "BCM43602/1")
#
# No brcmfmac43602-pcie.txcap_blob is produced — see notes.md for why.
#
# Requires: curl, binwalk (>=3.0), unsquashfs (squashfs-tools), readelf (binutils), python3.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTDIR="${OUTDIR:-$SCRIPT_DIR}"
WORKDIR="${WORKDIR:-$OUTDIR/extractions}"
STOCK_CHK="${STOCK_CHK:-/home/andrew/netgearr8000/images/R8000-V1.0.4.88_10.1.88.chk}"
LFW_BASE="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"

mkdir -p "$OUTDIR" "$WORKDIR"

# ---------------------------------------------------------------------------
# Step 1 — newest upstream brcmfmac43602-pcie firmware.
#
# The canonical linux-firmware tree lives at kernel.org
# (git://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git),
# mirrored at https://gitlab.com/kernel-firmware/linux-firmware.git. There is
# no "github.com/torvalds/linux-firmware" — that repo does not exist; several
# unofficial read-only GitHub mirrors do (lumag/linux-firmware etc.) but the
# kernel.org cgit "plain" endpoint used below is authoritative and needs no
# clone.
#
# WHENCE confirms only *.bin and *.ap.bin exist for 43602 upstream — no
# clm_blob/txcap_blob ship for ANY BCM4360-family chip (4358/43602/4366b/
# 4366c/4371). Only newer Cypress-era chips (4356, 43455, 43012, 43430, ...)
# get a clm_blob in linux-firmware, and none of those are close enough to
# 43602 to trust as a drop-in substitute (CLM data is chip+firmware-build
# specific). This is *why* step 2 (vendor extraction) is the real work.
# ---------------------------------------------------------------------------
echo "== fetching newest upstream brcmfmac43602-pcie firmware =="
curl -sL "$LFW_BASE/brcm/brcmfmac43602-pcie.bin"    -o "$OUTDIR/brcmfmac43602-pcie.bin"
curl -sL "$LFW_BASE/brcm/brcmfmac43602-pcie.ap.bin" -o "$OUTDIR/brcmfmac43602-pcie.ap.bin"
curl -sL "$LFW_BASE/WHENCE"                          -o "$WORKDIR/WHENCE.txt"

# ---------------------------------------------------------------------------
# Step 2 — pull the CLM data out of the operator's own stock R8000 image.
# ---------------------------------------------------------------------------
echo "== binwalk: identify + extract the TRX partitions from the stock .chk =="
binwalk -e -M -C "$WORKDIR" "$STOCK_CHK"

TRXDIR="$WORKDIR/$(basename "$STOCK_CHK").extracted/3A"
SQFS="$TRXDIR/partition_1.bin"   # squashfs rootfs partition (standard squashfs 4.0, xz)

echo "== unsquashfs: unpack the vendor rootfs =="
rm -rf "$WORKDIR/squashfs-root"
# -no-xattrs: unsquashfs exits 2 (tripping set -e) when run unprivileged and
# it can't restore security.selinux xattrs; we don't need xattrs here.
unsquashfs -no-xattrs -d "$WORKDIR/squashfs-root" "$SQFS"

KO="$WORKDIR/squashfs-root/lib/modules/2.6.36.4brcmarm+/kernel/drivers/net/dhd/dhd.ko"
[ -f "$KO" ] || { echo "dhd.ko not found at expected path" >&2; exit 1; }

echo "== dhd.ko is a plain ELF relocatable object (2.6.36 kernel module, not compressed) =="
file "$KO"

# ---------------------------------------------------------------------------
# The vendor "dhd" FullMAC driver statically links a per-chip-revision
# "download array" containing [RAM firmware image][CLM blob] concatenated,
# used for its embedded/no-separate-file firmware-load mode. The ELF symbol
# table names these arrays directly and gives their EXACT size (no guessing
# at byte boundaries by eye):
#
#   $ readelf -sW dhd.ko | grep dlarray
#     ... OBJECT LOCAL ... dlarray_43602a0   (chip rev A0)
#     ... OBJECT LOCAL ... dlarray_43602a1   (chip rev A1 — matches this
#                                             router: dmesg says "BCM43602/1")
#
# Absolute file offset = <.init.data section file offset> + <symbol value>.
#
# Within dlarray_43602a1 the CLM sub-blob is located by its own literal
# magic tag "CLM DATA" (8 bytes ASCII). This is confirmed against the real
# generated source for this exact struct (RMerl/asuswrt-merlin
# wlc_clm_data.c, a GPL-released clm_data.c for a different Broadcom router):
#
#   const struct clm_data_header clm_header = {
#       CLM_HEADER_TAG,      /* Magic word */          <- "CLM DATA"
#       11, 0,                /* BLOB format version */
#       "7.7.5", "1.18.3",    /* CLM version, compiler version */
#       &clm_header,           /* Self reference */
#       &clm_data,             /* Data registry */
#       "ClmImport: 1.17.7",  /* generator version */
#       "Broadcom-0.0",        /* SW Apps version */
#   };
#   /** BLOB header — Base of all references ... */
#
# i.e. the tag sits at byte 0 of the blob; nothing precedes it. Our own
# binary matches this layout field-for-field (format version ints, a CLM
# version string, a compiler version string, two 4-byte pointers, a
# "ClmImport: 1.36.0" generator-version string, "Broadcom-0.0"), and the
# tag occurs exactly ONCE inside dlarray_43602a1. The blob runs from that
# tag to the end of the array (the array's own ELF-symbol-bounded end,
# i.e. NOT eyeballed), corroborated by content: the blob's own trailing
# metadata self-declares "43602a1-roml/... Ucode Ver: ... FWID: ..." —
# i.e. a chip/build compatibility stamp, consistent with real CLM registry
# data, and nothing else follows it in the array.
# ---------------------------------------------------------------------------
echo "== readelf + carve: dlarray_43602a1 -> CLM sub-blob =="

python3 - "$KO" "$OUTDIR/brcmfmac43602-pcie.clm_blob" <<'PYEOF'
import re
import subprocess
import sys

ko, out_path = sys.argv[1], sys.argv[2]

# --- section table: name -> (file offset, size) ---
sh = subprocess.run(["readelf", "-SW", ko], capture_output=True, text=True, check=True).stdout
sections = {}
for line in sh.splitlines():
    m = re.match(r"\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
    if m:
        idx, name, addr, off, size = m.groups()
        sections[int(idx)] = (name, int(off, 16), int(size, 16))

init_idx = next(i for i, (n, o, s) in sections.items() if n == ".init.data")
init_off = sections[init_idx][1]

# --- symbol table: find dlarray_43602a1's value+size, confirm section ---
st = subprocess.run(["readelf", "-sW", ko], capture_output=True, text=True, check=True).stdout
sym = None
for line in st.splitlines():
    if "dlarray_43602a1" in line:
        parts = line.split()
        value = int(parts[1], 16)
        size = int(parts[2], 0)
        ndx = parts[6]
        if ndx == str(init_idx):
            sym = (value, size)
if sym is None:
    sys.exit("dlarray_43602a1 symbol not found in .init.data")

rel, size = sym
start = init_off + rel
end = start + size
print(f"dlarray_43602a1: abs file offset {start} (0x{start:x}) .. {end} (0x{end:x}), size {size} bytes")

data = open(ko, "rb").read()
region = data[start:end]

tag = b"CLM DATA"
occurrences = [m.start() for m in re.finditer(re.escape(tag), region)]
if len(occurrences) != 1:
    sys.exit(f"expected exactly one CLM DATA tag inside dlarray_43602a1, found {len(occurrences)}")
clm_rel = occurrences[0]
clm_abs = start + clm_rel
clm_len = len(region) - clm_rel
print(f"CLM blob: abs file offset {clm_abs} (0x{clm_abs:x}) .. {end}, size {clm_len} bytes")

with open(out_path, "wb") as f:
    f.write(region[clm_rel:])
print(f"wrote {clm_len} bytes -> {out_path}")
PYEOF

echo "== done =="
sha256sum "$OUTDIR/brcmfmac43602-pcie.bin" "$OUTDIR/brcmfmac43602-pcie.ap.bin" "$OUTDIR/brcmfmac43602-pcie.clm_blob"
