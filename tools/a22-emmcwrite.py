#!/usr/bin/env python3
"""
A22: writes to the eMMC, only into the tail of "xnusys" past the APFS containers (the future store of
/private/var). This step is the self-test only: with boot-arg emmc_wtest=1 the kernel writes 1 MB of
a pattern at the start of the write tail, reads it back by DMA and by PIO, and logs whether writing
works. Without that boot-arg nothing is written, and the device nodes stay read only.

The guard: a write is refused unless it lies wholly inside the last 256 MB of the partition and that
tail starts at least 128 MB after the end of the APFS containers. The preloader, lk, seccfg, efs, nvdata
and PARTITION_CONFIG are never touched: the card block comes from the GPT entry of xnusys.

  python3 tools/a22-emmcwrite.py ~/dev/hakios          -> only checks
  python3 tools/a22-emmcwrite.py ~/dev/hakios apply    -> applies (saves redwood_emmc.c.write.orig the first time)

Log lines to look for: "emmc: writes allowed from card block ..." and "emmc: write test: 0 words differ
read back by DMA, 0 by PIO -- WRITING WORKS".
"""
import os, shutil, sys

DEST = "xnu-src/xnu-8796.121.2/bsd/dev/redwood_emmc.c"
MARK = "emmc_wtest"

BLOCK = r'''/* ---- writes: only into the tail of the partition, past the APFS containers (the store of /private/var) ---- */

#define EMMC_RCA            1u                      /* an eMMC card's address, as Linux gives it */
#define SDC_RESP0           0x40
#define CMD_WRITE           (1u << 13)
#define EMMC_W_BYTES        (256ull << 20)          /* the tail of the partition that may be written */
#define EMMC_W_MARGIN       (128ull << 20)          /* kept clear between the APFS containers and that tail */

static uint64_t emmc_w_first;                       /* the first card block that may be written; 0 = none */
static uint64_t emmc_writes, emmc_write_blocks;

/* may these card blocks be written? Only inside the tail, and never outside the partition */
static bool
write_range_ok(uint64_t lba, uint32_t blocks)
{
	return emmc_w_first != 0 && blocks != 0 && lba >= emmc_w_first &&
	       lba + blocks <= emmc_lba + emmc_part_blocks;
}

static bool
write_init(void)
{
	uint64_t tail;

	emmc_w_first = 0;
	if (emmc_part_blocks * 512 <= EMMC_W_BYTES) {
		return false;
	}
	tail = emmc_lba + emmc_part_blocks - EMMC_W_BYTES / 512;
	if ((tail - emmc_lba) * 512 < emmc_blocks * 512 + EMMC_W_MARGIN) {
		printf("emmc: no room for writes: the APFS containers reach %llu MB of %llu MB\n", (emmc_blocks * 512) >> 20,
		    (emmc_part_blocks * 512) >> 20);
		return false;
	}
	emmc_w_first = tail;
	return true;
}

/* CMD13 until the card says it is in the transfer state and ready for data */
static bool
card_ready(void)
{
	for (uint32_t tries = 0; tries < 2000; tries++) {
		uint32_t resp;

		if (!wait_idle()) {
			return false;
		}
		wr(SDC_ARG, EMMC_RCA << 16);
		wr(SDC_CMD, 13 | CMD_RSP_R1);
		if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "status")) {
			return false;
		}
		resp = rd(SDC_RESP0);
		if ((resp & (1u << 8)) && ((resp >> 9) & 0xf) == 4) {
			return true;
		}
		for (uint32_t k = 0; k < 5000; k++) {
			(void)rd(SDC_STS);                  /* about 5 ms */
		}
	}
	return fail("card still busy", rd(SDC_RESP0));
}

/* blocks of 512 bytes from dma_buf to the card's block lba, by DMA */
static bool
write_blocks_dma(uint64_t lba, uint32_t blocks)
{
	uint64_t gpd_pa = dma_area_pa, bd_pa = dma_area_pa + 256, buf_pa = dma_area_pa + 4096;
	uint32_t len = blocks * 512, status = 0;
	bool ok;

	if (!dma_on || blocks > DMA_MAX_BLOCKS || !write_range_ok(lba, blocks)) {
		return fail("write refused", (uint32_t)lba);
	}
	if (!wait_idle()) {
		return false;
	}
	memset(dma_area, 0, 4096);
	dma_bd[0].info = 1u | (uint32_t)(((buf_pa >> 32) & 0xf) << 28);
	dma_bd[0].ptr = (uint32_t)buf_pa;
	dma_bd[0].len = len & 0xffffffu;
	dma_bd[0].info |= desc_sum(&dma_bd[0]) << 8;
	dma_gpd[0].info = 1u | 2u | (uint32_t)((((gpd_pa + sizeof(struct gpd)) >> 32) & 0xf) << 24) |
	    (uint32_t)(((bd_pa >> 32) & 0xf) << 28);
	dma_gpd[0].next = (uint32_t)(gpd_pa + sizeof(struct gpd));
	dma_gpd[0].ptr = (uint32_t)bd_pa;
	dma_gpd[0].info |= desc_sum(&dma_gpd[0]) << 8;
	cache_flush(dma_area, 4096);
	cache_flush(dma_buf, len);                  /* the data is in the cache: the controller reads memory */

	wr(MSDC_CFG, rd(MSDC_CFG) & ~MSDC_CFG_PIO);
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
	wr(MSDC_DMA_CFG, rd(MSDC_DMA_CFG) | DMA_CFG_DECSEN);
	wr(MSDC_DMA_CTRL, (rd(MSDC_DMA_CTRL) & ~(DMA_CTRL_BURST | DMA_CTRL_MODE)) | DMA_BURST_64B | DMA_CTRL_MODE);
	wr(MSDC_PATCH_BIT2, rd(MSDC_PATCH_BIT2) | PB2_SUPPORT_64G);
	wr(DMA_SA_H4BIT, (rd(DMA_SA_H4BIT) & ~0xfu) | (uint32_t)((gpd_pa >> 32) & 0xf));
	wr(MSDC_DMA_SA, (uint32_t)gpd_pa);

	wr(SDC_BLK_NUM, blocks);
	wr(SDC_ARG, (uint32_t)lba);
	wr(SDC_CMD, (blocks == 1 ? 24 : 25) | CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) |
	    CMD_BLKLEN(512) | CMD_WRITE);
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "write command (dma)")) {
		return false;
	}
	wr(MSDC_DMA_CTRL, rd(MSDC_DMA_CTRL) | DMA_CTRL_START);
	ok = wait_int(INT_XFER_COMPL, INT_DMA_BAD, "write end (dma)");
	if (!ok) {
		status = emmc_failed_status;
	}
	wr(MSDC_DMA_CTRL, rd(MSDC_DMA_CTRL) | DMA_CTRL_STOP);
	for (uint32_t i = 0; i < 200000 && (rd(MSDC_DMA_CTRL) & DMA_CTRL_STOP); i++) {
	}
	for (uint32_t i = 0; i < 200000 && (rd(MSDC_DMA_CFG) & DMA_CFG_STS); i++) {
	}
	if (rd(MSDC_DMA_CFG) & DMA_CFG_STS) {
		ok = fail("dma did not stop", rd(MSDC_DMA_CFG));
	}
	if (blocks > 1 && !stop()) {
		ok = false;
	}
	if (ok) {
		ok = card_ready();                      /* the card programs the flash after the last block */
	}
	if (!ok && status) {
		emmc_failed_status = status;
	}
	if (ok) {
		emmc_writes++;
		emmc_write_blocks += blocks;
	}
	return ok;
}

/* a test pattern that tells every block and every word apart */
static uint32_t
test_word(uint64_t lba, uint32_t word)
{
	return (uint32_t)(lba + 1) * 2654435761u ^ word * 40503u ^ 0xa22c0de5u;
}

static void
test_fill(uint64_t lba, uint32_t blocks)
{
	uint32_t *p = (uint32_t *)(void *)dma_buf;

	for (uint32_t b = 0; b < blocks; b++) {
		for (uint32_t w = 0; w < 128; w++) {
			p[b * 128 + w] = test_word(lba + b, w);
		}
	}
}

static uint32_t
test_diff(const uint32_t *p, uint64_t lba, uint32_t blocks)         /* how many words differ */
{
	uint32_t bad = 0;

	for (uint32_t b = 0; b < blocks; b++) {
		for (uint32_t w = 0; w < 128; w++) {
			bad += p[b * 128 + w] != test_word(lba + b, w);
		}
	}
	return bad;
}

/*
 * Boot-arg emmc_wtest=1: the first megabyte of the write tail gets the pattern and is read back by DMA and
 * by PIO. Nothing else is written, and the APFS containers are never in range.
 */
static void
emmc_wtest(uint32_t *pio)
{
	uint32_t want = 0;
	uint64_t at, t0, ns;
	uint32_t bad_dma = 0, bad_pio = 0;

	if (!PE_parse_boot_argn("emmc_wtest", &want, sizeof(want)) || want == 0) {
		return;
	}
	if (!dma_on || !write_init()) {
		printf("emmc: write test not run (%s)\n", dma_on ? "no room" : "DMA is off");
		return;
	}
	printf("emmc: writes allowed from card block %llu (%llu MB into the partition), %llu MB of tail\n", emmc_w_first,
	    ((emmc_w_first - emmc_lba) * 512) >> 20, EMMC_W_BYTES >> 20);
	if (write_range_ok(emmc_w_first - 1, 1) || write_range_ok(emmc_lba, 1) ||
	    write_range_ok(emmc_lba + emmc_part_blocks, 1)) {
		printf("emmc: THE WRITE GUARD LETS BLOCKS OUTSIDE THE TAIL THROUGH; not writing\n");
		emmc_w_first = 0;
		return;
	}
	t0 = mach_absolute_time();
	for (at = 0; at < 2048; at += DMA_MAX_BLOCKS) {
		test_fill(emmc_w_first + at, DMA_MAX_BLOCKS);
		if (!write_blocks_dma(emmc_w_first + at, DMA_MAX_BLOCKS)) {
			printf("emmc: write at tail block %llu: %s (%#x); no more writes\n", at, emmc_failed, emmc_failed_status);
			pio_mode();
			(void)stop();
			emmc_w_first = 0;
			return;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	printf("emmc: 1 MB written by DMA in %llu ms\n", ns / 1000000);
	for (at = 0; at < 2048; at += DMA_MAX_BLOCKS) {
		memset(dma_buf, 0, DMA_MAX_BLOCKS * 512);
		if (!read_blocks_dma(emmc_w_first + at, DMA_MAX_BLOCKS)) {
			printf("emmc: read back by DMA at %llu: %s (%#x)\n", at, emmc_failed, emmc_failed_status);
			bad_dma = ~0u;
			break;
		}
		bad_dma += test_diff((const uint32_t *)(void *)dma_buf, emmc_w_first + at, DMA_MAX_BLOCKS);
	}
	for (at = 0; at < 2048 && bad_pio != ~0u; at += EMMC_MAX_BLOCKS) {
		if (!read_retrying(emmc_w_first + at, EMMC_MAX_BLOCKS, pio)) {
			bad_pio = ~0u;
			break;
		}
		bad_pio += test_diff(pio, emmc_w_first + at, EMMC_MAX_BLOCKS);
	}
	printf("emmc: write test: %u words differ read back by DMA, %u by PIO%s\n", bad_dma, bad_pio,
	    (bad_dma == 0 && bad_pio == 0) ? " -- WRITING WORKS" : " -- WRITING DOES NOT WORK");
	if (bad_dma != 0 || bad_pio != 0) {
		emmc_w_first = 0;
	}
}

'''

CHANGES = [
    ("static uint64_t emmc_lba, emmc_blocks;      /* the partition: its first block, and the image's length in blocks */\n",
     "static uint64_t emmc_lba, emmc_blocks;      /* the partition: its first block, and the image's length in blocks */\n"
     "static uint64_t emmc_part_blocks;           /* the whole partition, in blocks */\n"),
    ("\temmc_blocks = at / 512;\n",
     "\temmc_part_blocks = emmc_blocks;\n\temmc_blocks = at / 512;\n"),
    ("/* the first 4 MB by DMA and by PIO: on only if every byte agrees */\n",
     BLOCK + "/* the first 4 MB by DMA and by PIO: on only if every byte agrees */\n"),
    ("\t\tdma_try(probe);\n",
     "\t\tdma_try(probe);\n\t\temmc_wtest(probe);\n"),
]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = os.path.expanduser(sys.argv[1])
    apply = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    path = os.path.join(root, DEST)
    if not os.path.isfile(path):
        raise SystemExit("MISSING: " + path)
    text = open(path, encoding="utf-8").read()
    if MARK in text:
        print("already applied:", DEST)
        return
    bad = 0
    for old, new in CHANGES:
        n = text.count(old)
        if n != 1:
            print("NOT FOUND (%d times) |" % n, old.strip().splitlines()[0][:70])
            bad += 1
            continue
        text = text.replace(old, new)
        print("changes    |", new.strip().splitlines()[0][:70])
    if bad:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % bad)
    if not apply:
        print("\nEverything fits. To apply it: python3 a22-emmcwrite.py %s apply" % sys.argv[1])
        return
    if not os.path.exists(path + ".write.orig"):
        shutil.copy2(path, path + ".write.orig")
    open(path, "w", encoding="utf-8").write(text)
    print("\nApplied. Original in redwood_emmc.c.write.orig")


main()
