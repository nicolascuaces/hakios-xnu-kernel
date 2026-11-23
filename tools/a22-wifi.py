#!/usr/bin/env python3
"""
A22: Wi-Fi step 8. New gen4m driver bsd/dev/redwood_a22wifi.c (adapted from the Infinix's
redwood_mtkwifi.c), called by redwood_a22connsys.c after "WMT IS UP" with a22conn_step>=8.
Also makes userland/build.sh copy WIFI_RAM_CODE_soc2_2_* into root/fw.

  python3 tools/a22-wifi.py ~/dev/hakios          -> only checks
  python3 tools/a22-wifi.py ~/dev/hakios apply    -> applies (saves *.conn.orig the first time)

Copies redwood_a22wifi.c and redwood_a22connsys.c (next to this script) into place.
"""
import os, shutil, sys

XNU = "xnu-src/xnu-8796.121.2"
CAMBIOS = {
    XNU + "/bsd/conf/files": [
        ("bsd/dev/redwood_a22connsys.c\tstandard\n",
         "bsd/dev/redwood_a22connsys.c\tstandard\nbsd/dev/redwood_a22wifi.c\tstandard\n"),
    ],
    "userland/build.sh": [
        ("\tcp ../xnu-deps/build/connsys-fw/firmware/soc2_2_* root/fw/\n",
         "\tcp ../xnu-deps/build/connsys-fw/firmware/soc2_2_* root/fw/\n"
         "\tcp ../xnu-deps/build/connsys-fw/firmware/WIFI_RAM_CODE_soc2_2_* root/fw/\n"),
    ],
}
COPIAS = [("redwood_a22wifi.c", XNU + "/bsd/dev/redwood_a22wifi.c"),
          ("redwood_a22connsys.c", XNU + "/bsd/dev/redwood_a22connsys.c")]


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
        print("\nEverything fits. To apply it: python3 tools/a22-wifi.py %s apply" % sys.argv[1]); return
    for ruta, texto in nuevos.items():
        if not os.path.exists(ruta + ".conn.orig"):
            shutil.copy2(ruta, ruta + ".conn.orig")
        open(ruta, "w", encoding="utf-8").write(texto)
    for origen, destino in COPIAS:
        shutil.copy2(os.path.join(aqui, origen), os.path.join(raiz, destino))
        print("copied     |", destino)
    print("\nApplied. Now: userland/build.sh, ./xnu-build.sh -j8 -k, ./xl_x6826b build and a22conn_step=9")


if __name__ == "__main__":
    main()
