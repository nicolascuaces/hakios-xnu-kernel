#!/bin/bash
# Himax touch controller probe on the A22 (read-only on the chip): builds mtkloader with -DHIMAX_PROBE=1,
# puts it in front of your Android kernel (no XNU payload), writes it to "boot" with dd from
# Android (root), checks the hash, asks for ONE run by pmsg and reboots. When Android is back,
# it prints what the chip said. Lives in ~/dev/hakios/tools/. Before: python3 himax-probe.py ~/dev/hakios apply
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="adb ${HAKIOS_SERIAL:+-s $HAKIOS_SERIAL}"
BOOT=${A22_BOOT:-$HOME/a22-backup/boot-magisk.img}
OUT=${HIMAX_LOG_OUT:-$ROOT/logs/himax-probe.log}
IMG="$ROOT/himax-a22.img"

for tool in adb python3 shasum; do
	command -v $tool >/dev/null || { echo "$tool is missing from the PATH"; exit 1; }
done
[ -f "$BOOT" ] || { echo "$BOOT does not exist"; exit 1; }
[ -f "$ROOT/mtkloader/mtkhimax.h" ] || { echo "mtkloader/mtkhimax.h is missing: python3 himax-probe.py $ROOT apply"; exit 1; }
[ "$($A get-state 2>/dev/null)" = device ] || { echo "the A22 is not in Android with adb"; exit 1; }
mkdir -p "$(dirname "$OUT")"

# HIMAX_FW=1: also loads the chip's firmware (your phone's) and reads fingers for 40 s
FWFLAG=""
if [ -n "$HIMAX_FW" ]; then
	FWBIN=${HIMAX_FW_BIN:-$HOME/a22-backup/Himax_firmware_fw_jdi.bin}
	[ -f "$FWBIN" ] || { echo "$FWBIN does not exist"; exit 1; }
	python3 "$ROOT/mkhimaxfw.py" "$FWBIN" > "$ROOT/mtkloader/himaxfw.h"
	FWFLAG="-DHIMAX_FW=1"
fi
"$ROOT/mtkloader/build.sh" -DHIMAX_PROBE=1 $FWFLAG "$@" >/dev/null
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$ROOT/mtkloader/mtkloader.bin" "$IMG"

$A push "$IMG" /data/local/tmp/himax-a22.img >/dev/null
$A shell "su -c 'dd if=/data/local/tmp/himax-a22.img of=/dev/block/by-name/boot bs=1048576 conv=fsync'" 2>&1 | tail -1
L=$(stat -f %z "$IMG")
H1=$(shasum -a 256 "$IMG" | cut -d' ' -f1)
H2=$($A shell "su -c 'head -c $L /dev/block/by-name/boot | sha256sum'" | cut -d' ' -f1)
[ "$H1" = "$H2" ] || { echo "BOOT DOES NOT MATCH ($H1 / $H2): restore boot-magisk before rebooting"; exit 1; }
echo "boot written and verified"

# Request in pmsg (64 KB circular zone): several copies and an immediate reboot, as in xnu-cycle-a22.sh
$A shell "su -c 'sync; for i in 1 2 3 4 5 6 7 8; do printf DARWIN-REDWOOD:PROBE-HIMAX > /dev/pmsg0; done; sync; echo b > /proc/sysrq-trigger'" >/dev/null 2>&1 &
sleep 5
echo "testing the touch controller (~15 s; with HIMAX_FW=1 ~60 s: touch the screen when it says so); Android comes back by itself. If it hangs: Power + Vol- until it reboots."
for i in $(seq 1 15); do [ "$($A get-state 2>/dev/null)" = device ] || break; sleep 2; done
for i in $(seq 1 100); do [ "$($A get-state 2>/dev/null)" = device ] && break; sleep 3; done
[ "$($A get-state 2>/dev/null)" = device ] || { echo "Android did not come back; when it does: cat /sys/fs/pstore/console-ramoops*"; exit 1; }
sleep 8
: > "$OUT"
for f in $($A shell "su -c 'ls /sys/fs/pstore/'" | tr -d '\r' | grep '^console-ramoops'); do
	$A shell "su -c 'cat /sys/fs/pstore/$f'" > "$OUT.part" 2>/dev/null || true
	if grep -aq "Himax" "$OUT.part"; then { echo "=== $f ==="; cat "$OUT.part"; } >> "$OUT"; fi
done
rm -f "$OUT.part"
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
sed -n '/Himax touch probe/,/starting Android/p' "$OUT" | tr -d '\000' | head -120
grep -aq "Himax" "$OUT" || echo "WARNING: the probe did not appear in the log; Android booted without seeing the request (repeat the command)"
echo
echo "Your boot is now the probe's (Android boots normally). To go back to yours:"
echo "  adb push $BOOT /data/local/tmp/b.img && adb shell \"su -c 'dd if=/data/local/tmp/b.img of=/dev/block/by-name/boot bs=1048576 conv=fsync'\""
true
