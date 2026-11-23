#!/usr/bin/env python3
"""
A22: boot iOS without a PC (Vol- at power-on) and go back to Android without a PC (Vol+ and Vol- for 3 s).

  python3 a22-autoboot.py ~/dev/hakios          -> only checks (changes nothing)
  python3 a22-autoboot.py ~/dev/hakios apply    -> applies (saves .autoboot.orig the first time)

Changes:
  mtkloader/mtkloader.c   includes mtka22boot.h; boot_choice() looks at Vol- if there is no request in pmsg;
                          the boot-args get epoch= from the PMIC's RTC if they do not carry one
  bsd/dev/redwood_a22keys.c  Vol+ and Vol- together for 3 s -> loader_return() (boot-arg exitkeys=0 turns it off)
Copies mtka22boot.h (next to this script) into mtkloader/.
The loader's new code is only active when compiling with -DA22_KEY_BOOT=1 (hakios-fijo-a22.sh sets it).
"""
import os
import shutil
import sys

XNU = "xnu-src/xnu-8796.121.2"

LOADER = "mtkloader/mtkloader.c"
KEYS = XNU + "/bsd/dev/redwood_a22keys.c"

CAMBIOS = {
    LOADER: [
        ("static void __attribute__((noreturn)) run_xnu(void)\n",
         '#include "mtka22boot.h"             /* A22: Vol- at power-on = iOS; time from the RTC */\n\n'
         "static void __attribute__((noreturn)) run_xnu(void)\n"),
        ("\tint xnu = MENU_DEFAULT_XNU || xnu_requested();\n",
         "\tint xnu = MENU_DEFAULT_XNU || xnu_requested() || a22_key_wants_xnu();   /* A22: Vol- */\n"),
        ("\tmemcpy(ba->commandLine, XNU_BOOT_ARGS, sizeof(XNU_BOOT_ARGS));\n",
         "\tmemcpy(ba->commandLine, XNU_BOOT_ARGS, sizeof(XNU_BOOT_ARGS));\n"
         "\ta22_append_epoch(ba->commandLine, sizeof(ba->commandLine));   /* A22: time from the RTC */\n"),
    ],
    KEYS: [
        ("#include <sys/systm.h>\n",
         "#include <sys/systm.h>\n\n"
         "/* A22: Vol+ and Vol- together for 3 s = end of the session, back to Android (like the 5 fingers) */\n"
         "extern void loader_return(void);\n"
         "extern void redwood_adreno_stop(void);\n"
         "static uint32_t a22_exit_keys = 1, a22_both;\n"),
        ('\tPE_parse_boot_argn("keylog", &log, sizeof(log));\n',
         '\tPE_parse_boot_argn("keylog", &log, sizeof(log));\n'
         '\tPE_parse_boot_argn("exitkeys", &a22_exit_keys, sizeof(a22_exit_keys));\n'),
        ("\t\tif (tick % 3000 == 0) {\n",
         "\t\tif (a22_exit_keys && keys[0].down && keys[1].down) {\n"
         "\t\t\tif (++a22_both == 150) {     /* 150 x 20 ms = 3 s */\n"
         '\t\t\t\tprintf("\\n--- a22keys: volume up and down held 3 s: back to the loader ---\\n");\n'
         "\t\t\t\tredwood_adreno_stop();\n"
         "\t\t\t\tloader_return();\n"
         "\t\t\t}\n"
         "\t\t} else {\n"
         "\t\t\ta22_both = 0;\n"
         "\t\t}\n"
         "\t\tif (tick % 3000 == 0) {\n"),
    ],
}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    aqui = os.path.dirname(os.path.abspath(__file__))
    cabecera = os.path.join(aqui, "mtka22boot.h")
    if not os.path.isfile(cabecera):
        raise SystemExit("mtka22boot.h is missing next to this script")
    nuevos, faltan = {}, 0
    for rel, cambios in CAMBIOS.items():
        ruta = os.path.join(raiz, rel)
        if not os.path.isfile(ruta):
            print("MISSING    |", rel)
            faltan += 1
            continue
        texto = open(ruta, encoding="utf-8").read()
        for viejo, nuevo in cambios:
            etiqueta = rel.split("/")[-1] + ": " + viejo.strip()[:50]
            if nuevo in texto:
                print("was there  |", etiqueta)
                continue
            n = texto.count(viejo)
            if n != 1:
                print("NOT FOUND (%d times) | %s" % (n, etiqueta))
                faltan += 1
                continue
            texto = texto.replace(viejo, nuevo)
            print("changes    |", etiqueta)
        nuevos[ruta] = texto
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-autoboot.py %s apply" % sys.argv[1])
        return
    for ruta, texto in nuevos.items():
        if not os.path.exists(ruta + ".autoboot.orig"):
            shutil.copy2(ruta, ruta + ".autoboot.orig")
        open(ruta, "w", encoding="utf-8").write(texto)
    shutil.copy2(cabecera, os.path.join(raiz, "mtkloader", "mtka22boot.h"))
    print("copied     | mtkloader/mtka22boot.h")
    print("\nApplied. Rebuild the kernel (xnu-build.sh) and the payload (xl_x6826b build).")


if __name__ == "__main__":
    main()
