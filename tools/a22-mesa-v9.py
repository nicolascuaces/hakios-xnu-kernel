#!/usr/bin/env python3
"""
A22: the GPU, step 2 -- Mesa (panfrost) for the A22's Mali-G57, which is Valhall v9.
The Infinix's patch (tools/mesa/panfrost-darwin.patch) builds ONLY v7 on macOS
(the Mali-G52, Bifrost). This script changes those four lines to v9, without changing the number
of lines of the patch (the @@ headers stay valid). tools/mk-mesa.sh sees that the
patch changed and downloads and builds Mesa again by itself.

  python3 a22-mesa-v9.py ~/dev/hakios          -> only checks
  python3 a22-mesa-v9.py ~/dev/hakios apply    -> applies (saves panfrost-darwin.patch.v7.orig)
"""
import os
import shutil
import sys

DESTINO = "tools/mesa/panfrost-darwin.patch"

CAMBIOS = [
    ("+foreach ver : host_machine.system() == 'darwin' ? ['7'] :",
     "+foreach ver : host_machine.system() == 'darwin' ? ['9'] :"),
    ("+panfrost_versions = host_machine.system() == 'darwin' ? ['7'] :",
     "+panfrost_versions = host_machine.system() == 'darwin' ? ['9'] :"),
    ("+#if PAN_ARCH == 5 || (defined(__APPLE__) && PAN_ARCH == 7)   /* Darwin builds v7 alone */",
     "+#if PAN_ARCH == 5 || (defined(__APPLE__) && PAN_ARCH == 9)   /* Darwin builds v9 alone (A22) */"),
    ("+   if (dev->arch == 7)\n+      panfrost_cmdstream_screen_init_v7(screen);",
     "+   if (dev->arch == 9)\n+      panfrost_cmdstream_screen_init_v9(screen);"),
]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    ruta = os.path.join(raiz, DESTINO)
    if not os.path.isfile(ruta):
        raise SystemExit("MISSING | " + DESTINO)
    texto = open(ruta, encoding="utf-8").read()
    lineas = texto.count("\n")
    faltan = 0
    for viejo, nuevo in CAMBIOS:
        if nuevo in texto:
            print("was there  |", nuevo.splitlines()[0][:70])
            continue
        n = texto.count(viejo)
        if n != 1:
            print("NOT FOUND (%d times) |" % n, viejo.splitlines()[0][:70])
            faltan += 1
            continue
        texto = texto.replace(viejo, nuevo)
        print("changes    |", nuevo.splitlines()[0][:70])
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    assert texto.count("\n") == lineas
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-mesa-v9.py %s apply" % sys.argv[1])
        return
    if not os.path.exists(ruta + ".v7.orig"):
        shutil.copy2(ruta, ruta + ".v7.orig")
    open(ruta, "w", encoding="utf-8").write(texto)
    print("\nApplied. Now: DRIVERS=softpipe,panfrost GL=1 RC_JOBS=rc.jobs.unlock ./userland/build.sh")


if __name__ == "__main__":
    main()
