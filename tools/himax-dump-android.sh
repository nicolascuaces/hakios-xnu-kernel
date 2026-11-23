#!/bin/bash
# Reads (read-only) the Himax touch controller's registers while Android has it working, using
# the debug node of Samsung's driver (/proc/android_touch/debug, command "register").
# Useful to compare them with the ones mtkloader leaves. Lives in ~/dev/hakios/tools/.
# Before: the A22 in normal Android, with root (Magisk) and adb.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
A="adb ${HAKIOS_SERIAL:+-s $HAKIOS_SERIAL}"
OUT=${HIMAX_REGS_OUT:-$ROOT/logs/himax-android-regs.txt}
mkdir -p "$(dirname "$OUT")"
[ "$($A get-state 2>/dev/null)" = device ] || { echo "the A22 is not in Android with adb"; exit 1; }

$A shell "su -c 'ls /proc/android_touch/debug'" >/dev/null 2>&1 || { echo "/proc/android_touch/debug does not exist"; exit 1; }

$A shell "su -c '
D=/proc/android_touch/debug
read_reg() { echo \"register,r:x\$1\" > \$D; echo \"== \$1\"; cat \$D; }
for a in 10006A00 10006A80 10006B00 10006B80 10006C00 10006C80 10006D00 10006D80 \
         10006E00 10006E80 10006F00 10006F80 10007000 10007080 10007100 10007180 \
         10007200 10007280 10007300 10007380 10007400 10007480 10007500 10007580 \
         10007600 10007680 10007700 10007780 10007800 10007880 10007900 10007980 \
         10007A00 10007A80 10007B00 10007B80 10007C00 10007C80 10007D00 10007D80 \
         10007E00 10007E80 10007F00 10007F80 \
         90000000 90000080 80020000 80020080 80020480 800204B4; do
	read_reg \$a
done
'" | tr -d '\r' > "$OUT"
echo "done: $OUT ($(wc -l < "$OUT") lines)"
grep -A2 "== 10007280" "$OUT" | head -4
