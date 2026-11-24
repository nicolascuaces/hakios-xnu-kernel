#!/usr/bin/env python3
"""
a22-dram3.py -- a third and bigger piece of DRAM for iOS: 1910 MB instead of 1504 MB.

  python3 tools/a22-dram3.py ~/dev/hakios            # review only (changes nothing)
  python3 tools/a22-dram3.py ~/dev/hakios apply      # apply (backups *.dram3.orig)

Needs a22-dram2.py already applied. Android's own memory map (logs/a22-dram-map.txt) says 0xa0000000-0xb95fffff
is plain "System RAM" (406 MB) right below the block XNU already has. So XNU's span now starts at 0xa0000000:
  0xa0000000 - 0xb95fffff   406 MB   new
  0xb9600000 - 0xbfffffff   106 MB   hole A (small pieces, TEE/wifi/ccci carve-outs; not worth the risk)
  0xc0000000 - 0xdfffffff   512 MB   as in dram2
  0xe0000000 - 0xffffffff   512 MB   hole B (modem, DPM), as in dram2
  0x100000000 - 0x13dffffff 992 MB   as in dram2
The kernel takes two holes as boot-args (ram_hole_start_mb/end_mb and ram_hole2_start_mb/end_mb) that the loader
appends; the physical map is built in three pieces; the hole pages stay in vm_pages as vmp_error like in dram2.
Back out: build the loader with -DA22_DRAM2_ONLY (the dram2 layout, 1504 MB) or -DA22_ONE_BANK (992 MB).
"""
import os, re, shutil, sys

XNU = "xnu-src/xnu-8796.121.2"
FLAGS = "ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT"
MARK = "A22 dram3"

def sub1(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit("NOT FOUND (%d times): %s" % (text.count(old), what))
    return text.replace(old, new)

def loader(t):
    if MARK in t: return t
    t = sub1(t, "#define XNU_PHYS_BASE       0xc0000000ULL\n#define XNU_MEM_SIZE        0x7e000000ULL\n#endif\n",
        "#ifdef A22_DRAM2_ONLY\n#define XNU_PHYS_BASE       0xc0000000ULL\n#define XNU_MEM_SIZE        0x7e000000ULL\n#else\n"
        "/* A22 dram3: 0xa0000000..0x13dffffff is one span for XNU, with two holes the kernel is told about: 0xb9600000..0xbfffffff\n"
        " * (small carve-outs) and 0xe0000000..0xffffffff (modem, DPM). Usable: 406 + 512 + 992 = 1910 MB. */\n"
        "#define XNU_PHYS_BASE       0xa0000000ULL\n#define XNU_MEM_SIZE        0x9e000000ULL\n#endif\n#endif\n", "loader base")
    t = sub1(t, '\t\tstatic const char hole[] = " ram_hole_start_mb=3584 ram_hole_end_mb=4096";\n',
        '#ifdef A22_DRAM2_ONLY\n\t\tstatic const char hole[] = " ram_hole_start_mb=3584 ram_hole_end_mb=4096";\n#else\n'
        '\t\tstatic const char hole[] = " ram_hole_start_mb=2966 ram_hole_end_mb=3072 ram_hole2_start_mb=3584 ram_hole2_end_mb=4096";\n#endif\n', "loader hole args")
    return t

def vminit(t):
    if MARK in t: return t
    t = sub1(t, "pmap_paddr_t ram_hole_start, ram_hole_end;\n",
        "pmap_paddr_t ram_hole_start, ram_hole_end, ram_hole2_start, ram_hole2_end;       /* A22 dram3: two holes */\n\n"
        "/* A22 dram3: is this physical address in one of the holes? (vm_resident.c) */\nbool\nram_hole_page(pmap_paddr_t pa)\n{\n"
        "\treturn (pa >= ram_hole_start && pa < ram_hole_end) || (pa >= ram_hole2_start && pa < ram_hole2_end);\n}\n", "hole globals")
    t = sub1(t, "#define PTOV_TABLE_SIZE 8\n", "#define PTOV_TABLE_SIZE 16      /* A22 dram3: more pieces of the physical map (was 8) */\n", "ptov size")
    t = sub1(t, "\t\t\tram_hole_end = (pmap_paddr_t)hole_end_mb << 20;\n\t\t}\n",
        "\t\t\tram_hole_end = (pmap_paddr_t)hole_end_mb << 20;\n\t\t}\n"
        "\t\tif (PE_parse_boot_argn(\"ram_hole2_start_mb\", &hole_start_mb, sizeof(hole_start_mb)) &&\n"
        "\t\t    PE_parse_boot_argn(\"ram_hole2_end_mb\", &hole_end_mb, sizeof(hole_end_mb)) && hole_end_mb > hole_start_mb) {\n"
        "\t\t\tram_hole2_start = (pmap_paddr_t)hole_start_mb << 20;\n\t\t\tram_hole2_end = (pmap_paddr_t)hole_end_mb << 20;\n\t\t}\n", "hole parse")
    a = t.index("\t/* A22 dram2: the hole (modem memory) is left out of the physical map")
    b = t.index("\tassert((temp_ptov_table[ptov_index - 1].va", a)
    new = ("\t/* A22 dram3: up to two holes (not memory) are left out of the physical map; what lies between them is mapped */\n"
        "\t{\n\t\tpmap_paddr_t from = args->topOfKernelData;\n"
        "\t\tpmap_paddr_t hs[2] = { ram_hole_start, ram_hole2_start }, he[2] = { ram_hole_end, ram_hole2_end };\n\n"
        "\t\tfor (int h = 0; h < 2; h++) {\n"
        "\t\t\tif (he[h] > hs[h] && (hs[h] < from || he[h] > real_avail_end || (hs[h] & PAGE_MASK) || (he[h] & PAGE_MASK))) {\n"
        "\t\t\t\ths[h] = he[h] = 0;      /* does not fit, or out of order: ignored */\n\t\t\t}\n"
        "\t\t\tif (he[h] > hs[h]) {\n\t\t\t\tif (hs[h] > from) {\n"
        "\t\t\t\t\tarm_vm_physmap_slide(temp_ptov_table, (from - gPhysBase + gVirtBase), hs[h] - from, AP_RWNA,\n"
        "\t\t\t\t\t    %s);\n\t\t\t\t}\n\t\t\t\tfrom = he[h];\n\t\t\t}\n\t\t}\n"
        "\t\tram_hole_start = hs[0];\n\t\tram_hole_end = he[0];\n\t\tram_hole2_start = hs[1];\n\t\tram_hole2_end = he[1];\n"
        "\t\tif (real_avail_end > from) {\n"
        "\t\t\tarm_vm_physmap_slide(temp_ptov_table, (from - gPhysBase + gVirtBase), real_avail_end - from, AP_RWNA,\n"
        "\t\t\t    %s);       /* rest of physmem */\n\t\t}\n\t}\n\n") % (FLAGS, FLAGS)
    t = t[:a] + new + t[b:]
    t = sub1(t, "\tif (ram_hole_end > ram_hole_start) {    /* A22 dram2: the hole is not memory */\n"
        "\t\tsane_size -= (ram_hole_end - ram_hole_start);\n\t\tmax_mem -= (ram_hole_end - ram_hole_start);\n"
        "\t\tmax_mem_actual -= (ram_hole_end - ram_hole_start);\n\t}\n",
        "\tif (ram_hole_end > ram_hole_start || ram_hole2_end > ram_hole2_start) {    /* A22 dram3: the holes are not memory */\n"
        "\t\tpmap_paddr_t holes = (ram_hole_end - ram_hole_start) + (ram_hole2_end - ram_hole2_start);\n\n"
        "\t\tsane_size -= holes;\n\t\tmax_mem -= holes;\n\t\tmax_mem_actual -= holes;\n\t}\n", "sane size")
    return t

def vmres(t):
    if MARK in t: return t
    t, n = re.subn(r"extern pmap_paddr_t ram_hole_start, ram_hole_end;[^\n]*\n",
                   "extern bool ram_hole_page(pmap_paddr_t pa);      /* A22 dram3: arm_vm_init.c */\n", t)
    if n != 1: raise SystemExit("vm_resident extern not found")
    t = sub1(t, "\t\tif (ram_hole_end > ram_hole_start && ((pmap_paddr_t)phys_page << PAGE_SHIFT) >= ram_hole_start &&\n"
        "\t\t    ((pmap_paddr_t)phys_page << PAGE_SHIFT) < ram_hole_end) {\n",
        "\t\tif (ram_hole_page((pmap_paddr_t)phys_page << PAGE_SHIFT)) {\n", "hole page test")
    return t

FILES = [("mtkloader/mtkloader.c", loader, "A22 dram3"), (XNU + "/osfmk/arm64/arm_vm_init.c", vminit, MARK),
         (XNU + "/osfmk/vm/vm_resident.c", vmres, MARK)]

def main():
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    root = os.path.expanduser(sys.argv[1]); do = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    out = {}
    for rel, fn, _ in FILES:
        p = os.path.join(root, rel)
        if not os.path.isfile(p): raise SystemExit("MISSING " + rel)
        old = open(p, encoding="utf-8").read()
        if "ram_hole_start" not in old and "A22 dram2" not in old: raise SystemExit("a22-dram2.py is not applied: " + rel)
        new = fn(old)
        print(("changes    | " if new != old else "was there  | ") + rel)
        out[p] = (old, new)
    if not do:
        print("\nEverything fits. To apply it: python3 tools/a22-dram3.py %s apply" % sys.argv[1]); return
    for p, (old, new) in out.items():
        if new != old:
            if not os.path.exists(p + ".dram3.orig"): shutil.copy(p, p + ".dram3.orig")
            open(p, "w", encoding="utf-8").write(new)
    print("applied")
main()
