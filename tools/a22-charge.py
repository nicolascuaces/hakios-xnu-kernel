#!/usr/bin/env python3
"""
A22: MT6360 charger (not RT9471). New driver bsd/dev/redwood_a22charge.c, boot-arg a22chg_step=N.
Step 1 (READ-ONLY): the I2C5 clock and registers as LK leaves them.
Step 2: reads the MT6360's registers (0x34) over I2C without writing anything to it, and every 10 s the charging state.

  python3 tools/a22-charge.py ~/dev/hakios          -> only checks
  python3 tools/a22-charge.py ~/dev/hakios apply    -> applies (saves *.chg.orig the first time)

Needs a22-audio.py applied first. Copies redwood_a22charge.c (next to this script) into place.
"""
import os, shutil, sys

XNU = "xnu-src/xnu-8796.121.2"
CAMBIOS = {
    XNU + "/bsd/conf/files": [
        ("bsd/dev/redwood_a22audio.c\tstandard\n",
         "bsd/dev/redwood_a22audio.c\tstandard\nbsd/dev/redwood_a22charge.c\tstandard\n"),
    ],
    XNU + "/bsd/sys/redwood_input.h": [
        ("void redwood_a22audio_init(void);",
         "void redwood_a22charge_init(void);      /* boot-arg a22chg_step=N: the A22's MT6360 charger */\n"
         "void redwood_a22audio_init(void);"),
    ],
    XNU + "/bsd/dev/redwood_input.c": [
        ("\tredwood_a22audio_init();\n", "\tredwood_a22audio_init();\n\tredwood_a22charge_init();\n"),
    ],
}
COPIAS = [("redwood_a22charge.c", XNU + "/bsd/dev/redwood_a22charge.c")]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    aqui = os.path.dirname(os.path.abspath(__file__))
    for origen, _ in COPIAS:
        if not os.path.isfile(os.path.join(aqui, origen)):
            raise SystemExit("%s is missing next to this script" % origen)
    nuevos, faltan = {}, 0
    for rel, cambios in CAMBIOS.items():
        ruta = os.path.join(raiz, rel)
        if not os.path.isfile(ruta):
            print("MISSING    |", rel); faltan += 1; continue
        texto = open(ruta, encoding="utf-8").read()
        for viejo, nuevo in cambios:
            if nuevo in texto:
                print("was there  |", rel); continue
            n = texto.count(viejo)
            if n != 1:
                print("NOT FOUND (%d times) |" % n, rel, "|", viejo.strip()[:60]); faltan += 1; continue
            texto = texto.replace(viejo, nuevo)
            print("changes    |", rel)
        nuevos[ruta] = texto
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    if not aplicar:
        print("\nEverything fits. To apply it: python3 tools/a22-charge.py %s apply" % sys.argv[1]); return
    for ruta, texto in nuevos.items():
        if not os.path.exists(ruta + ".chg.orig"):
            shutil.copy2(ruta, ruta + ".chg.orig")
        open(ruta, "w", encoding="utf-8").write(texto)
    for origen, destino in COPIAS:
        shutil.copy2(os.path.join(aqui, origen), os.path.join(raiz, destino))
        print("copied     |", destino)
    print("\nApplied. Now: taskpolicy -b ./xnu-build.sh -j4 -k, ./xl_x6826b build and a22chg_step=1")


if __name__ == "__main__":
    main()
