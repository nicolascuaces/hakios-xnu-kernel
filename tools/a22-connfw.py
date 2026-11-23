#!/usr/bin/env python3
"""
A22 / Wi-Fi step 5: CONNSYS's firmware goes in the small RAM disk (root/fw), not in the iOS system.
Changes userland/build.sh: before creating root.apfs it copies the soc2_2_*.bin files from
xnu-deps/build/connsys-fw/firmware into root/fw.

  python3 tools/a22-connfw.py ~/dev/hakios          -> only checks
  python3 tools/a22-connfw.py ~/dev/hakios apply    -> applies (saves build.sh.conn.orig)
"""
import os, shutil, sys

MARCA = "python3 ../tools/apfs/mkapfs.py -z --plain fw -o root.apfs redwood=root | tail -1"
NUEVO = ('# the A22\'s CONNSYS firmware (Wi-Fi, Bluetooth) into the small image\'s fw/ (bsd/dev/redwood_a22connsys.c)\n'
         'if [ -d ../xnu-deps/build/connsys-fw/firmware ]; then\n'
         '\tmkdir -p root/fw\n'
         '\tcp ../xnu-deps/build/connsys-fw/firmware/soc2_2_* root/fw/\n'
         'fi\n')

def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    ruta = os.path.join(os.path.expanduser(sys.argv[1]), "userland/build.sh")
    t = open(ruta, encoding="utf-8").read()
    if "soc2_2_* root/fw/" in t:
        print("was there"); return
    if t.count(MARCA) != 1:
        raise SystemExit("DOES NOT FIT (%d times the mkapfs line): touching nothing" % t.count(MARCA))
    if len(sys.argv) < 3 or sys.argv[2] not in ("apply", "aplicar"):
        print("It fits. To apply: python3 tools/a22-connfw.py %s apply" % sys.argv[1]); return
    shutil.copy2(ruta, ruta + ".conn.orig")
    open(ruta, "w", encoding="utf-8").write(t.replace(MARCA, NUEVO + MARCA))
    print("Applied.")

main()
