# brcmfmac43602-pcie firmware + CLM calibration

Target chip (confirmed from `extracted/boot-results/dmesg-brcmfmac.txt` and
`extracted/calibration.txt`): **BCM43602 revision A1** ("BCM43602/1" in
dmesg), all three R8000 radios (`devid` 0x43BB/0x43BC), `boardtype=0x0665`,
running vendor RAM firmware **7.35.177.56** (built 2015-09-18, FWID
01-6cb8e269). On-device symptoms before this work:

```
brcmfmac 0001:03:00.0: Direct firmware load for brcm/brcmfmac43602-pcie.clm_blob failed with error -2
brcmfmac 0001:03:00.0: Direct firmware load for brcm/brcmfmac43602-pcie.txcap_blob failed with error -2
brcmfmac: brcmf_c_process_clm_blob: no clm_blob available (err=-2), device may have limited channels available
brcmfmac: brcmf_c_process_txcap_blob: no txcap_blob available (err=-2)
```

Destination in v2: `/lib/firmware/brcm/`.

## 1. Newest upstream `brcmfmac43602-pcie.bin`

Canonical source is kernel.org, not GitHub — **`github.com/torvalds/linux-firmware`
does not exist.** The canonical tree is
`git://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git`
(cgit: https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/),
mirrored at `https://gitlab.com/kernel-firmware/linux-firmware.git`. Several
unofficial read-only GitHub mirrors exist (e.g. `lumag/linux-firmware`) but
aren't needed — the kernel.org cgit `/plain/` endpoint serves files directly,
no clone required.

`WHENCE` lists exactly two 43602 firmware files, no clm_blob/txcap_blob:

| file | version | date | FWID | ucode | sha256 |
|---|---|---|---|---|---|
| `brcm/brcmfmac43602-pcie.bin` (STA) | **7.35.177.61** | 2015-11-10 | 01-ea662a8c | 986.122 | `bf4cfc23ee952a3d82ef33a0f5f87853201c98f1bed034876a910f354f37862d` |
| `brcm/brcmfmac43602-pcie.ap.bin` (AP) | **7.35.177.56** | 2015-09-18 | 01-6cb8e269 | 986.122 | `7f735b7254527ce7cae84a854211879a4b85d6a77a92210c4d92bcf12a35618b` |

Both downloaded to this directory as `brcmfmac43602-pcie.bin` and
`brcmfmac43602-pcie.ap.bin`.

**Important finding:** the router is an AP device, and its currently-running
firmware (`sha256sum` taken live over SSH on `/lib/firmware/brcm/brcmfmac43602-pcie.bin`,
595472 bytes) is **byte-for-byte identical** to upstream's `.ap.bin`
(`7f735b72...`) — same version, date and FWID as what's already deployed.
**There is no newer AP firmware to ship.** The generic-named `.bin` upstream
file is the *STA*-mode build (7.35.177.61, slightly newer, different FWID) —
deploying that under the generic `brcmfmac43602-pcie.bin` name would swap in
STA-oriented firmware on an AP-only device, which is very likely a downgrade
for this use case, not an upgrade. Recommendation: **keep the AP firmware
already on the box** (equivalently, keep shipping the content of
`brcmfmac43602-pcie.ap.bin` under the generic filename in the v2 image); the
STA `.bin` is included here for completeness/reference only, not as something
to deploy in place of the AP build.

## 2. CLM blob — extracted from the operator's own stock image

### Why extraction from the stock image was the only real option

Checked `WHENCE` for every BCM4360-family sibling of 43602 (4358, 4366b,
4366c, 4350/4350c2, 4371): **none of them ship a `clm_blob` in linux-firmware
either.** Only newer Cypress-era chips (4356, 43012, 43430, 43455, 4354,
43570, 4373, 54591) get a `clm_blob`/`txcap_blob` upstream — this matches the
well-documented history that Cypress never contributed clm_blob files for the
older pure-Broadcom chip generation, only special all-in-one firmware
builds for *newer* chips. There is **no compatible drop-in `.clm_blob`
fallback available anywhere in linux-firmware for the 43602 family** — CLM
data is chip- and firmware-build specific, and grabbing e.g. a 4356-pcie
clm_blob would be for a different silicon/firmware combination the driver's
CLM version check would very likely reject (and even if accepted, would carry
wrong power/channel tables for this hardware). So the stock-image extraction
below isn't a nice-to-have — it's the only path to a genuine, correctly
calibrated clm_blob for this chip.

### Method (full commands in `extract.sh`)

1. `binwalk -e -M` (binwalk 3.1.0, installed via `pacman -S binwalk`) on the
   operator's saved stock image `images/R8000-V1.0.4.88_10.1.88.chk`
   identifies a Netgear **CHK** header (board ID `U12H315T00_NETGEAR`)
   wrapping a **TRX** image with 2 partitions:
   - partition 0: LZMA-compressed Linux kernel (2.6.36.4brcmarm+, built
     2024-05-08 — the vendor's rebuilt/patched kernel for this firmware
     release)
   - partition 1: standard **SquashFS 4.0, xz**, 1778 inodes, 28132416 bytes
     (binwalk's bundled `sasquatch` extractor errors out — `file`/`unsquashfs`
     correctly recognize this as *standard* squashfs, so the fix is just to
     run plain `unsquashfs` on the carved partition directly)
2. `unsquashfs -no-xattrs` unpacks the vendor rootfs. It contains the
   userspace `wl` CLI (`usr/sbin/wl`) and the proprietary FullMAC combo
   driver **`dhd.ko`** (`lib/modules/2.6.36.4brcmarm+/kernel/drivers/net/dhd/dhd.ko`,
   an *unstripped* ELF 32-bit ARM relocatable kernel module).
3. `usr/sbin/wl` contains the literal bytes `"HDR0CLM DATA"` — but this is
   just two adjacent strings in the CLI tool's help/usage string table
   (`wl clmload` / `wl clmver` command descriptions), **not** an embedded
   blob. Dead end, correctly ruled out by inspecting the surrounding bytes
   (plain rodata string table, not binary blob content).
4. `dhd.ko`, being unstripped, still carries its full **symbol table**.
   `readelf -sW dhd.ko` inside `.init.data` (`__initdata` — data used only at
   module init/attach, exactly where compiled-in default calibration data
   would live) shows:
   ```
   887: 00000000    48 OBJECT LOCAL DEFAULT 37 chip_image_index_map_table
   888: 00000030    40 OBJECT LOCAL DEFAULT 37 chip_dl_image_array
   889: 00000058 0x90f12 OBJECT LOCAL DEFAULT 37 dlarray_43602a0
   890: 00090f6c 0x83084 OBJECT LOCAL DEFAULT 37 dlarray_43602a1
   ```
   These are the vendor driver's statically-linked "download array" images —
   one per **43602 silicon revision** (`a0`, `a1`) — used by dhd's
   embedded/static-firmware code path. Their **sizes come straight from the
   ELF symbol table**, not from eyeballing hex: `dlarray_43602a1` is
   `0x83084` = 536708 bytes, located at absolute file offset
   `<.init.data section file offset> + <symbol value>` = 191176 + 593772 =
   **784948**, running to **1321656** (which lines up exactly with the start
   of the next section, `.devinit.data` — an independent sanity check that
   the symbol size is exact, not padded/rounded).
   - `dlarray_43602a1` matches this router's actual chip revision
     ("BCM43602/**1**" in dmesg). `dlarray_43602a0` (the A0 revision) was
     *not* used — it's for older 43602 A0-stepping hardware, not this board.
5. Each `dlarray_43602aN` array is `[RAM firmware image][CLM blob]`
   concatenated (confirmed: the array starts with recognizable
   ARM/Thumb-2 opcodes and a "DBPP" firmware-image tag, i.e. executable
   code, not table data). The CLM sub-blob is located by its own literal
   8-byte ASCII magic tag, **`"CLM DATA"`**, found via a byte-offset scan
   (`grep -abo "CLM DATA" dhd.ko`) — occurring **exactly once** inside
   `dlarray_43602a1`, at absolute file offset **1271352** (relative offset
   486404 within the array).
6. **Structure cross-check against known-real CLM source.** Fetched the
   only publicly available *generated* CLM blob C source we could find —
   `RMerl/asuswrt-merlin`'s `wlc_clm_data.c` (a GPL release for a *different*
   Broadcom router, RT-AC88U) — which shows the real struct:
   ```c
   /** BLOB header — Base of all references, contains version information */
   const struct clm_data_header clm_header = {
       CLM_HEADER_TAG,       /* Magic word */
       11, 0,                /* BLOB format version */
       "7.7.5", "1.18.3",    /* CLM version, compiler version */
       &clm_header,          /* Self reference */
       &clm_data,             /* Data registry */
       "ClmImport: 1.17.7",  /* generator version */
       "Broadcom-0.0",        /* SW Apps version */
   };
   ```
   "Base of all references" confirms `clm_header` — i.e. the `CLM_HEADER_TAG`
   magic — sits at **byte 0** of the blob; nothing precedes it. Our carved
   bytes match this layout field-for-field: `CLM DATA` tag, two small-int
   format-version fields, a CLM version string (`"9.10.1"`, vs. the
   reference's `"7.7.5"` — expected, ours is a much newer CLM database), a
   compiler version string (`"1.29.20"`), two 4-byte pointer-sized fields
   (self-reference + data-registry pointer, matching `&clm_header`/`&clm_data`),
   the generator-version string `"ClmImport: 1.36.0"`, and `"Broadcom-0.0"`.
7. **Boundary confirmed by content, not just by ELF symbol size.** The very
   end of `dlarray_43602a1` (i.e. the end of the carved CLM blob) contains a
   readable trailer:
   ```
   43602a1-roml/pcie-ag-splitrx-fdap-mbss-mfp-wl11k-wl11u-txbf-pktctx-amsdutx-ampduretry-proptxstatus-txpwr
   Version: 7.10.274.3.REBASE.R493518 (r799540) CRC: 4d722361
   Date: Wed 2021-06-02 11:11:01 CST  Ucode Ver: 941.301  FWID: 01-a20087dc
   ```
   This is a **chip/ucode compatibility stamp naming this exact chip build
   ("43602a1-...")** — consistent with CLM registry data self-declaring which
   firmware/ucode build it's paired with — not firmware code (no such string
   exists elsewhere in the array; `grep -c "CLM DATA"` inside the carved
   region returns exactly 1, at offset 0, and no other magic tag of any kind
   was found after it). This is strong evidence the entire 486404-byte
   region *before* the tag is firmware, and the entire 50304-byte region
   *from* the tag to the array's end is one coherent, self-contained CLM
   blob — not a chance mid-table false-positive text match.
8. Carved with `dd`: **`brcmfmac43602-pcie.clm_blob`, 50304 bytes**, sha256
   `39d018bc0ad4a38571a135d9822ca780e806511378cfef815a9d3cadf13951a3`.

### Confidence / residual risk

- High confidence this is a real, correctly-bounded CLM blob for the exact
  chip revision running on this router: the boundary is ELF-symbol-exact on
  one end and content-validated (unique magic tag + matching struct layout +
  self-declared chip-build stamp) on the other.
- Not independently verified against a byte-identical reference (none
  exists upstream for this chip — see §2 preamble), and not yet
  hardware-tested.
- **Low risk to try.** `brcmfmac`'s clm_blob path is explicitly optional and
  non-fatal by design (`firmware_request_nowarn`, and the observed dmesg
  line itself is only an info-level "may have limited channels" notice, not
  an error abort). The host driver does not deeply parse the blob — it hands
  the raw bytes to the chip firmware via a CLM download IOVAR, and the *chip
  firmware* is what validates it; a malformed blob is expected to be
  rejected by firmware-side checks rather than cause a hang or damage.
  Recommended test: drop this file at `/lib/firmware/brcm/brcmfmac43602-pcie.clm_blob`,
  reboot, and check `dmesg | grep -i clm` for either successful load or a
  clean rejection (vs. the current "no clm_blob available" absence).

## 3. `txcap_blob` — not extractable from this source, documented

Searched the entire unpacked vendor rootfs (`grep -rlI txcap`, `find -iname
'*txcap*'`, `strings` over `wl` and `dhd` userspace binaries, and the whole
`dlarray_43602a1`/`dlarray_43602a0` byte ranges for any second `"* DATA"`-style
magic tag after the CLM header) — **zero references to txcap anywhere.** The
combo driver in this stock image (`dhd.ko`, built ~2014–2015 vintage judging
by the older `dlarray_43602a0` blob's `ClmImport: 1.24.7` / 2014-09-15 date)
appears to simply predate txcap_blob as a concept; it is a newer addition to
the brcmfmac/CLM ecosystem than this vendor SDK generation. No fallback was
identified in linux-firmware either (no 4360-family chip has one). Per the
kernel driver, txcap_blob absence is non-fatal and independent of clm_blob
(`brcmf_c_process_txcap_blob: no txcap_blob available (err=-2)` is its own
separate, equally optional info message) — shipping the clm_blob alone is
still a real improvement (full CLM channel/power tables) even without it.

## Files in this directory

| file | what | size | sha256 |
|---|---|---|---|
| `brcmfmac43602-pcie.bin` | newest upstream STA firmware (reference only — do not deploy over the current AP build) | 635449 | `bf4cfc23ee...` |
| `brcmfmac43602-pcie.ap.bin` | newest upstream AP firmware — **byte-identical to what's already on the router** | 595472 | `7f735b7254...` |
| `brcmfmac43602-pcie.clm_blob` | carved from stock image, chip BCM43602 rev A1 | 50304 | `39d018bc0a...` |
| `extract.sh` | reproduces all of the above from `images/R8000-V1.0.4.88_10.1.88.chk` + linux-firmware | — | — |

No `brcmfmac43602-pcie.txcap_blob` is produced (see §3).
