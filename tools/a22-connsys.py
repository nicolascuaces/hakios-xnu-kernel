#!/usr/bin/env python3
"""
A22: Wi-Fi, steps 1 and 2 (READ-ONLY). New driver bsd/dev/redwood_a22connsys.c, boot-arg
a22conn_step=1 or 2. Reads CONNSYS's power state and its supplies in the PMIC; writes nothing.

  python3 tools/a22-connsys.py ~/dev/hakios          -> only checks
  python3 tools/a22-connsys.py ~/dev/hakios apply    -> applies (saves *.conn.orig the first time)

Needs a22-keys.py applied first. Copies redwood_a22connsys.c (next to this script) into place.
"""
import os, shutil, sys

XNU = "xnu-src/xnu-8796.121.2"
CAMBIOS = {
    XNU + "/bsd/conf/files": [
        ("bsd/dev/redwood_a22keys.c\tstandard\n",
         "bsd/dev/redwood_a22keys.c\tstandard\nbsd/dev/redwood_a22connsys.c\tstandard\n"),
    ],
    XNU + "/bsd/sys/redwood_input.h": [
        ("void redwood_a22keys_init(void);",
         "void redwood_a22connsys_init(void);     /* boot-arg a22conn_step=N: the A22's CONNSYS (read) */\n"
         "void redwood_a22keys_init(void);"),
    ],
    XNU + "/bsd/dev/redwood_input.c": [
        ("\tredwood_a22keys_init();\n", "\tredwood_a22keys_init();\n\tredwood_a22connsys_init();\n"),
    ],
}
COPIAS = [("redwood_a22connsys.c", XNU + "/bsd/dev/redwood_a22connsys.c")]


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
        print("\nEverything fits. To apply it: python3 tools/a22-connsys.py %s apply" % sys.argv[1]); return
    for ruta, texto in nuevos.items():
        if not os.path.exists(ruta + ".conn.orig"):
            shutil.copy2(ruta, ruta + ".conn.orig")
        open(ruta, "w", encoding="utf-8").write(texto)
    for origen, destino in COPIAS:
        shutil.copy2(os.path.join(aqui, origen), os.path.join(raiz, destino))
        print("copied     |", destino)
    print("\nApplied. Now: ./xnu-build.sh -j8 -k, ./xl_x6826b build and a22conn_step=2")


if __name__ == "__main__":
    main()
