#!/usr/bin/env python3
"""
a22-dram4.py -- more DRAM for iOS: 2370 MB instead of 1910 MB.

  python3 tools/a22-dram4.py ~/dev/hakios            # review only (changes nothing)
  python3 tools/a22-dram4.py ~/dev/hakios apply      # apply (backups *.dram4.orig)

Needs a22-dram3.py already applied. XNU's span now starts at 0x80000000 (Android has 0x80000000-0xdfffffff mostly as
plain "System RAM", logs/a22-dram-map.txt), with these holes the kernel does not use (MB, rounded so that no usable RAM
is risked and every hole is whole MB):
  0x8a000000-0x8ca00000   (firmware carve-outs)        0x8e000000-0x8e200000   0x8f000000-0x8f400000
  0x9fc00000-0xa0000000   0xb9600000-0xc0000000   0xe0000000-0x100000000 (modem, DPM)
Usable: 160 + 22 + 14 + 264 + 406 + 512 + 992 = 2370 MB. The loader passes them in ONE short boot-arg
(ram_holes=8a0-8ca,8e0-8e2,8f0-8f4,9fc-a00,b96-c00,e00-1000: hex MB) because the boot-args line is nearly full.
The kernel takes up to 8 holes, sorts them, maps what lies between and keeps their pages in vm_pages as vmp_error.
Back out: build the loader with -DA22_DRAM3_ONLY (1910 MB), -DA22_DRAM2_ONLY (1504 MB) or -DA22_ONE_BANK (992 MB).
"""
import os, shutil, sys

XNU = "xnu-src/xnu-8796.121.2"
FLAGS = "ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT"
MARK = "A22 dram4"

def sub1(text, old, new, what):
    if text.count(old) != 1:
        raise SystemExit("NOT FOUND (%d times): %s" % (text.count(old), what))
    return text.replace(old, new)

def loader(t):
    if MARK in t: return t
    t = sub1(t, "#define XNU_PHYS_BASE       0xa0000000ULL\n#define XNU_MEM_SIZE        0x9e000000ULL\n#endif\n",
        "#ifdef A22_DRAM3_ONLY\n#define XNU_PHYS_BASE       0xa0000000ULL\n#define XNU_MEM_SIZE        0x9e000000ULL\n#else\n"
        "/* A22 dram4: 0x80000000..0x13dffffff is one span for XNU with six holes (ram_holes boot-arg below). Usable 2370 MB. */\n"
        "#define XNU_PHYS_BASE       0x80000000ULL\n#define XNU_MEM_SIZE        0xbe000000ULL\n#endif\n#endif\n", "loader base")
    t = sub1(t, '#else\n\t\tstatic const char hole[] = " ram_hole_start_mb=2966 ram_hole_end_mb=3072 ram_hole2_start_mb=3584 ram_hole2_end_mb=4096";\n',
        '#elif defined(A22_DRAM3_ONLY)\n\t\tstatic const char hole[] = " ram_hole_start_mb=2966 ram_hole_end_mb=3072 ram_hole2_start_mb=3584 ram_hole2_end_mb=4096";\n'
        '#else\n\t\t/* A22 dram4: the holes in hex MB: START-END, ... */\n'
        '\t\tstatic const char hole[] = " ram_holes=8a0-8ca,8e0-8e2,8f0-8f4,9fc-a00,b96-c00,e00-1000";\n', "loader hole args")
    return t

GLOBALS = '''#define RAM_HOLES_MAX 8         /* A22 dram4 */
pmap_paddr_t ram_hole_s[RAM_HOLES_MAX], ram_hole_e[RAM_HOLES_MAX];       /* sorted, [start, end) */
unsigned ram_hole_n;

static void
ram_hole_add(pmap_paddr_t s, pmap_paddr_t e)
{
	unsigned i;

	if (e <= s || ram_hole_n >= RAM_HOLES_MAX) {
		return;
	}
	for (i = ram_hole_n; i > 0 && ram_hole_s[i - 1] > s; i--) {
		ram_hole_s[i] = ram_hole_s[i - 1];
		ram_hole_e[i] = ram_hole_e[i - 1];
	}
	ram_hole_s[i] = s;
	ram_hole_e[i] = e;
	ram_hole_n++;
}

static int
ram_hole_hex(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

/* A22 dram4: is this physical address in one of the holes? (vm_resident.c) */
bool
ram_hole_page(pmap_paddr_t pa)
{
	for (unsigned i = 0; i < ram_hole_n; i++) {
		if (pa >= ram_hole_s[i] && pa < ram_hole_e[i]) {
			return true;
		}
	}
	return false;
}
'''

PARSE = '''\t\tuint32_t hole_start_mb = 0, hole_end_mb = 0;
\t\tchar holes[96] = { 0 };

\t\t/* the pairs of the dram2 and dram3 loaders */
\t\tif (PE_parse_boot_argn("ram_hole_start_mb", &hole_start_mb, sizeof(hole_start_mb)) &&
\t\t    PE_parse_boot_argn("ram_hole_end_mb", &hole_end_mb, sizeof(hole_end_mb))) {
\t\t\tram_hole_add((pmap_paddr_t)hole_start_mb << 20, (pmap_paddr_t)hole_end_mb << 20);
\t\t}
\t\tif (PE_parse_boot_argn("ram_hole2_start_mb", &hole_start_mb, sizeof(hole_start_mb)) &&
\t\t    PE_parse_boot_argn("ram_hole2_end_mb", &hole_end_mb, sizeof(hole_end_mb))) {
\t\t\tram_hole_add((pmap_paddr_t)hole_start_mb << 20, (pmap_paddr_t)hole_end_mb << 20);
\t\t}
\t\t/* ram_holes=START-END,START-END,...: hex MB (A22 dram4) */
\t\tif (PE_parse_boot_argn("ram_holes", holes, sizeof(holes))) {
\t\t\tconst char *p = holes;

\t\t\twhile (*p) {
\t\t\t\tuint64_t s = 0, e = 0;

\t\t\t\twhile (ram_hole_hex(*p) >= 0) {
\t\t\t\t\ts = s * 16 + (uint64_t)ram_hole_hex(*p++);
\t\t\t\t}
\t\t\t\tif (*p++ != '-') {
\t\t\t\t\tbreak;
\t\t\t\t}
\t\t\t\twhile (ram_hole_hex(*p) >= 0) {
\t\t\t\t\te = e * 16 + (uint64_t)ram_hole_hex(*p++);
\t\t\t\t}
\t\t\t\tram_hole_add((pmap_paddr_t)s << 20, (pmap_paddr_t)e << 20);
\t\t\t\tif (*p++ != ',') {
\t\t\t\t\tbreak;
\t\t\t\t}
\t\t\t}
\t\t}
'''

def vminit(t):
    if MARK in t: return t
    a = t.index("pmap_paddr_t ram_hole_start, ram_hole_end, ram_hole2_start, ram_hole2_end;")
    k = t.index("ram_hole_page(pmap_paddr_t pa)", a)
    b = t.index("}\n", k) + 2
    t = t[:a] + GLOBALS + t[b:]
    a = t.index("\t\tuint32_t hole_start_mb = 0, hole_end_mb = 0;\n")
    mk = "\t\t\tram_hole2_end = (pmap_paddr_t)hole_end_mb << 20;\n\t\t}\n"
    b = t.index(mk, a) + len(mk)
    t = t[:a] + PARSE + t[b:]
    a = t.index("\t/* A22 dram3: up to two holes (not memory) are left out of the physical map")
    b = t.index("\tassert((temp_ptov_table[ptov_index - 1].va", a)
    new = ("\t/* A22 dram4: the holes (not memory) are left out of the physical map; what lies between them is mapped */\n"
        "\t{\n\t\tpmap_paddr_t from = args->topOfKernelData;\n\t\tunsigned kept = 0;\n\n"
        "\t\tfor (unsigned h = 0; h < ram_hole_n; h++) {\n"
        "\t\t\tpmap_paddr_t hs = ram_hole_s[h], he = ram_hole_e[h];\n\n"
        "\t\t\tif (hs < from || he > real_avail_end || (hs & PAGE_MASK) || (he & PAGE_MASK)) {\n"
        "\t\t\t\tcontinue;       /* does not fit, or out of order: ignored */\n\t\t\t}\n"
        "\t\t\tif (hs > from) {\n"
        "\t\t\t\tarm_vm_physmap_slide(temp_ptov_table, (from - gPhysBase + gVirtBase), hs - from, AP_RWNA,\n"
        "\t\t\t\t    %s);\n\t\t\t}\n"
        "\t\t\tfrom = he;\n\t\t\tram_hole_s[kept] = hs;\n\t\t\tram_hole_e[kept] = he;\n\t\t\tkept++;\n\t\t}\n"
        "\t\tram_hole_n = kept;\n"
        "\t\tif (real_avail_end > from) {\n"
        "\t\t\tarm_vm_physmap_slide(temp_ptov_table, (from - gPhysBase + gVirtBase), real_avail_end - from, AP_RWNA,\n"
        "\t\t\t    %s);       /* rest of physmem */\n\t\t}\n\t}\n\n") % (FLAGS, FLAGS)
    t = t[:a] + new + t[b:]
    t = sub1(t, "\tif (ram_hole_end > ram_hole_start || ram_hole2_end > ram_hole2_start) {    /* A22 dram3: the holes are not memory */\n"
        "\t\tpmap_paddr_t holes = (ram_hole_end - ram_hole_start) + (ram_hole2_end - ram_hole2_start);\n\n",
        "\tif (ram_hole_n > 0) {    /* A22 dram4: the holes are not memory */\n\t\tpmap_paddr_t holes = 0;\n\n"
        "\t\tfor (unsigned h = 0; h < ram_hole_n; h++) {\n\t\t\tholes += ram_hole_e[h] - ram_hole_s[h];\n\t\t}\n", "sane size")
    return t

FILES = [("mtkloader/mtkloader.c", loader), (XNU + "/osfmk/arm64/arm_vm_init.c", vminit)]

def main():
    if len(sys.argv) < 2: raise SystemExit(__doc__)
    root = os.path.expanduser(sys.argv[1]); do = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    out = {}
    for rel, fn in FILES:
        p = os.path.join(root, rel)
        if not os.path.isfile(p): raise SystemExit("MISSING " + rel)
        old = open(p, encoding="utf-8").read()
        if "A22 dram3" not in old and MARK not in old: raise SystemExit("a22-dram3.py is not applied: " + rel)
        new = fn(old)
        print(("changes    | " if new != old else "was there  | ") + rel)
        out[p] = (old, new)
    if not do:
        print("\nEverything fits. To apply it: python3 tools/a22-dram4.py %s apply" % sys.argv[1]); return
    for p, (old, new) in out.items():
        if new != old:
            if not os.path.exists(p + ".dram4.orig"): shutil.copy(p, p + ".dram4.orig")
            open(p, "w", encoding="utf-8").write(new)
    print("applied")
main()
