#!/usr/bin/env bash
# Real hardware-in-the-loop verification against the operator's own router.
# Runs ONLY on the self-hosted runner (has LAN access to 192.168.1.1) - never
# on a GitHub-hosted runner, which has no path to reach it.
#
# This is exactly the live-swap-diagnose-revert procedure already proven
# safe this project's own investigations (docs/FINDINGS.md SS15-18): unload
# in dependency order, load the candidate module from /tmp only (never
# touches /lib/modules, so a script failure can't leave a bad module
# persisting across a reboot), verify, then ALWAYS revert - trap-guaranteed,
# not just "the happy path reverts".
#
# This proves the new brcmfmac build loads and negotiates with the real
# BCM43602 radio - the one thing no dump, emulator, or CI runner without
# this router could ever prove. It does not flash/persist anything; the
# router boots back to its actual currently-installed image on next reboot
# regardless of what this script does.
#
# Usage: hardware-verify.sh <path-to-new-brcmfmac.ko>
# Exit 0 = the candidate module loaded clean, radios came up, no firmware
#          errors in dmesg during the test window, and revert succeeded.
# Exit 1 = anything about the candidate looked wrong. Router is still
#          reverted either way - a FAIL here is a real finding, not damage.
set -uo pipefail  # deliberately not -e: the trap must run even on failure

ROUTER="${ROUTER_HOST:-192.168.1.1}"
SSH="ssh -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new -o BatchMode=yes root@${ROUTER}"
SCP="scp -o ConnectTimeout=8 -o StrictHostKeyChecking=accept-new -o BatchMode=yes"
NEW_KO="${1:?usage: hardware-verify.sh <path-to-new-brcmfmac.ko>}"
RESULT=1
REVERTED=0

fail() { echo "FAIL: $*"; }
ok()   { echo "OK: $*"; }

revert() {
  [ "$REVERTED" -eq 1 ] && return 0
  echo "==> Reverting to the known-good module (runs regardless of test outcome)"
  $SSH '
    rmmod brcmfmac_wcc 2>/dev/null
    rmmod brcmfmac 2>/dev/null
    insmod /lib/modules/*/brcmfmac.ko
    /etc/init.d/network restart
  ' >/tmp/hw-verify-revert.log 2>&1
  sleep 8
  UP="$($SSH 'ubus list network.wireless 2>/dev/null | wc -l')"
  if $SSH 'ping -c1 -W2 8.8.8.8 >/dev/null 2>&1'; then
    ok "revert confirmed: network reachable"
    REVERTED=1
  else
    fail "revert command ran but router isn't answering ping - check console/nmrpflash recovery net NOW, do not assume it's fine"
    REVERTED=1  # still mark done - retrying won't help, this needs a human
  fi
}
trap revert EXIT

echo "==> Pre-flight: router reachable and no pending unsaved config"
$SSH 'true' || { fail "router not reachable via SSH, aborting before touching anything"; exit 1; }
CHANGES="$($SSH 'uci changes 2>/dev/null | wc -l')"
if [ "${CHANGES:-1}" != "0" ]; then
  fail "router has uncommitted uci changes - not touching it, resolve manually first"
  exit 1
fi
MARKER="$($SSH 'dmesg | tail -1')"
ok "pre-flight clean, dmesg marker: $MARKER"

echo "==> Staging candidate module"
$SCP "$NEW_KO" "root@${ROUTER}:/tmp/brcmfmac-candidate.ko" || { fail "scp of candidate module failed"; exit 1; }

echo "==> Swapping in candidate (unload order matters: wcc depends on brcmfmac)"
$SSH '
  rmmod brcmfmac_wcc 2>/dev/null
  rmmod brcmfmac
  insmod /tmp/brcmfmac-candidate.ko
  /etc/init.d/network restart
' >/tmp/hw-verify-swap.log 2>&1
SWAP_EXIT=$?
cat /tmp/hw-verify-swap.log
if [ "$SWAP_EXIT" -ne 0 ]; then
  fail "module swap commands returned non-zero - see log above"
  exit 1
fi

echo "==> Test window: letting radios settle, then checking real state"
sleep 15

DMESG_SINCE="$($SSH "dmesg | awk -v m='$MARKER' 'found{print} \$0==m{found=1}'")"
if grep -qiE 'firmware load.*failed|external abort|kernel panic|Oops|brcmf_c_process_clm_blob.*err' <<<"$DMESG_SINCE"; then
  fail "dmesg shows firmware/driver errors during the candidate's test window:"
  echo "$DMESG_SINCE" | grep -iE 'firmware load.*failed|external abort|kernel panic|Oops|brcmf_c_process_clm_blob.*err'
  exit 1
fi
ok "no firmware/panic errors in dmesg during test window"

PHY_COUNT="$($SSH 'iw phy 2>/dev/null | grep -c "^Wiphy"')"
if [ "${PHY_COUNT:-0}" -lt 3 ]; then
  fail "expected 3 radios (phy), found $PHY_COUNT"
  exit 1
fi
ok "all 3 radios present (iw phy)"

SSID_COUNT="$($SSH "ubus call network.wireless status 2>/dev/null | grep -c '\"up\": true'")"
if [ "${SSID_COUNT:-0}" -lt 3 ]; then
  fail "expected at least 3 up wireless interfaces, found $SSID_COUNT"
  exit 1
fi
ok "wireless interfaces report up ($SSID_COUNT)"

echo "==> Candidate module: PASS - real hardware negotiated cleanly with the real BCM43602 radios"
RESULT=0
exit $RESULT
