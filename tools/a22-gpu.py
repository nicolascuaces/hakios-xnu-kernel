#!/usr/bin/env python3
"""
A22: the Mali-G57 GPU, step 1 -- turn it on from XNU and make it run its first two jobs.
Adds to bsd/dev/redwood_mali.c the MT6833 power-up (redwood_mali_a22.h, boot-arg mali=2);
everything else in the driver (GPU registers, jobs, page tables) is the Infinix's.

  python3 a22-gpu.py ~/dev/hakios          -> only checks
  python3 a22-gpu.py ~/dev/hakios apply    -> applies (saves redwood_mali.c.gpu.orig the first time)
"""
import os
import shutil
import sys

XNU = "xnu-src/xnu-8796.121.2"
DESTINO = XNU + "/bsd/dev/redwood_mali.c"

CAMBIOS = [
    ("static void\nmali_thread(",
     '#include "redwood_mali_a22.h"   /* A22: MT6833 power-up, boot-arg mali=2 */\n\nstatic void\nmali_thread('),
    ("\tmali_up();\n\tprintf(\"mali: came up to step %u of 6\\n\", reached);\n\tif (reached >= 6) {",
     "\tuint32_t start = 0;\n\n"
     "\t(void)PE_parse_boot_argn(\"mali_start\", &start, sizeof(start));\n"
     "\tif (mali_mode == 2) {\n\t\tmali_up_a22();\n\t} else {\n\t\tmali_up();\n\t}\n"
     "\tprintf(\"mali: came up to step %u of 6\\n\", reached);\n"
     "\tif (reached >= 6 && (mali_mode != 2 || start)) {"),
    ("\tif (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {",
     "\tmali_mode = on;\n\tif (kernel_thread_start(mali_thread, NULL, &thread) == KERN_SUCCESS) {"),
]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    aqui = os.path.dirname(os.path.abspath(__file__))
    cabecera = os.path.join(aqui, "redwood_mali_a22.h")
    if not os.path.isfile(cabecera):
        raise SystemExit("redwood_mali_a22.h is missing next to this script")
    ruta = os.path.join(raiz, DESTINO)
    if not os.path.isfile(ruta):
        raise SystemExit("MISSING | " + DESTINO)
    texto = open(ruta, encoding="utf-8").read()
    faltan = 0
    for viejo, nuevo in CAMBIOS:
        if nuevo in texto:
            print("was there  |", nuevo.strip().splitlines()[0][:60])
            continue
        n = texto.count(viejo)
        if n != 1:
            print("NOT FOUND (%d times) |" % n, viejo.strip().splitlines()[0][:60])
            faltan += 1
            continue
        texto = texto.replace(viejo, nuevo)
        print("changes    |", nuevo.strip().splitlines()[0][:60])
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-gpu.py %s apply" % sys.argv[1])
        return
    if not os.path.exists(ruta + ".gpu.orig"):
        shutil.copy2(ruta, ruta + ".gpu.orig")
    open(ruta, "w", encoding="utf-8").write(texto)
    shutil.copy2(cabecera, os.path.join(raiz, XNU, "bsd/dev/redwood_mali_a22.h"))
    print("copied     |", XNU + "/bsd/dev/redwood_mali_a22.h")
    print("\nApplied. Now: ./xnu-build.sh -j8 -k, ./xl_x6826b build and XNU_ARGS with mali=2")


if __name__ == "__main__":
    main()
