#!/usr/bin/env python3
"""
a22-emmcbench.py -- read-only eMMC speed test, printed in the boot log.

  python3 tools/a22-emmcbench.py ~/dev/hakios            # review only (changes nothing)
  python3 tools/a22-emmcbench.py ~/dev/hakios apply      # apply (backup redwood_emmc.c.bench.orig)

Boot-arg emmc_bench=1 runs it once at boot, right after the DMA test (nothing happens without it). It only READS
xnusys by DMA, in commands of 4, 16, 64 and 512 KB, each one sequential and at pseudo-random places, and prints
  emmc bench: 16 KB seq: 71.3 MB/s, 224 us a command; random: 12.4 MB/s, 1260 us a command
plus the raw MSDC_CFG / MSDC_IOCON registers (bus clock, as LK left it). The time includes the cache work of each
command but not the copy to the caller. It adds about 2 s to the boot.
"""
import os
import shutil
import sys

tree = os.path.expanduser(sys.argv[1]) if len(sys.argv) > 1 else sys.exit(__doc__)
do_apply = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
path = os.path.join(tree, "xnu-src", "xnu-8796.121.2", "bsd", "dev", "redwood_emmc.c")

CALL_OLD = "\t\tdma_try(probe);\n\t\temmc_wtest(probe);\n"
CALL_NEW = CALL_OLD + "\t\temmc_bench();\n"

ANCHOR = "/* ---- /dev/remmc_var: the store of /private/var"

BENCH = r'''/* ---- emmc_bench=1 (tools/a22-emmcbench.py): how fast does it read? Only reads. ---- */

static uint64_t
bench_run(uint32_t blocks, uint32_t count, bool random, uint64_t *bytes)
{
	uint64_t t0, ns, seq_at = (emmc_blocks / 8) & ~1023ull, x = 0x9E3779B97F4A7C15ull;

	t0 = mach_absolute_time();
	for (uint32_t i = 0; i < count; i++) {
		uint64_t off;

		if (random) {
			x = x * 6364136223846793005ull + 1442695040888963407ull;
			off = ((x >> 20) % (emmc_blocks - 1024)) & ~7ull;
		} else {
			off = seq_at + (uint64_t)i * blocks;
		}
		if (off + blocks > emmc_blocks || !read_blocks_dma(emmc_lba + off, blocks)) {
			return 0;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	*bytes = (uint64_t)count * blocks * 512;
	return ns;
}

static void
emmc_bench(void)
{
	static const uint32_t size[4] = { 8, 32, 128, 1024 };       /* blocks: 4, 16, 64, 512 KB */
	uint32_t want = 0;

	if (!dma_on || !PE_parse_boot_argn("emmc_bench", &want, sizeof(want)) || want == 0) {
		return;
	}
	printf("emmc bench: MSDC_CFG %#x, MSDC_IOCON %#x\n", rd(MSDC_CFG), rd(0x04));
	for (int s = 0; s < 4; s++) {
		uint32_t blocks = size[s];
		uint32_t count = (blocks >= 128) ? 128 : 512;
		uint64_t by = 0, ns_seq, ns_rnd;
		uint64_t by2 = 0;

		ns_seq = bench_run(blocks, count, false, &by);
		ns_rnd = bench_run(blocks, count / 2, true, &by2);
		if (ns_seq == 0 || ns_rnd == 0) {
			printf("emmc bench: %u KB: a read failed\n", blocks / 2);
			return;
		}
		printf("emmc bench: %u KB seq: %llu MB/s x10, %llu us a command; random: %llu MB/s x10, %llu us a command\n",
		    blocks / 2,
		    (by * 10 * 1000 / ns_seq) >> 20 << 0 ? (by * 10 * 1000ull / ns_seq) * 1000 / 1048576 : 0,
		    ns_seq / count / 1000,
		    (by2 * 10 * 1000ull / ns_rnd) * 1000 / 1048576,
		    ns_rnd / (count / 2) / 1000);
	}
	printf("emmc bench: end\n");
}

'''

src = open(path).read()
if "emmc_bench" in src:
    print("already applied:", path)
    sys.exit(0)
for old in (CALL_OLD, ANCHOR):
    if src.count(old) != 1:
        sys.exit("NOT FOUND (%d matches): %r" % (src.count(old), old.strip().splitlines()[0]))
out = src.replace(CALL_OLD, CALL_NEW).replace(ANCHOR, BENCH + ANCHOR)
print(("patching: " if do_apply else "would patch: ") + path)
if do_apply:
    shutil.copy(path, path + ".bench.orig")
    open(path, "w").write(out)
else:
    print("review only; add 'apply' to apply.")
