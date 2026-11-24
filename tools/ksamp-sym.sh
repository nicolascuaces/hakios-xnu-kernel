#!/bin/bash
# Symbolizes the "ksamp:" lines of a HakiOS log with the kernel binary.
#   ./tools/ksamp-sym.sh logs/ios-ksamp.log [kernel-binary]
# Addresses are already unslid, so they match `nm` of the kernel file directly.
LOG="$1"
KERN="${2:-xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837}"
[ -f "$LOG" ] || { sed -n 2,5p "$0"; exit 1; }
grep -E 'ksamp: (total|sampling)' "$LOG"
for kind in pc lr; do
  echo "== top $kind =="
  grep -a "ksamp: $kind " "$LOG" | sed 's/.*ksamp: //' | while read -r _ n addr; do
    s=$(atos -o "$KERN" -arch arm64 "$addr" 2>/dev/null | head -1)
    printf '%6s  %s  %s\n' "$n" "$addr" "$s"
  done
done
