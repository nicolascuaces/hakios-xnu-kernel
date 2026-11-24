#!/usr/bin/env python3
"""
a22-emmcstat.py -- what does iOS really ask of the eMMC? Counters printed in the log (diagnostic only).

  python3 tools/a22-emmcstat.py ~/dev/hakios            # review only (changes nothing)
  python3 tools/a22-emmcstat.py ~/dev/hakios apply      # apply (backup redwood_emmc.c.stat.orig)

Boot-arg emmc_stat=SECONDS (e.g. 15) prints, every that many seconds, one line for the interval:
  emmc stat: 15 s: 842 reads, 31 MB, busy 1210 ms (8 %), <=4K 120 <=8K 40 <=16K 300 <=32K 90 <=64K 150 <=128K 80 <=256K 40 >256K 22
"busy" is the time spent inside the reads (the CPU spins there, see ksamp); the sizes are those of each command
(a request of the file system is cut into commands of up to 512 KB). A second line, "emmc mem:", gives the memory of the
same moment (free, active, inactive, file-backed, anonymous, wired, and what was fed to the compressor, in MB).
Running the script again on a tree that has the first version adds only the memory line. Only counts; nothing is
read or written.
"""
import os
import shutil
import sys

tree = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(__doc__)
do_apply = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
path = os.path.join(tree, "xnu-src", "xnu-8796.121.2", "bsd", "dev", "redwood_emmc.c")

INC_OLD = "#include <kern/locks.h>\n"
INC_NEW = INC_OLD + "#include <kern/startup.h>\n#include <kern/thread_call.h>\n"

STRAT_OLD = "static void\nemmc_strategy(buf_t bp)\n"
STAT = r'''/* ---- emmc_stat=SECONDS (tools/a22-emmcstat.py): a line every so many seconds with what was read ---- */

static uint64_t es_size[8], es_ns, es_cmds, es_bytes;
static uint64_t es_p_size[8], es_p_ns, es_p_cmds, es_p_bytes;
static uint32_t es_every;
static thread_call_t es_call;

static void
es_count(uint32_t bytes, uint64_t ns)
{
	int b = bytes <= 4096 ? 0 : bytes <= 8192 ? 1 : bytes <= 16384 ? 2 : bytes <= 32768 ? 3 :
	    bytes <= 65536 ? 4 : bytes <= 131072 ? 5 : bytes <= 262144 ? 6 : 7;

	es_size[b]++;
	es_ns += ns;
	es_cmds++;
	es_bytes += bytes;
}

static void
es_fire(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t d[8], dn = es_ns - es_p_ns, dc = es_cmds - es_p_cmds, db = es_bytes - es_p_bytes, deadline;

	for (int i = 0; i < 8; i++) {
		d[i] = es_size[i] - es_p_size[i];
		es_p_size[i] = es_size[i];
	}
	es_p_ns = es_ns;
	es_p_cmds = es_cmds;
	es_p_bytes = es_bytes;
	printf("emmc stat: %u s: %llu reads, %llu MB, busy %llu ms (%llu %%), <=4K %llu <=8K %llu <=16K %llu <=32K %llu "
	    "<=64K %llu <=128K %llu <=256K %llu >256K %llu\n", es_every, dc, db >> 20, dn / 1000000,
	    dn / 10000000 / es_every, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
	clock_interval_to_deadline(es_every, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(es_call, deadline);
}

static void
es_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("emmc_stat", &es_every, sizeof(es_every)) || es_every == 0) {
		return;
	}
	es_call = thread_call_allocate(es_fire, NULL);
	clock_interval_to_deadline(es_every, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(es_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, es_init);

'''

READ_OLD = "\t\tif (!read_any(emmc_lba + block + done / 512, blocks, data + done, bytes)) {\n\t\t\terror = EIO;\n"
READ_NEW = "\t\tuint64_t es_t0 = mach_absolute_time(), es_dt;\n\n" + READ_OLD
COUNT_OLD = "\t\temmc_reads++;\n\t\temmc_read_blocks += blocks;\n"
COUNT_NEW = COUNT_OLD + "\t\tabsolutetime_to_nanoseconds(mach_absolute_time() - es_t0, &es_dt);\n\t\tes_count(bytes, es_dt);\n"

MEM_DECL_OLD = "static uint32_t es_every;\n"
MEM_DECL_NEW = MEM_DECL_OLD + """extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count, vm_page_speculative_count;
extern unsigned int vm_page_pageable_external_count, vm_page_pageable_internal_count, vm_page_wire_count;
extern uint32_t c_segment_pages_compressed;
#define ES_MB(pages) ((uint64_t)(pages) * PAGE_SIZE >> 20)
"""
MEM_PRINT_OLD = "d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);\n"
MEM_PRINT_NEW = MEM_PRINT_OLD + """	printf("emmc mem: free %llu MB, active %llu, inactive %llu, speculative %llu, file %llu, anon %llu, wired %llu, "
	    "fed to the compressor %llu\\n", ES_MB(vm_page_free_count), ES_MB(vm_page_active_count),
	    ES_MB(vm_page_inactive_count), ES_MB(vm_page_speculative_count), ES_MB(vm_page_pageable_external_count),
	    ES_MB(vm_page_pageable_internal_count), ES_MB(vm_page_wire_count), ES_MB(c_segment_pages_compressed));
"""

src = open(path).read()
out = src
if "es_init" not in out:
    for old, new in [(INC_OLD, INC_NEW), (STRAT_OLD, STAT + STRAT_OLD), (READ_OLD, READ_NEW), (COUNT_OLD, COUNT_NEW)]:
        if out.count(old) != 1:
            sys.exit("NOT FOUND (%d matches): %r" % (out.count(old), old.strip().splitlines()[0]))
        out = out.replace(old, new)
if "emmc mem:" not in out:
    for old, new in [(MEM_DECL_OLD, MEM_DECL_NEW), (MEM_PRINT_OLD, MEM_PRINT_NEW)]:
        if out.count(old) != 1:
            sys.exit("NOT FOUND (%d matches): %r" % (out.count(old), old.strip().splitlines()[0]))
        out = out.replace(old, new)
if out == src:
    print("already applied:", path)
    sys.exit(0)
print(("patching: " if do_apply else "would patch: ") + path)
if do_apply:
    shutil.copy(path, path + ".stat2.orig")
    open(path, "w").write(out)
else:
    print("review only; add 'apply' to apply.")
