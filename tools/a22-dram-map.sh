#!/bin/bash
# Reads the memory map of the A22 from Android (adb + root). READ ONLY: it changes nothing on the phone.
#   ./tools/a22-dram-map.sh                -> logs/a22-dram-map.txt  (send that file)
# The serial of the phone is taken from adb by itself (HAKIOS_SERIAL overrides it).
cd "$(dirname "$0")/.." || exit 1
S=${HAKIOS_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
[ -n "$S" ] || { echo "no phone in Android with adb (adb devices)"; exit 1; }
OUT=${1:-logs/a22-dram-map.txt}
mkdir -p "$(dirname "$OUT")"
A="adb -s $S shell"
r() { $A "su -c '$1'" 2>&1 | tr -d '\r'; }

{
echo "### a22-dram-map $(date -u +%FT%TZ) (no serial stored)"
echo; echo "### getprop"
$A 'getprop ro.product.model; getprop ro.build.version.release; getprop ro.boot.hardware; getprop ro.hardware' | tr -d '\r'
echo; echo "### /proc/meminfo (MemTotal, MemFree)"
r 'head -3 /proc/meminfo'
echo; echo "### /proc/iomem (System RAM and reserved, as the Android kernel has them)"
r 'cat /proc/iomem'
echo; echo "### device tree /memory*: reg"
r 'for d in /proc/device-tree/memory*; do echo $d; od -An -tx1 -v $d/reg; done'
echo; echo "### device tree /reserved-memory/*: name, reg, size, alignment, flags"
r 'for d in /proc/device-tree/reserved-memory/*; do [ -d $d ] || continue; echo "== $(basename $d)"; for p in reg size alignment alloc-ranges alignment no-map reusable compatible; do [ -e $d/$p ] && { echo -n "  $p: "; od -An -tx1 -v $d/$p | tr -d "\n"; echo; }; done; done'
echo; echo "### /proc/device-tree/chosen (bootargs, atag, memory hints)"
r 'ls /proc/device-tree/chosen; cat /proc/cmdline'
echo; echo "### dmesg: memory, reserved, DRAM, ccci, scp, tee, wifi/connsys"
r 'dmesg | grep -i -E "memblock|reserved|dram|memory:|ccci|md1|scp|tee|connsys|ramoops|ion|cma|bootmem|Zone ranges|Early memory node"' | head -150
echo; echo "### /proc/zoneinfo (zone starts)"
r 'grep -E "^Node|spanned|present|start_pfn" /proc/zoneinfo'
echo; echo "### done"
} > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
