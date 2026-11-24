#!/usr/bin/env python3
"""
a22-goodixoff.py -- the POCO's Goodix touch driver must run only with touchscreen=1 (as its header says), not with any
non-zero value. With touchscreen=3 (the A22's Himax) it still started, read the Qualcomm RPMh database at physical
0x80860000 and mapped that page as device memory. With dram4 (iOS memory from 0x80000000) that page is RAM, and the
kernel panicked later: "pmap_assert_free: Found at least one mapping to 0x80860000" (ios-dram4.log).

  python3 tools/a22-goodixoff.py ~/dev/hakios            # review only
  python3 tools/a22-goodixoff.py ~/dev/hakios apply      # apply (backup redwood_goodix.c.goodixoff.orig)
"""
import os, shutil, sys
P = "xnu-src/xnu-8796.121.2/bsd/dev/redwood_goodix.c"
OLD = '\tif (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || !enabled) {\n\t\treturn;\n\t}\n\tPE_parse_boot_argn("touch_flip"'
NEW = '\tif (!PE_parse_boot_argn("touchscreen", &enabled, sizeof(enabled)) || enabled != 1) {   /* A22: 3 is the Himax\'s, not this one */\n\t\treturn;\n\t}\n\tPE_parse_boot_argn("touch_flip"'
def main():
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    root = os.path.expanduser(sys.argv[1]); do = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    p = os.path.join(root, P); t = open(p, encoding="utf-8").read()
    if NEW in t: print("was there  |", P); return
    if t.count(OLD) != 1: raise SystemExit("NOT FOUND (%d times): the touchscreen test in redwood_goodix_init" % t.count(OLD))
    print("changes    |", P)
    if not do: print("\nTo apply it: python3 tools/a22-goodixoff.py %s apply" % sys.argv[1]); return
    shutil.copy(p, p + ".goodixoff.orig"); open(p, "w", encoding="utf-8").write(t.replace(OLD, NEW)); print("applied")
main()
