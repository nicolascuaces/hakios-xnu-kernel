#!/usr/bin/env python3
"""
A22: backlight experiment. New file bsd/dev/redwood_a22bl.c, boot-arg a22bl_test=1 (nothing happens without it).
Sends the panel's brightness command (DCS 0x51) through the DSI at 0x14013000 and prints the DSI registers.

  python3 tools/a22-backlight.py ~/dev/hakios          -> only checks
  python3 tools/a22-backlight.py ~/dev/hakios apply    -> applies (backups *.bl.orig)

Needs a22-charge.py applied (it hooks in right after redwood_a22charge_init). Copies redwood_a22bl.c (next to this script).
"""
import os, shutil, sys
XNU = "xnu-src/xnu-8796.121.2"
CAMBIOS = {
    XNU + "/bsd/conf/files": [
        ("bsd/dev/redwood_a22charge.c\tstandard\n", "bsd/dev/redwood_a22charge.c\tstandard\nbsd/dev/redwood_a22bl.c\tstandard\n")],
    XNU + "/bsd/sys/redwood_input.h": [
        ("void redwood_a22charge_init(void);",
         "void redwood_a22bl_init(void);          /* boot-arg a22bl_test=1: backlight experiment */\nvoid redwood_a22charge_init(void);")],
    XNU + "/bsd/dev/redwood_a22keys.c": [
        ("extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);\n",
         "extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);\nextern void redwood_a22bl_toggle(void);   /* redwood_a22bl.c */\n"),
        ("\t\t\t\tpresses += k->down;\n",
         "\t\t\t\tpresses += k->down;\n\t\t\t\tif (k->down && k->button == REDWOOD_BUTTON_LOCK) {\n\t\t\t\t\tredwood_a22bl_toggle();\n\t\t\t\t}\n"),
    ],
    XNU + "/bsd/dev/redwood_himax.c": [
        ("static void\nhimax_thread(__unused void *parameter, __unused wait_result_t result)\n{",
         "/* a22-backlight.py: is this thread kept waiting? (prints a gap of the loop, or a slow read, of 40 ms or more) */\n"
         "static void\ngap_check(bool after_read)\n{\n\tstatic uint64_t last_start, read_start;\n\tstatic unsigned lines;\n\tuint64_t now = mach_absolute_time(), ns;\n\n"
         "\tif (!after_read) {\n\t\tif (last_start != 0 && lines < 60) {\n\t\t\tabsolutetime_to_nanoseconds(now - last_start, &ns);\n"
         "\t\t\tif (ns > 40000000ull) {\n\t\t\t\tlines++;\n\t\t\t\tprintf(\"himax: the loop was away %llu ms\\n\", ns / 1000000);\n\t\t\t}\n\t\t}\n"
         "\t\tlast_start = now;\n\t\tread_start = now;\n\t\treturn;\n\t}\n"
         "\tabsolutetime_to_nanoseconds(now - read_start, &ns);\n\tif (ns > 40000000ull && lines < 60) {\n\t\tlines++;\n\t\tprintf(\"himax: a read took %llu ms\\n\", ns / 1000000);\n\t}\n}\n\n"
         "static void\nhimax_thread(__unused void *parameter, __unused wait_result_t result)\n{"),
        ("\t\tread_report(r);\n\t\tif (report_valid(r)) {",
         "\t\tgap_check(false);\n\t\tread_report(r);\n\t\tgap_check(true);\n\t\tif (report_valid(r)) {"),
    ],
    XNU + "/bsd/dev/redwood_input.c": [
        ("\tredwood_a22charge_init();\n", "\tredwood_a22charge_init();\n\tredwood_a22bl_init();\n"),
        ("redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)\n{\n\ttouch_post(type, finger, x, y, true);\n}",
         "redwood_touch_post(uint32_t type, uint32_t finger, uint32_t x, uint32_t y)\n{\n\textern bool redwood_a22bl_swallow(uint32_t type);   /* redwood_a22bl.c: tap to wake */\n\n"
         "\tif (type >= REDWOOD_TOUCH_DOWN && type <= REDWOOD_TOUCH_UP && redwood_a22bl_swallow(type)) {\n\t\treturn;\n\t}\n"
         "\ttouch_post(type, finger, x, y, true);\n}")],
}
COPY = ("redwood_a22bl.c", XNU + "/bsd/dev/redwood_a22bl.c")

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = os.path.expanduser(sys.argv[1]); do = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    here = os.path.dirname(os.path.abspath(__file__))
    if not os.path.isfile(os.path.join(here, COPY[0])):
        raise SystemExit("%s is missing next to this script" % COPY[0])
    new, bad = {}, 0
    for rel, changes in CAMBIOS.items():
        p = os.path.join(root, rel)
        if not os.path.isfile(p):
            print("MISSING    |", rel); bad += 1; continue
        t = open(p, encoding="utf-8").read()
        for old, nw in changes:
            if nw in t:
                print("was there  |", rel); continue
            if t.count(old) != 1:
                print("NOT FOUND (%d times) |" % t.count(old), rel, "|", old.strip()[:60]); bad += 1; continue
            t = t.replace(old, nw); print("changes    |", rel)
        new[p] = t
    if bad:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % bad)
    if not do:
        print("\nEverything fits. To apply it: python3 tools/a22-backlight.py %s apply" % sys.argv[1]); return
    for p, t in new.items():
        if not os.path.exists(p + ".bl.orig"):
            shutil.copy(p, p + ".bl.orig")
        open(p, "w", encoding="utf-8").write(t)
    shutil.copy(os.path.join(here, COPY[0]), os.path.join(root, COPY[1]))
    print("applied")
main()
