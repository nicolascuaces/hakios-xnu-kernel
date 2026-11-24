#!/usr/bin/env python3
"""
a22-dram2.py -- the second DRAM rank for iOS: 1504 MB instead of 992 MB.

  python3 tools/a22-dram2.py ~/dev/hakios            # review only (changes nothing)
  python3 tools/a22-dram2.py ~/dev/hakios apply      # apply (backups *.dram2.orig)

What the A22's 4 GB look like (from /proc/iomem and the device tree, tools/a22-dram-map.sh):
  0x40000000-0xbfffffff  rank 0 (2 GB): Android, LK, TEE, ATF, modem...
  0xc0000000-0xdfffffff  512 MB of clean System RAM                     <- NEW for iOS
  0xe0000000-0xffffffff  modem (ccci md_mem_usage), DPM: NOT ours      <- the "ram hole"
  0x100000000-0x13dffffff  992 MB that iOS already used
XNU wants one contiguous range, so it is given 0xc0000000..0x13dffffff (2016 MB) and told where the hole is:

  mtkloader/mtkloader.c           XNU_PHYS_BASE 0xc0000000, XNU_MEM_SIZE 0x7e000000, and the boot-args
                                  ram_hole_start_mb=3584 ram_hole_end_mb=4096 appended by the loader
                                  (-DA22_ONE_BANK at build time goes back to the old single bank)
  osfmk/arm64/arm_vm_init.c       reads the hole; the kernel's physical map (ptov table) is built in two
                                  pieces around it, so the hole is NOT mapped (no speculative access to the
                                  modem's memory); max_mem / sane_size lose the hole
  osfmk/vm/vm_resident.c          pmap_startup(): on arm64 the vm_pages array gives each entry its page number by
                                  position (index + first), so the hole cannot be skipped: its entries are
                                  created and marked "error" (never released to the free list, like a retired
                                  page). About 2.6 MB of vm_page structures are spent on them.
  osfmk/arm/pmap/pmap.c           (first version of this script changed it; running this one undoes that)

Without the boot-args the kernel behaves as before. Read the notes in the answer before testing.
"""
import os
import shutil
import sys

tree = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(__doc__)
do_apply = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
xnu = os.path.join(tree, "xnu-src", "xnu-8796.121.2", "osfmk")
FILES = {
    "loader": os.path.join(tree, "mtkloader", "mtkloader.c"),
    "vm": os.path.join(xnu, "arm64", "arm_vm_init.c"),
    "pmap": os.path.join(xnu, "arm", "pmap", "pmap.c"),
    "vmres": os.path.join(xnu, "vm", "vm_resident.c"),
}

LOADER = [
    (
        "#define XNU_PHYS_BASE       0x100000000ULL  /* A22: between 0xe0000000 and 0xffff0000 there are the modem and DPM */\n"
        "#define XNU_MEM_SIZE        0x3e000000ULL       /* A22: 992 MB clean, below dramc-rk1 (0x13ffff000) */\n",
        "#ifdef A22_ONE_BANK\n"
        "#define XNU_PHYS_BASE       0x100000000ULL  /* A22: between 0xe0000000 and 0xffff0000 there are the modem and DPM */\n"
        "#define XNU_MEM_SIZE        0x3e000000ULL       /* A22: 992 MB clean, below dramc-rk1 (0x13ffff000) */\n"
        "#else\n"
        "/* A22 dram2: 0xc0000000..0x13dffffff is one span for XNU; 0xe0000000..0xffffffff (modem, DPM) is a hole\n"
        " * the kernel is told about (ram_hole_* boot-args below). Usable: 512 MB + 992 MB = 1504 MB. */\n"
        "#define XNU_PHYS_BASE       0xc0000000ULL\n"
        "#define XNU_MEM_SIZE        0x7e000000ULL\n"
        "#endif\n",
    ),
    (
        "\ta22_append_epoch(ba->commandLine, sizeof(ba->commandLine));   /* A22: time from the RTC */\n",
        "\ta22_append_epoch(ba->commandLine, sizeof(ba->commandLine));   /* A22: time from the RTC */\n"
        "#ifndef A22_ONE_BANK\n"
        "\t{\n"
        "\t\t/* A22 dram2: where the modem's memory is, in MB: 0xe0000000 .. 0x100000000 */\n"
        "\t\tstatic const char hole[] = \" ram_hole_start_mb=3584 ram_hole_end_mb=4096\";\n"
        "\t\tuint32_t n = 0;\n\n"
        "\t\twhile (ba->commandLine[n]) {\n"
        "\t\t\tn++;\n"
        "\t\t}\n"
        "\t\tif (n + sizeof(hole) <= sizeof(ba->commandLine)) {\n"
        "\t\t\tmemcpy(ba->commandLine + n, hole, sizeof(hole));\n"
        "\t\t}\n"
        "\t}\n"
        "#endif\n",
    ),
]

VM = [
    (
        "SECURITY_READ_ONLY_LATE(pmap_paddr_t)    real_avail_end;\n",
        "SECURITY_READ_ONLY_LATE(pmap_paddr_t)    real_avail_end;\n"
        "/* A22 dram2: a hole inside [gPhysBase, real_avail_end) that is not ours (modem); 0, 0 = none */\n"
        "pmap_paddr_t ram_hole_start, ram_hole_end;\n",
    ),
    (
        "\tgPhysBase = args->physBase;\n",
        "\tgPhysBase = args->physBase;\n"
        "\t{\n"
        "\t\tuint32_t hole_start_mb = 0, hole_end_mb = 0;\n\n"
        "\t\tif (PE_parse_boot_argn(\"ram_hole_start_mb\", &hole_start_mb, sizeof(hole_start_mb)) &&\n"
        "\t\t    PE_parse_boot_argn(\"ram_hole_end_mb\", &hole_end_mb, sizeof(hole_end_mb)) && hole_end_mb > hole_start_mb) {\n"
        "\t\t\tram_hole_start = (pmap_paddr_t)hole_start_mb << 20;\n"
        "\t\t\tram_hole_end = (pmap_paddr_t)hole_end_mb << 20;\n"
        "\t\t}\n"
        "\t}\n",
    ),
    (
        "\tarm_vm_physmap_slide(temp_ptov_table, (args->topOfKernelData - gPhysBase + gVirtBase),\n"
        "\t    real_avail_end - args->topOfKernelData, AP_RWNA, ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT); // rest of physmem\n",
        "\t/* A22 dram2: the hole (modem memory) is left out of the physical map, in two pieces around it */\n"
        "\tif (ram_hole_end > ram_hole_start && (ram_hole_start < args->topOfKernelData || ram_hole_end > real_avail_end ||\n"
        "\t    (ram_hole_start & PAGE_MASK) || (ram_hole_end & PAGE_MASK))) {\n"
        "\t\tram_hole_start = ram_hole_end = 0;      /* does not fit: ignored */\n"
        "\t}\n"
        "\tif (ram_hole_end > ram_hole_start) {\n"
        "\t\tarm_vm_physmap_slide(temp_ptov_table, (args->topOfKernelData - gPhysBase + gVirtBase),\n"
        "\t\t    ram_hole_start - args->topOfKernelData, AP_RWNA, ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT);\n"
        "\t\tif (real_avail_end > ram_hole_end) {\n"
        "\t\t\tarm_vm_physmap_slide(temp_ptov_table, (ram_hole_end - gPhysBase + gVirtBase),\n"
        "\t\t\t    real_avail_end - ram_hole_end, AP_RWNA, ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT);\n"
        "\t\t}\n"
        "\t} else {\n"
        "\t\tarm_vm_physmap_slide(temp_ptov_table, (args->topOfKernelData - gPhysBase + gVirtBase),\n"
        "\t\t    real_avail_end - args->topOfKernelData, AP_RWNA, ARM64_GRANULE_ALLOW_BLOCK | ARM64_GRANULE_ALLOW_HINT); // rest of physmem\n"
        "\t}\n",
    ),
    (
        "\tsane_size = mem_size - (avail_start - gPhysBase);\n\tmax_mem = mem_size;\n",
        "\tsane_size = mem_size - (avail_start - gPhysBase);\n\tmax_mem = mem_size;\n"
        "\tif (ram_hole_end > ram_hole_start) {    /* A22 dram2: the hole is not memory */\n"
        "\t\tsane_size -= (ram_hole_end - ram_hole_start);\n"
        "\t\tmax_mem -= (ram_hole_end - ram_hole_start);\n"
        "\t\tmax_mem_actual -= (ram_hole_end - ram_hole_start);\n"
        "\t}\n",
    ),
]

PMAP = [
    (
        "static unsigned int avail_page_count = 0;\n",
        "static unsigned int avail_page_count = 0;\n"
        "extern pmap_paddr_t ram_hole_start, ram_hole_end;       /* A22 dram2: arm_vm_init.c */\n",
    ),
    (
        "\tavail_page_count = atop(end - first);\n",
        "\tavail_page_count = atop(end - first);\n"
        "\tif (ram_hole_end > ram_hole_start && ram_hole_start >= first && ram_hole_end <= end) {\n"
        "\t\tavail_page_count -= (unsigned int)atop(ram_hole_end - ram_hole_start);     /* A22 dram2 */\n"
        "\t}\n",
    ),
    (
        "\tif (first_avail != avail_end) {\n\t\t*pnum = (ppnum_t)atop(first_avail);\n",
        "\tif (ram_hole_end > ram_hole_start && first_avail >= ram_hole_start && first_avail < ram_hole_end) {\n"
        "\t\tfirst_avail = ram_hole_end;     /* A22 dram2: not ours */\n"
        "\t}\n"
        "\tif (first_avail != avail_end) {\n\t\t*pnum = (ppnum_t)atop(first_avail);\n",
    ),
]

VMRES = [
    (
        "unsigned int    vm_pages_count = 0;\n",
        "unsigned int    vm_pages_count = 0;\nextern pmap_paddr_t ram_hole_start, ram_hole_end;      /* A22 dram2: arm_vm_init.c */\n",
    ),
    (
        "\t\t++vm_pages_count;\n\t\tvm_page_init(&vm_pages[i], phys_page, FALSE);\n",
        "\t\t++vm_pages_count;\n\t\tvm_page_init(&vm_pages[i], phys_page, FALSE);\n"
        "\t\tif (ram_hole_end > ram_hole_start && ((pmap_paddr_t)phys_page << PAGE_SHIFT) >= ram_hole_start &&\n"
        "\t\t    ((pmap_paddr_t)phys_page << PAGE_SHIFT) < ram_hole_end) {\n"
        "\t\t\t/* A22 dram2: modem memory, not ours: in the array (the page number is its position) but never freed */\n"
        "\t\t\tvm_pages[i].vmp_error = TRUE;\n"
        "\t\t\tvm_pages[i].vmp_unusual = TRUE;\n"
        "\t\t\tcontinue;\n"
        "\t\t}\n",
    ),
]

# the first version also edited pmap.c: put it back (the new text -> the old)
PMAP_UNDO = [(new, old) for (old, new) in PMAP]

PLAN = [("loader", LOADER, "A22_ONE_BANK"), ("vm", VM, "ram_hole_start"), ("vmres", VMRES, "ram_hole_start")]

work = []
ok = True
for key, edits, marker in PLAN:
    path = FILES[key]
    src = open(path).read()
    if marker in src:
        print("already applied:", path)
        continue
    out = src
    for old, new in edits:
        n = out.count(old)
        if n != 1:
            print("NOT FOUND (%d matches) in %s: %r" % (n, path, old.strip().splitlines()[0][:80]))
            ok = False
            break
        out = out.replace(old, new)
    else:
        work.append((path, out))
psrc = open(FILES["pmap"]).read()
if "ram_hole_start" in psrc:
    pout = psrc
    for old, new in PMAP_UNDO:
        if pout.count(old) != 1:
            print("NOT FOUND (%d matches) in %s: %r" % (pout.count(old), FILES["pmap"], old.strip().splitlines()[0][:80]))
            ok = False
            break
        pout = pout.replace(old, new)
    else:
        work.append((FILES["pmap"], pout))
if not ok:
    sys.exit("nothing was changed")
for path, out in work:
    print(("patching: " if do_apply else "would patch: ") + path)
    if do_apply:
        if not os.path.exists(path + ".dram2.orig"):
            shutil.copy(path, path + ".dram2.orig")
        open(path, "w").write(out)
if not do_apply and work:
    print("review only; add 'apply' to apply.")
