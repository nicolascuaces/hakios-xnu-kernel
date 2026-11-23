#!/bin/bash
# One run of XNU on the Samsung A22 5G (without fastboot): builds mtkloader, packs it with the
# Android kernel (your boot with Magisk) and the XNU payload, writes it to "boot" with dd from
# Android (root), checks the hash, asks for ONE XNU boot by pmsg and reboots. When Android
# comes back, it saves the ramoops log. Lives in ~/dev/hakios/tools/. Extra arguments -> compiler
# (e.g. -DXNU_CHECKPOINT=5).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="adb ${HAKIOS_SERIAL:+-s $HAKIOS_SERIAL}"
BOOT=${A22_BOOT:-$HOME/a22-backup/boot-magisk.img}
OUT=${XNU_LOG_OUT:-$ROOT/logs/xnu-last.log}
SECS=${XNU_SECS:-45}
XNU_ARGS=${XNU_ARGS:-cpus=1}
CORES=${XNU_CORES:-$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)}
[ -n "$CORES" ] || CORES=1
# watchdog off at the start: its debug window (0x0e010000) belongs to the MT6768
WATCH=${XNU_WATCH:-0}
IMG="$ROOT/xnu-a22.img"

for tool in adb python3 shasum; do
	command -v $tool >/dev/null || { echo "$tool is missing from the PATH"; exit 1; }
done
[ -f "$BOOT" ] || { echo "$BOOT does not exist"; exit 1; }
[ -f "$ROOT/build-payload.bin" ] || { echo "there is no build-payload.bin: HAKIOS_SERIAL=a22 ./xl_x6826b build"; exit 1; }
[ "$($A get-state 2>/dev/null)" = device ] || { echo "the A22 is not in Android with adb"; exit 1; }
mkdir -p "$(dirname "$OUT")"

"$ROOT/mtkloader/build.sh" -DXNU_WDT_SECONDS=0 -DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=$WATCH \
	"-DXNU_RETURN_AFTER=\"$SECS\"" "-DXNU_EXTRA_ARGS=\"$XNU_ARGS epoch=$(date +%s)\"" "$@" >/dev/null
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$ROOT/mtkloader/mtkloader.bin" "$IMG" "$ROOT/build-payload.bin"

$A push "$IMG" /data/local/tmp/xnu-a22.img >/dev/null
$A shell "su -c 'dd if=/data/local/tmp/xnu-a22.img of=/dev/block/by-name/boot bs=1048576 conv=fsync'" 2>&1 | tail -1
L=$(stat -f %z "$IMG")
H1=$(shasum -a 256 "$IMG" | cut -d' ' -f1)
H2=$($A shell "su -c 'head -c $L /dev/block/by-name/boot | sha256sum'" | cut -d' ' -f1)
[ "$H1" = "$H2" ] || { echo "BOOT DOES NOT MATCH ($H1 / $H2): restore boot-magisk before rebooting"; exit 1; }
echo "boot written and verified"

# The request goes in ramoops' pmsg zone (64 KB, circular). Android writes its own log there and,
# with a lot of activity, overwrites it before the reboot: hence several copies and an immediate reboot
# (sysrq) right after, without Android's shutdown, after a sync.
$A shell "su -c 'sync; for i in 1 2 3 4 5 6 7 8; do printf DARWIN-REDWOOD:BOOT-XNU > /dev/pmsg0; done; sync; echo b > /proc/sysrq-trigger'" >/dev/null 2>&1 &
sleep 5
echo "XNU running (~$SECS s); Android comes back by itself. If it hangs: Power + Vol- until it reboots."
for i in $(seq 1 15); do [ "$($A get-state 2>/dev/null)" = device ] || break; sleep 2; done
for i in $(seq 1 $(( (SECS + 300) / 3 ))); do [ "$($A get-state 2>/dev/null)" = device ] && break; sleep 3; done
[ "$($A get-state 2>/dev/null)" = device ] || { echo "Android did not come back; when it does: cat /sys/fs/pstore/console-ramoops*"; exit 1; }
sleep 5
: > "$OUT"
# pstore can take a while to appear after Android boots: up to ~60 s of retries
for attempt in $(seq 1 12); do
	for f in $($A shell "su -c 'ls /sys/fs/pstore/'" | tr -d '\r' | grep '^console-ramoops'); do
		$A shell "su -c 'cat /sys/fs/pstore/$f'" > "$OUT.part" 2>/dev/null || true
		if grep -aq -e mtkloader -e Darwin -e XNU -e "^boot: " "$OUT.part"; then { echo "=== $f ==="; cat "$OUT.part"; } >> "$OUT"; fi
	done
	[ -s "$OUT" ] && break
	sleep 5
done
rm -f "$OUT.part"
$A shell "su -c 'cat /sys/fs/pstore/pmsg-ramoops-0'" > "$OUT.keep" 2>/dev/null || true
if head -c 64 "$OUT.keep" | grep -q 'kept for the host by the kernel'; then { echo; cat "$OUT.keep"; } >> "$OUT"; fi
rm -f "$OUT.keep"
echo "log: $OUT ($(wc -c < "$OUT") bytes)"
grep -aq "^boot: Android" "$OUT" && echo "WARNING: the loader did not see the XNU request in pmsg and Android booted"
true
