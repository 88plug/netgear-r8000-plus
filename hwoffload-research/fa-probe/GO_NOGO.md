# GO/NO-GO: loading `fa_probe.ko` on the live R8000

> **Resolved 2026-07-23 — this gate is closed, not an open decision.**
> `fa_probe.ko` loaded per the procedure below, and the investigation
> continued through GMAC bring-up, switch-side enable, real-traffic
> testing, and root-cause closure. See `docs/WINS.md`'s "FA/CTF hardware
> accelerator" section for the final narrative. Investigation closed; the
> historical record below is left as written at the time.

## Verdict: **GO, WITH TWO CHEAP ZERO-RISK PRECONDITIONS FIRST**

Load `fa_probe.ko` on the bench router. Risk of the MMIO read itself is
assessed **LOW** (not zero) for a hard fault. Do the two preconditions below
first — both cost under a minute, require no module load, and either add
useful pre-probe context or confirm the safety net is actually in place.

---

## What this verdict is built on

**Available at synthesis time:**

| Input | Status | Weight given |
|---|---|---|
| `fa_probe.c` + `Makefile` (the actual code that would be loaded) | Landed, read in full | **Highest** — the module's own header comment *is* a rigorous external-abort risk assessment (see below), grounded in the real code, not a generic estimate |
| `VERDICT.md` (prior synthesis: register map, DTS static analysis, blob-analysis) | Landed, read in full | High — this is where the physical address (`0x18027c00`) and the devicetree reasoning come from |
| `graveyard-vendor/extracted-source/etc_fa.c` + `notes.md` (WAR777 erratum) | Landed, checked directly | Medium — relevant to a *later* phase, not this probe (see below) |
| `graveyard-openwrt/notes.md`, `v2-staging/extras/dsa-switch/NOTES.md` (via `WAVE2_INDEX.md`, a **separate, concurrently-running research wave** — explicitly disclaimed by its own index as non-overlapping with this fa-probe track) | Landed, read in full | Low-medium — confirms no upstream driver exists anywhere, and independently confirms the live switch chip is `BCM53012 rev 5` on this bench unit |

**Not found as separate deliverables**, despite checking repeatedly over
several minutes past the specified wait window:
- A live SSH re-confirmation of `gmac3`'s devicetree `status` property on the
  *currently-booted* kernel (as opposed to the static `.dts`/`.dtsi` source
  analysis already in `VERDICT.md`).
- A live `dmesg` / `bcma` bus-enumeration transcript from the running router.
- A standalone "agent 7" ARM external-abort risk-assessment file, distinct
  from the one embedded in `fa_probe.c` itself.
- A `b53_srab` debugfs/sysfs alternate-access-path check.

None of these gaps changes the verdict below, but they're the reason this is
GO-with-preconditions rather than an unconditional GO — see Precondition 1.

---

## Why LOW risk, not HIGH

`fa_probe.c`'s own header comment (lines 37–114) already does the ARM
external-abort analysis this task asked to weight heavily. Summary, with my
agreement noted:

Two structurally different failure modes for an MMIO read on this platform:

1. **Genuinely unmapped/undecoded physical address** — the SoC's on-chip
   interconnect has no slave port there at all. The ARM AXI-to-CPU bridge
   turns this into a synchronous external abort, which is normally **fatal**
   in kernel context on mainline ARM32 Linux (no page-fault-style recovery
   for a real bus error). This is the outcome that would produce a kernel
   oops/panic on `insmod`.
2. **Real, backplane-decoded core wrapper, inner logic clock-gated/held-in-
   reset/absent** — the wrapper keeps answering backplane transactions (it's
   a distinct, always-present piece of interconnect glue from the core's
   *internal* register logic) and most commonly returns a fixed pattern
   (`0xFFFFFFFF` or `0x00000000`) rather than aborting.

**The deciding fact**: `FA_PHYS_BASE` (`0x18027c00`) sits *inside* GMAC-3's
already-decoded, already-populated 4KB backplane aperture (`0x18027000` per
mainline `bcm-ns.dtsi`) — not in some unrelated gap of the physical address
map. That aperture's backplane wrapper is proven live on this exact board,
because it's the same wrapper answering GMAC-3's own known-working register
traffic today. That places this read in category (2), not (1) — clean
readback expected, hard fault judged unlikely but explicitly **not ruled
out** (a segmentation-fused SKU could in principle omit the backplane slave
port entirely for a disabled feature, which would flip this back to category
1). I agree with this analysis; it's the correct architectural argument and
it's the same reasoning `VERDICT.md`'s DTS analysis supports independently
(`gmac3`'s DT `reg` only covers the first 2KB of the 4KB window, so offset
`0xc00` — where FA lives — is outside whatever `bgmac` itself reserves: no
`request_mem_region()` conflict either).

The module itself minimizes what's left to go wrong even within category 2:
read-only (no writes anywhere), maps only the needed `0x100`-byte window,
unmaps immediately after the single read pair, and never touches GMAC-3's
own `0x18027000–0x180277ff` register range that `bgmac` may have reserved.

**WAR777 erratum — checked, not a blocker for this probe.** It's
real (`CTF_FA_WAR777_ON/OFF` in `etc_fa.c`, documented for BCM4707 rev 2:
force `HWQ_THRESHLD` to 0 before indirect table access, restore after, 1ms
delay) — but it only guards the **indirect table-access path**
(`fa_napt_add/del/live`, `mem_acc_ctl` writes), which `fa_probe.c` never
touches. It matters for a later bring-up phase, not this read-only probe. Two
open items to resolve *before* that later phase, not before this one: (a)
R8000 uses BCM4709, the erratum is documented for BCM4707 — same family,
unconfirmed whether the rev-2 quirk (or another) applies to R8000's actual
die/stepping; (b) that stepping itself hasn't been confirmed live in this
synthesis window.

---

## Precondition 1 (do first, ~1 minute, zero risk, no module load)

SSH to the router (it is currently running a custom OpenWrt build per
`images/openwrt-25.12.5-r8000plus-v6-...chk` — SSH/LuCI access should already
be live) and run:

```sh
# Devicetree status of gmac2/gmac3 as actually compiled into the booted .dtb
for n in /sys/firmware/devicetree/base/axi@18000000/gmac@*; do
  echo "== $n =="; cat "$n/status" 2>/dev/null || echo "(no status property = enabled)"
done

# Anything the kernel logged about bcma/bgmac core enumeration at boot
dmesg | grep -iE 'bcma|bgmac|gmac|siliconbackplane'
```

This repeats what agents 1/6 were tasked to gather live but that did not
land as a file in this synthesis window. It's worth the minute because it's
free: it either matches `VERDICT.md`'s static-source finding (gmac3 present,
default-enabled, no `status=disabled`) — in which case proceed with full
confidence — or it surfaces something `VERDICT.md`'s DTS-source-only analysis
couldn't have caught (e.g. a live probe failure message near this address
already logged by `bgmac`, which would be a reason to stop and investigate
further before loading a new module at a neighboring offset).

## Precondition 2 (confirm before insmod, not new work — already required by project CLAUDE.md)

Confirm the recovery net described in `/home/andrew/netgearr8000/CLAUDE.md`
is actually staged and reachable right now: `nmrpflash` installed and able to
see the dongle interface, known-good stock/OpenWrt `.chk` on hand. A kernel
panic on `insmod` is architecturally the same class of "board stops responding,
needs an out-of-band recovery path" event as a bad flash — treat it with the
same precaution.

---

## Exact procedure (once both preconditions are satisfied)

1. **Build** (cross-compile, not on the router):
   ```sh
   cd /home/andrew/netgearr8000/hwoffload-research/fa-probe
   make
   ```
   Uses the same SDK/toolchain tree already used for `kmod-brcmfmac` in this
   session. Produces `fa_probe.ko`, nothing is installed or loaded yet.

2. **Copy to the router** and note the current dmesg tail position so the
   probe's output is easy to isolate:
   ```sh
   scp fa_probe.ko root@192.168.1.1:/tmp/
   ssh root@192.168.1.1 'dmesg | tail -1'   # note this line as your marker
   ```

3. **Load it**:
   ```sh
   ssh root@192.168.1.1 'insmod /tmp/fa_probe.ko; echo "insmod exit=$?"'
   ```
   If the SSH session hangs, drops, or the router stops responding to ping
   entirely after this command: **do not power-cycle blindly assuming it'll
   recover.** That is the category-1 external-abort/panic outcome the risk
   assessment flagged as low-probability-but-real. Go straight to the
   nmrpflash recovery path from `CLAUDE.md`.

4. **Read the result**:
   ```sh
   ssh root@192.168.1.1 'dmesg | grep fa_probe'
   ```

### Interpreting the output

| dmesg shows | Meaning | Next step |
|---|---|---|
| `control=0xffffffff status=0xffffffff` | Category-2 clean readback, consistent with FA block present-but-unclocked/absent on this SKU (most-likely outcome per the risk assessment above) | Converts "unverified" → "provisionally absent on this SKU," matching `VERDICT.md` §3's fallback plan. Does not distinguish "fused off" from "clocked off, gateable" — that needs the NVRAM/clock-gating angle, a separate follow-on, not urgent. |
| `control=0x00000000 status=0x00000000` | Held-in-reset or uninitialized-but-present | Practically the same conclusion as above — do not proceed to the `fa_up()` bring-up/indirect-table-write sequence based on this alone. |
| Non-trivial, non-repeating values | Possible sign FA is live | **Do not** jump straight to `fa_up()`/table writes from this one read (the module's own comment says the same). Before that phase: confirm R8000's actual BCM4709 die revision and whether WAR777 (or another undocumented erratum) applies — that's a new, separate go/no-go, not covered by this one. |
| Nothing in dmesg / SSH session dead / router unresponsive | Likely category-1 external abort → kernel panic on load | Recover via `nmrpflash` + stock `.chk` per `CLAUDE.md`. This result is itself useful data: it means `0x18027c00` is *not* backed by a live backplane slave, which is a different (and equally definitive) way of confirming FA is absent/inaccessible here — but confirm it once, don't retry the same load again without changing something. |

5. **Unload.** The module already unmaps its own mapping and returns
   immediately after the one-time read in `init()` — there's nothing held
   open. `rmmod fa_probe` is bookkeeping only (logs "module unloaded"), no
   unwind risk:
   ```sh
   ssh root@192.168.1.1 'rmmod fa_probe'
   ```

---

## What would change this to NO-GO

- Precondition 1 surfacing that `gmac3` (or the `axi@18000000` bus segment it
  sits on) is explicitly `status = "disabled"` in the *live, booted* devicetree
  (not just the static source) — that would suggest the backplane wrapper
  itself may not be routed/decoded at all, shifting the risk toward category 1
  and warranting a re-think before loading anything.
- Precondition 2 finding the recovery net is *not* actually staged (nmrpflash
  not installed, no known-good `.chk` reachable) — fix that first regardless
  of FA risk; it's a hard requirement from project `CLAUDE.md` for any
  operation with panic/brick potential.
- Discovery that the router in its current boot has any *other* pending
  changes/unsaved config that a panic-triggered reboot would lose — trivial
  to avoid by just checking `uci changes` / saving state before running step 3.

None of these apply based on what's known now; they're listed so the
precondition checks have a clear stop criterion if they surface something
unexpected.
