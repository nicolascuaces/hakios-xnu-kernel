#!/bin/bash
# Reads how the A22's screen backlight works, from Android (adb + root). READ ONLY: changes nothing.
#   ./tools/a22-backlight-map.sh            -> logs/a22-backlight-map.txt  (send that file)
# Run it with the screen ON and at a middle brightness; the script also writes down what Android has now.
cd "$(dirname "$0")/.." || exit 1
S=${HAKIOS_SERIAL:-$(adb devices | awk 'NR>1 && $2=="device"{print $1; exit}')}
[ -n "$S" ] || { echo "no phone in Android with adb (adb devices)"; exit 1; }
OUT=${1:-logs/a22-backlight-map.txt}
mkdir -p "$(dirname "$OUT")"
A="adb -s $S shell"
r() { $A "su -c '$1'" 2>&1 | tr -d '\r'; }
{
echo "### a22-backlight-map $(date -u +%FT%TZ) (no serial stored)"
echo; echo "### /sys/class/leds and backlight (name, brightness, max)"
r 'for d in /sys/class/leds/* /sys/class/backlight/*; do [ -e $d ] || continue; echo "$d: $(cat $d/brightness 2>/dev/null) / $(cat $d/max_brightness 2>/dev/null)"; done'
echo; echo "### device tree: nodes that mention backlight, pwm, led, bl, panel"
r 'find /proc/device-tree -maxdepth 3 \( -iname "*backlight*" -o -iname "*pwm*" -o -iname "*led*" -o -iname "*panel*" -o -iname "*dsi*" -o -iname "*disp*" \) 2>/dev/null | head -60'
echo; echo "### compatible of those nodes"
r 'for d in $(find /proc/device-tree -maxdepth 3 \( -iname "*backlight*" -o -iname "*pwm*" -o -iname "*led*" \) -type d 2>/dev/null | head -30); do echo "== $d"; for p in compatible reg status; do [ -e $d/$p ] && { echo -n "  $p: "; tr "\0" " " < $d/$p | head -c 200; echo; }; done; done'
echo; echo "### dmesg: backlight, pwm, led, disp, mt6360/mt6359 led, dsi, panel"
r 'dmesg | grep -i -E "backlight|disp_pwm|pwm|led_|mt6360|mt6359|bl_|brightness|dsi|panel" | head -120'
echo; echo "### DISP_PWM registers (0x1100e000 on MT6833; 0x14014000 = DISP_PWM0 on newer): first words"
r 'for a in 0x1100e000 0x14014000; do echo "-- $a"; (cat /proc/iomem | grep -i -E "pwm|$(printf %x $((a)))" ) 2>/dev/null; done'
r 'grep -i -E "pwm|disp|dsi|led|backlight" /proc/iomem'
echo; echo "### GPIO of the panel/backlight (debugfs pinctrl, if present)"
r 'ls /sys/kernel/debug/pinctrl 2>/dev/null; cat /sys/kernel/debug/pinctrl/*/pinmux-pins 2>/dev/null | grep -i -E "pwm|lcm|dsi|led|bl" | head -40'
echo; echo "### regulators related to the panel (vsp/vsn, lcm)"
r 'for d in /sys/class/regulator/*; do n=$(cat $d/name 2>/dev/null); case "$n" in *vsp*|*vsn*|*lcm*|*led*|*bl*|*VLED*) echo "$n: $(cat $d/state 2>/dev/null) $(cat $d/microvolts 2>/dev/null)";; esac; done'
echo; echo "### dumpsys display (brightness values)"
$A 'dumpsys display | grep -i -E "brightness|backlight" | head -20' | tr -d '\r'
echo; echo "### done"
} > "$OUT"
echo "wrote $OUT ($(wc -l < "$OUT") lines)"
