# Deprecated — not the build source

This directory was the ImageBuilder `FILES=` overlay through v6. Since
commit `1e2baf2` ("Ship v7: consolidated FILES overlay actually built and
flashed"), **`v2-files/` is the real overlay** — every image from v7 onward
(v7, v8, v9, ...) was built from `v2-files/`, not this directory.

This tree lacks guest-network isolation, `perf-tune`, the `radio-watchdog`
rc.d enable symlink, the LED netdev-binding uci-defaults script, and
`board.d/01_leds`. Building an image with `FILES=image-files` today would
silently regress all of those.

Kept only as historical reference for what v1-v6 actually shipped. See
`docs/RUNBOOK.md` §5 for the current, correct build recipe.
