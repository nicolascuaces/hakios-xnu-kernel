#!/usr/bin/env python3
"""
A22: the GPU, step 3 -- a single MMU UPDATE before each job, not one per buffer.
The G57 keeps empty entries in its TLB, which is why mali_fastmap=0 is needed (one MMU UPDATE
for every new buffer, ~2.4 ms each, with the processor waiting). With the boot-arg
mali_fastmap=2, every new or removed buffer only marks "tables changed", and the UPDATE is
done once, right before the next job (the GPU is idle then).

  python3 a22-lazymmu.py ~/dev/hakios          -> only checks
  python3 a22-lazymmu.py ~/dev/hakios apply    -> applies (saves redwood_mali.c.lazy.orig)
"""
import os, shutil, sys

DESTINO = "xnu-src/xnu-8796.121.2/bsd/dev/redwood_mali.c"

CAMBIOS = [
    ("#define TRANSCFG_AARCH64_4K (mali_fastmap ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB)",
     "#define TRANSCFG_AARCH64_4K (mali_fastmap == 1 ? TRANSCFG_WALK_NC : TRANSCFG_WALK_WB)\n"
     "static volatile int mali_mmu_dirty;   /* mali_fastmap=2 (A22): entries changed, UPDATE before the next job */"),
    ("\tlck_mtx_unlock(&mali_lock);\n\tlck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */\n",
     "\tlck_mtx_unlock(&mali_lock);\n\tif (mali_fastmap == 2) {\n\t\tmali_mmu_dirty = 1;\n\t\treturn;\n\t}\n"
     "\tlck_mtx_lock(&mali_job_lock);                   /* an entry taken out: the TLB may hold it */\n"),
    ("\tif (mali_fastmap) {\n\t\treturn;         /* see TRANSCFG_WALK_NC */\n\t}\n",
     "\tif (mali_fastmap == 2) {\n\t\tmali_mmu_dirty = 1;\n\t\treturn;\n\t}\n"
     "\tif (mali_fastmap) {\n\t\treturn;         /* see TRANSCFG_WALK_NC */\n\t}\n"),
    ("\tlck_mtx_lock(&mali_job_lock);\n\tlck_mtx_lock(&mali_lock);\n\twr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));\n",
     "\tlck_mtx_lock(&mali_job_lock);\n\tlck_mtx_lock(&mali_lock);\n"
     "\tif (mali_mmu_dirty) {\n\t\tmali_mmu_dirty = 0;\n\t\tas0_update(pt_top_phys, TRANSCFG_AARCH64_4K);\n\t}\n"
     "\twr(mali, JOB_INT_CLEAR, (1u << slot) | (1u << (slot + 16)));\n"),
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
    faltan = 0
    for viejo, nuevo in CAMBIOS:
        if nuevo in texto:
            print("was there  |", nuevo.splitlines()[0][:70]); continue
        n = texto.count(viejo)
        if n != 1:
            print("NOT FOUND (%d times) |" % n, viejo.splitlines()[0][:70]); faltan += 1; continue
        texto = texto.replace(viejo, nuevo)
        print("changes    |", nuevo.splitlines()[0][:70])
    if faltan:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % faltan)
    if not aplicar:
        print("\nEverything fits. To apply it: python3 a22-lazymmu.py %s apply" % sys.argv[1]); return
    if not os.path.exists(ruta + ".lazy.orig"):
        shutil.copy2(ruta, ruta + ".lazy.orig")
    open(ruta, "w", encoding="utf-8").write(texto)
    print("\nApplied. Now: ./xnu-build.sh -j8 -k, ./xl_x6826b build and mali_fastmap=2")


if __name__ == "__main__":
    main()
