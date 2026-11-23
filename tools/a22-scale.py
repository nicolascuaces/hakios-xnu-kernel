#!/usr/bin/env python3
"""
A22: keyboard too big / cut off. The loader (mtkloader) sets /chosen display-scale = 2 (inherited from the
Infinix, 720 px), but the screen iOS sees is the iPhone X's at 3x (1125 x 2436). The keyboard takes its scale
from there and is drawn at another size. This patch makes MiniIOMobileFramebuffer write display-scale = 3 (and
artwork-scale-factor = 3) when fbscale is not used, just as it already does with fbscale=2.

  python3 tools/a22-scale.py ~/dev/hakios          -> only checks
  python3 tools/a22-scale.py ~/dev/hakios apply    -> applies (saves *.conn.orig the first time)
"""
import os, shutil, sys

XNU = "xnu-src/xnu-8796.121.2"
VIEJO = "\tif (gFBScale != 0 && gFBScale != kFBDefaultScale) {\n"
NUEVO = ("\tif (gFBScale == 0 || gFBScale == kFBDefaultScale) {\n"
    "\t\t/* A22: the loader says display-scale 2 (the Infinix's); the screen iOS gets is the iPhone X's at 3x */\n"
    "\t\tstatic const char * const places3[][2] = { { \"/chosen\", \"display-scale\" }, { \"/product\", \"artwork-scale-factor\" } };\n"
    "\t\tuint32_t three = kFBDefaultScale;\n\n"
    "\t\tfor (unsigned i = 0; i < sizeof(places3) / sizeof(places3[0]); i++) {\n"
    "\t\t\tIORegistryEntry * node = IORegistryEntry::fromPath(places3[i][0], gIODTPlane);\n\n"
    "\t\t\tif (node != NULL) {\n"
    "\t\t\t\tnode->setProperty(places3[i][1], &three, sizeof(three));\n"
    "\t\t\t\tnode->release();\n"
    "\t\t\t}\n"
    "\t\t}\n"
    "\t\tIOLog(\"MiniIOMobileFramebuffer: display-scale set to 3 (A22)\\n\");\n"
    "\t}\n" + VIEJO)
CAMBIOS = {
    XNU + "/iokit/Kernel/MiniIOMobileFramebuffer.cpp": [(VIEJO, NUEVO)],
}
COPIAS = []


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
        print("\nEverything fits. To apply it: python3 tools/a22-scale.py %s apply" % sys.argv[1]); return
    for ruta, texto in nuevos.items():
        if not os.path.exists(ruta + ".conn.orig"):
            shutil.copy2(ruta, ruta + ".conn.orig")
        open(ruta, "w", encoding="utf-8").write(texto)
    for origen, destino in COPIAS:
        shutil.copy2(os.path.join(aqui, origen), os.path.join(raiz, destino))
        print("copied     |", destino)
    print("\nApplied. Now: ./xnu-build.sh -j4 -k and ./xl_x6826b build")


if __name__ == "__main__":
    main()
