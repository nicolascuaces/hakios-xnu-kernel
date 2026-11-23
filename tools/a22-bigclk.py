#!/usr/bin/env python3
"""
A22: raises the two Cortex-A76 (cpu6-7) from 725 MHz to their real speed before jumping to XNU.

  python3 a22-bigclk.py ~/dev/hakios          -> only checks
  python3 a22-bigclk.py ~/dev/hakios apply    -> applies (saves mtkloader.c.bigclk.orig the first time)

Copies mtka22clk.h (next to this script) into mtkloader/. The speed is chosen at compile time:
-DA22_BIG_MHZ=1418 (gentle test), 2000 (default) or 0 (touch nothing).
"""
import os
import re
import shutil
import sys

VIEJO_INC = "static void __attribute__((noreturn)) run_xnu(void)\n"
NUEVO_INC = '#include "mtka22clk.h"             /* A22: clock and voltage of the A76s */\n\n' + VIEJO_INC
VIEJO_LLAMADA = re.compile(r"#if 0 /\* A22[^\n]*\*/\n\tbig_cores_clock\(\);\n#endif\n")
NUEVO_LLAMADA = "#if A22_BIG_MHZ\n\ta22_big_cores_clock();      /* A22: the A76s at their speed */\n#endif\n"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    aqui = os.path.dirname(os.path.abspath(__file__))
    if not os.path.isfile(os.path.join(aqui, "mtka22clk.h")):
        raise SystemExit("mtka22clk.h is missing next to this script")
    ruta = os.path.join(raiz, "mtkloader", "mtkloader.c")
    if not os.path.isfile(ruta):
        raise SystemExit("MISSING | mtkloader/mtkloader.c")
    texto = open(ruta, encoding="utf-8").read()
    faltan = 0
    for viejo, nuevo in ((VIEJO_INC, NUEVO_INC), (VIEJO_LLAMADA, NUEVO_LLAMADA)):
        if nuevo in texto:
            print("was there  |", nuevo.strip().splitlines()[0][:60])
            continue
        if isinstance(viejo, str):
            n = texto.count(viejo)
            etiqueta = viejo.strip().splitlines()[0][:60]
        else:
            n = len(viejo.findall(texto))
            etiqueta = "#if 0 /* A22 ... */ big_cores_clock();"
        if n != 1:
            print("NOT FOUND (%d times) |" % n, etiqueta)
            faltan += 1
            continue
        if isinstance(viejo, str):
            texto = texto.replace(viejo, nuevo)
        else:
            texto = viejo.sub(lambda _: nuevo, texto)
        print("changes    |", nuevo.strip().splitlines()[0][:60])
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-bigclk.py %s apply" % sys.argv[1])
        return
    if not os.path.exists(ruta + ".bigclk.orig"):
        shutil.copy2(ruta, ruta + ".bigclk.orig")
    open(ruta, "w", encoding="utf-8").write(texto)
    shutil.copy2(os.path.join(aqui, "mtka22clk.h"), os.path.join(raiz, "mtkloader", "mtka22clk.h"))
    print("copied     | mtkloader/mtka22clk.h")
    print("\nApplied. Now redo the cycle with tools/xnu-cycle-a22.sh")


if __name__ == "__main__":
    main()
