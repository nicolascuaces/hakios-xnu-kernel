#!/bin/bash
# Leaves HakiOS "installed" on the Samsung A22 5G: writes into boot a loader that at power-on
# asks (hold Vol- = iOS, nothing = Android), with no time limit by default.
# It does not ask for a boot by pmsg or reboot: after this the phone no longer needs the PC.
# Lives in ~/dev/hakios/tools/. Needs a22-autoboot.py applied, the kernel rebuilt and
# build-payload.bin redone. Variables:
#   XNU_ARGS     iOS boot-args (those of your last good cycle)
#   XNU_LIMIT    seconds until it goes back to Android by itself; 0 = no limit (default 0)
#                (the old Spanish name XNU_LIMITE still works)
#   A22_WINDOW   ms the loader waits for Vol- (default 2500) (the old name A22_VENTANA still works)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="adb ${HAKIOS_SERIAL:+-s $HAKIOS_SERIAL}"
BOOT=${A22_BOOT:-$HOME/a22-backup/boot-magisk.img}
LIMITE=${XNU_LIMIT:-${XNU_LIMITE:-0}}
VENTANA=${A22_WINDOW:-${A22_VENTANA:-2500}}
XNU_ARGS=${XNU_ARGS:?set XNU_ARGS to the boot-args of your last good cycle}
CORES=$(expr "$XNU_ARGS" : '.*cpus=\([0-9]*\)' || echo 1)
[ -n "$CORES" ] || CORES=1
IMG="$ROOT/xnu-a22-installed.img"

for tool in adb python3 shasum; do
	command -v $tool >/dev/null || { echo "$tool is missing from the PATH"; exit 1; }
done
[ -f "$BOOT" ] || { echo "$BOOT does not exist"; exit 1; }
[ -f "$ROOT/mtkloader/mtka22boot.h" ] || { echo "a22-autoboot.py has not been applied"; exit 1; }
[ -f "$ROOT/build-payload.bin" ] || { echo "there is no build-payload.bin: HAKIOS_SERIAL=a22 ./xl_x6826b build"; exit 1; }
[ "$($A get-state 2>/dev/null)" = device ] || { echo "the A22 is not in Android with adb"; exit 1; }

"$ROOT/mtkloader/build.sh" -DA22_KEY_BOOT=1 -DA22_KEY_WINDOW_MS=$VENTANA -DXNU_WDT_SECONDS=0 \
	-DXNU_CORES=$CORES -DXNU_WATCH_SECONDS=0 "-DXNU_RETURN_AFTER=\"$LIMITE\"" \
	"-DXNU_EXTRA_ARGS=\"$XNU_ARGS\"" "$@" >/dev/null
python3 "$ROOT/tools/chainboot.py" "$BOOT" "$ROOT/mtkloader/mtkloader.bin" "$IMG" "$ROOT/build-payload.bin"

$A push "$IMG" /data/local/tmp/xnu-a22-installed.img >/dev/null
$A shell "su -c 'dd if=/data/local/tmp/xnu-a22-installed.img of=/dev/block/by-name/boot bs=1048576 conv=fsync'" 2>&1 | tail -1
L=$(stat -f %z "$IMG")
H1=$(shasum -a 256 "$IMG" | cut -d' ' -f1)
H2=$($A shell "su -c 'head -c $L /dev/block/by-name/boot | sha256sum'" | cut -d' ' -f1)
[ "$H1" = "$H2" ] || { echo "BOOT DOES NOT MATCH ($H1 / $H2): restore boot-magisk before rebooting"; exit 1; }
echo "boot written and verified ($L bytes, limit ${LIMITE}s; 0 = no limit)"
echo "Done. At power-on: hold Vol- when 'Hold VOL- = iOS' appears. Let go = Android."
echo "To go back to Android from iOS: Vol+ and Vol- together for 3 s, or 5 fingers on the screen for 1.5 s."
