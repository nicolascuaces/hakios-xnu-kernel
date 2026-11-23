#!/usr/bin/env python3
"""
Puts the Samsung A22 5G (SM-A226BR, MT6833) values into the HakiOS tree (mtk branch).

  python3 a22-constantes.py ~/dev/hakios          -> check only (changes nothing)
  python3 a22-constantes.py ~/dev/hakios apply   -> changes the files (saves .orig the first time)

Values taken from the 2026-11-16 recon (DT, iomem, cmdline). Those marked "inferred" are
confirmed by fbdiag in Part 5.
"""
import os
import shutil
import sys

BLOQUE_A22 = ("#if 0 /* A22: touch_bring_up is for the Infinix's TD4160 touch controller (MT6768 pins, clocks and SPI) */\n"
              "\ttouch_bring_up();\n#endif\n"
              "#if 0 /* A22: the MT6833 A76 PLL is not verified */\n\tbig_cores_clock();\n#endif\n")

CAMBIOS = {
    "mtkloader/mtkdbg.h": [
        ("#define RAMOOPS_CONSOLE      0x4d09f000ULL",
         "#define RAMOOPS_CONSOLE      0x4811f000ULL  /* A22: 0x48090000 + 0x8f000 of dumps */"),
        ("#define RAMOOPS_PMSG         0x4d0e0000ULL",
         "#define RAMOOPS_PMSG         0x48160000ULL  /* A22 */"),
        ("#define LK_FB_BASE           0x787b0000ULL",
         "#define LK_FB_BASE           0x7c9b0000ULL  /* A22: mblock-20-framebuffer */"),
        ("#define LK_FB_SIZE           0x1050000ULL",
         "#define LK_FB_SIZE           0x1e50000ULL"),
        ("#define LK_FB_PITCH          2944u      /* 736 pixels: LK aligns the width to 32 */",
         "#define LK_FB_PITCH          4352u      /* A22: 1088 px (1080 aligned to 32), inferred */"),
        ("#define PANEL_W              720u", "#define PANEL_W              1080u"),
        ("#define PANEL_H              1640u", "#define PANEL_H              2400u  /* A22: tpd-resolution 1080x2400 */"),
        ("#define OVL0_BASE            0x1400b000ULL",
         "#define OVL0_BASE            0x14005000ULL  /* A22: disp_ovl0@14005000 */"),
    ],
    "mtkloader/fbdiag.c": [
        ("#define MARKER_PA     0x4d0df000ULL",
         "#define MARKER_PA     0x4815f000ULL /* A22: header of the ramoops ftrace zone */"),
        ("static const uint64_t probe_pa[] = { 0x42000000ULL, 0x47b1c9c0ULL, 0x80000000ULL, 0xc0000000ULL, 0xc7b1c9c0ULL, 0xd0000000ULL, 0x140000000ULL };",
         "static const uint64_t probe_pa[] = { 0x80000000ULL, 0x80100000ULL, 0xc0000000ULL, 0x100000000ULL, 0x13e000000ULL }; /* A22: 4 GB, nothing at 0x140000000 */"),
        ("l0_pitch >= PANEL_W * 4 && l0_pitch <= 4096)",
         "l0_pitch >= PANEL_W * 4 && l0_pitch <= 8192)"),
    ],
    "mtkloader/mtkloader.c": [
        ("#define XNU_PHYS_BASE       0xc0000000ULL",
         "#define XNU_PHYS_BASE       0x100000000ULL  /* A22: between 0xe0000000 and 0xffff0000 there are the modem and DPM */"),
        ("#define XNU_MEM_SIZE        0xfe000000ULL       /* up to dramc-rk1 at 0x1bffff000; room for the system image */",
         "#define XNU_MEM_SIZE        0x3e000000ULL       /* A22: 992 MB clean, below dramc-rk1 (0x13ffff000) */"),
        ("pitch >= PANEL_W * 4 && pitch <= 4096)",
         "pitch >= PANEL_W * 4 && pitch <= 8192)"),
        # two possible starting points: the original file, or the one that already had the PLL patch
        ("\ttouch_bring_up();\n\tbig_cores_clock();\n", BLOQUE_A22, True),
        ("\ttouch_bring_up();\n#if 0 /* A22: the MT6833 A76 PLL is not verified */\n\tbig_cores_clock();\n#endif\n",
         BLOQUE_A22, True),
    ],
    # XNU with <= 1 GB advertises 4K pages and then rejects the 16K signature of the iOS 16
    # dyld cache ("disallowing unsupported code signature page shift: 14"). The A22 gives it 992 MB.
    # With > 1 GB (the Infinix) it advertises 16K over 4K hardware pages, like the iPad Air 2.
    "xnu-src/xnu-8796.121.2/osfmk/arm/arm_init.c": [
        ("\t\tif (args->memSize > 1ULL * 1024 * 1024 * 1024) {\n",
         "\t\tif (1 || args->memSize > 1ULL * 1024 * 1024 * 1024) { /* A22: 992 MB, but the iOS 16 dyld cache is signed in 16K */\n"),
    ],
}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    raiz = os.path.expanduser(sys.argv[1])
    aplicar = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    faltan = 0
    nuevos = {}
    for rel, pares in CAMBIOS.items():
        ruta = os.path.join(raiz, rel)
        if not os.path.isfile(ruta):
            print("MISSING:", ruta)
            faltan += 1
            continue
        texto = open(ruta, encoding="utf-8").read()
        for par in pares:
            viejo, nuevo = par[0], par[1]
            opcional = len(par) > 2 and par[2]
            if texto.count(nuevo) == 1 and viejo not in texto:
                print("already set", rel, "|", nuevo.strip().splitlines()[0][:60])
                continue
            n = texto.count(viejo)
            if n == 0 and opcional:
                continue
            if n != 1:
                print("NOT FOUND (%d times)" % n, rel, "|", viejo.strip().splitlines()[0][:60])
                faltan += 1
                continue
            texto = texto.replace(viejo, nuevo)
            print("changes    ", rel, "|", nuevo.strip().splitlines()[0][:60])
        for par in pares:
            if par[1] not in texto:
                print("MISSING     ", rel, "|", par[1].strip().splitlines()[0][:60])
                faltan += 1
                break
        nuevos[ruta] = texto
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-constantes.py %s apply" % sys.argv[1])
        return
    for ruta, texto in nuevos.items():
        if not os.path.exists(ruta + ".orig"):
            shutil.copy2(ruta, ruta + ".orig")
        open(ruta, "w", encoding="utf-8").write(texto)
    print("\nApplied. Originals in *.orig")


main()
