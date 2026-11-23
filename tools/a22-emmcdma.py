#!/usr/bin/env python3
"""
A22: the eMMC by DMA in iOS. Replaces xnu-src/.../bsd/dev/redwood_emmc.c with the version that has DMA
(redwood_emmc.c, next to this script). At boot, XNU reads the first 4 MB by DMA and by PIO and
keeps DMA only if they match byte for byte; on any error it goes back to PIO. Boot-arg emmc_dma=0
turns it off.

  python3 a22-emmcdma.py ~/dev/hakios          -> only checks
  python3 a22-emmcdma.py ~/dev/hakios apply    -> applies (saves redwood_emmc.c.dma.orig the first time)
"""
import hashlib
import os
import shutil
import sys

DESTINO = "xnu-src/xnu-8796.121.2/bsd/dev/redwood_emmc.c"
ORIGINAL = "91def2afccf1b234ca3c5fd516b4e0eb72d9724ee67bad75b9c561f72ec7822a"
ANTERIORES = {ORIGINAL, "cc69bc904ded65401b18f22067f5edceae58ae2d897b39d1e594c135f172c0f6"}   # original y DMA v1


def sha(ruta):
    return hashlib.sha256(open(ruta, "rb").read()).hexdigest()


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    nuevo = os.path.join(os.path.dirname(os.path.abspath(__file__)), "redwood_emmc.c")
    destino = os.path.join(raiz, DESTINO)
    if not os.path.isfile(nuevo):
        raise SystemExit("redwood_emmc.c is missing next to this script")
    if not os.path.isfile(destino):
        raise SystemExit("MISSING | " + DESTINO)
    actual = sha(destino)
    if actual == sha(nuevo):
        print("was there  | " + DESTINO + " (with DMA)")
        return
    if actual not in ANTERIORES:
        raise SystemExit("the redwood_emmc.c in your tree is not the original (%s...): touching nothing. "
                         "Send it to me." % actual[:12])
    print("changes    | " + DESTINO + " -> version with DMA")
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-emmcdma.py %s apply" % sys.argv[1])
        return
    if not os.path.exists(destino + ".dma.orig"):
        shutil.copy2(destino, destino + ".dma.orig")
    shutil.copy2(nuevo, destino)
    print("\nApplied. Now: ./xnu-build.sh -j8 -k, rebuild the payload and run the cycle")


if __name__ == "__main__":
    main()
