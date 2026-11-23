/*
 * The eMMC as a block device (X6826B, and the A22 with DMA): the "xnusys" partition, where the system image lives, read
 * when the kernel asks for it rather than held in memory. Boot-arg emmc_root=1 makes it the root
 * (iokit/bsddev/IOKitBSDInit.cpp asks redwood_emmc_root for it); /dev/emmc0 and /dev/remmc0 are its
 * nodes once devfs is up.
 *
 * Until now the loader read the whole image -- 2.4 GB of it -- into DRAM as the RAM disk md0,
 * in the 4 GB the kernel has: 2.5 GB wired before the first program ran, the rest too little for
 * iOS with its wallpaper (run 320: 10-26 MB free, the system "unhealthy", jetsam at work), and
 * three and a half minutes of every boot spent reading it (11 MB/s).
 *
 * Nothing is set up here: LK has powered the card, set its bus up and left it in the transfer
 * state, and mtkloader has read the GPT through it the same way (mtkloader/msdc.h) -- MSDC0 in PIO
 * mode, plain block reads (CMD17, or CMD18 then CMD12). Registers as Linux's
 * drivers/mmc/host/mtk-sd.c has them.
 *
 * Read only, by construction: the strategy routine refuses writes and opening for writing is
 * refused. One request at a time, under a mutex, in pieces of EMMC_MAX_BLOCKS.
 *
 * A22 (MT6833): the data can also come by DMA, which is how Linux's mtk-sd.c always reads (the
 * a22x-mainline port runs this very eMMC with it, compatible "mediatek,mt8183-mmc": descriptors,
 * addresses of 36 bits). One GPD, one BD, and a bounce buffer in the kernel's own memory, its
 * pages checked to be one run of physical memory; the caches are cleaned before and dropped after,
 * since the controller does not look in them. The first 4 MB are read both ways and compared:
 * any difference, or any DMA error later, and it goes back to PIO for good. Boot-arg emmc_dma=0
 * keeps PIO from the start.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/redwood_emmc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

#define MSDC0_PHYS       0x11230000u
#define EMMC_PART        "xnusys"
#define EMMC_MAX_BLOCKS  256                /* 128 KB a command */
#define DMA_MAX_BLOCKS   1024               /* 512 KB a command by DMA */

#define MSDC_CFG         0x00
#define MSDC_CFG_PIO     (1u << 3)
#define MSDC_INT         0x0c
#define MSDC_FIFOCS      0x14
#define MSDC_FIFOCS_CLR  (1u << 31)
#define MSDC_INTEN       0x10
#define MSDC_RXDATA      0x1c
#define DMA_SA_H4BIT     0x8c
#define MSDC_DMA_SA      0x90
#define MSDC_DMA_CTRL    0x98
#define MSDC_DMA_CFG     0x9c
#define MSDC_PATCH_BIT2  0xb8
#define PB2_SUPPORT_64G  (1u << 1)          /* the descriptors' high address bits count */
#define DMA_CTRL_START   (1u << 0)
#define DMA_CTRL_STOP    (1u << 1)
#define DMA_CTRL_MODE    (1u << 8)          /* descriptors */
#define DMA_CTRL_BURST   (7u << 12)
#define DMA_BURST_64B    (6u << 12)
#define DMA_CFG_STS      (1u << 0)          /* running */
#define DMA_CFG_DECSEN   (1u << 1)          /* descriptor checksums */
#define SDC_CMD          0x34
#define SDC_ARG          0x38
#define SDC_STS          0x3c
#define SDC_STS_BUSY     3u                 /* SDCBUSY, CMDBUSY */
#define SDC_BLK_NUM      0x50

#define INT_CMDRDY       (1u << 8)
#define INT_CMDTMO       (1u << 9)
#define INT_RSPCRCERR    (1u << 10)
#define INT_XFER_COMPL   (1u << 12)
#define INT_DATTMO       (1u << 14)
#define INT_DATCRCERR    (1u << 15)
#define INT_DMA_BDCSERR  (1u << 17)
#define INT_DMA_GPDCSERR (1u << 18)
#define INT_DMA_PROTECT  (1u << 19)
#define INT_DMA_BAD      (INT_DATTMO | INT_DATCRCERR | INT_DMA_BDCSERR | INT_DMA_GPDCSERR | INT_DMA_PROTECT)

#define CMD_RSP_R1       (1u << 7)
#define CMD_RSP_R1B      (6u << 7)
#define CMD_DTYPE_SINGLE (1u << 11)
#define CMD_DTYPE_MULTI  (2u << 11)
#define CMD_STOP         (1u << 14)
#define CMD_BLKLEN(n)    (((n) & 0xfffu) << 16)

static vm_offset_t msdc;
static uint64_t emmc_lba, emmc_blocks;      /* the partition: its first block, and the image's length in blocks */
static int emmc_bmajor = -1, emmc_cmajor = -1;
static uint32_t emmc_int_seen;              /* MSDC_INT bits read (reading clears them) but not yet used */
static const char *emmc_failed;
static uint32_t emmc_failed_status;
static uint64_t emmc_reads, emmc_read_blocks, emmc_errors, emmc_dma_reads;
static LCK_GRP_DECLARE(emmc_lck_grp, "redwood-emmc");
static LCK_MTX_DECLARE(emmc_lock, &emmc_lck_grp);

static inline uint32_t
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(msdc + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(msdc + reg) = value;
}

static bool
fail(const char *what, uint32_t status)
{
	emmc_failed = what;
	emmc_failed_status = status;
	return false;
}

/* (spins are counted in register reads, as the loader does: about a microsecond each, give or take) */
static bool
wait_idle(void)
{
	for (uint32_t i = 0; i < 2000000; i++) {
		if (!(rd(SDC_STS) & SDC_STS_BUSY)) {
			return true;
		}
	}
	return fail("controller busy", rd(SDC_STS));
}

static bool
wait_int(uint32_t want, uint32_t bad, const char *what)
{
	for (uint32_t i = 0; i < 20000000; i++) {
		uint32_t status = rd(MSDC_INT);

		if (status) {
			wr(MSDC_INT, status);
			emmc_int_seen |= status;
		}
		if (emmc_int_seen & bad) {
			status = emmc_int_seen;
			emmc_int_seen = 0;
			return fail(what, status);
		}
		if (emmc_int_seen & want) {
			emmc_int_seen &= ~want;
			return true;
		}
	}
	return fail(what, emmc_int_seen);
}

static void
pio_mode(void)
{
	wr(MSDC_CFG, rd(MSDC_CFG) | MSDC_CFG_PIO);
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
}

static bool
stop(void)
{
	wr(SDC_ARG, 0);
	wr(SDC_CMD, 12 | CMD_RSP_R1B | CMD_STOP);
	return wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "stop") && wait_idle();
}

/* blocks of 512 bytes from the card's block lba into dst (4-aligned) */
static bool
read_blocks(uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	if (!wait_idle()) {
		return false;
	}
	pio_mode();
	wr(SDC_BLK_NUM, blocks);
	wr(SDC_ARG, (uint32_t)lba);                 /* high capacity: the unit is a block */
	wr(SDC_CMD, (blocks == 1 ? 17 : 18) | CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_BLKLEN(512));
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "command")) {
		return false;
	}
	for (uint32_t left = blocks * 512 / 4; left;) {
		uint32_t fifo = rd(MSDC_FIFOCS) & 0xff, spin = 0, words;

		while (fifo < 4) {
			uint32_t status = rd(MSDC_INT);

			if (status & (INT_DATTMO | INT_DATCRCERR)) {
				wr(MSDC_INT, status);
				if (blocks > 1) {
					(void)stop();
				}
				return fail("data", status);
			}
			if (++spin > 20000000) {
				return fail("data stalled", rd(MSDC_FIFOCS));
			}
			fifo = rd(MSDC_FIFOCS) & 0xff;
		}
		words = fifo / 4 < left ? fifo / 4 : left;
		for (uint32_t i = 0; i < words; i++) {
			*dst++ = rd(MSDC_RXDATA);
		}
		left -= words;
	}
	if (!wait_int(INT_XFER_COMPL, INT_DATTMO | INT_DATCRCERR, "transfer end")) {
		return false;
	}
	return blocks == 1 || stop();
}

/* the same, tried again once after a failure (the controller cleared and the card told to stop) */
static bool
read_retrying(uint64_t lba, uint32_t blocks, uint32_t *dst)
{
	if (read_blocks(lba, blocks, dst)) {
		return true;
	}
	emmc_errors++;
	printf("emmc: reading %u blocks at %llu: %s (%#x); trying again\n", blocks, lba, emmc_failed, emmc_failed_status);
	pio_mode();
	(void)stop();
	return read_blocks(lba, blocks, dst);
}

/* ---- DMA ---- */

static uint32_t sector[128] __attribute__((aligned(16)));      /* one block, for PIO and the GPT */

struct gpd {                                /* mtk-sd.c's mt_gpdma_desc */
	uint32_t info, next, ptr, len, arg, blknum, cmd;
};
struct bd {                                 /* and mt_bdma_desc */
	uint32_t info, next, ptr, len;
};

/* the descriptors in the first page, the data after it */
static uint8_t dma_area[4096 + DMA_MAX_BLOCKS * 512] __attribute__((aligned(4096)));
static struct gpd *dma_gpd = (struct gpd *)(void *)dma_area;               /* two of them */
static struct bd *dma_bd = (struct bd *)(void *)(dma_area + 256);
static uint8_t *dma_buf = dma_area + 4096;
static uint64_t dma_area_pa;
static bool dma_on;
static const char *dma_off_why = "not tried";

static void
cache_flush(const void *start, size_t len)  /* clean and drop to the point of coherency */
{
	uint64_t ctr, line;
	uintptr_t a, end = (uintptr_t)start + len;

	__asm__ volatile ("mrs %0, ctr_el0" : "=r"(ctr));
	line = 4u << ((ctr >> 16) & 0xf);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (a = (uintptr_t)start & ~(line - 1); a < end; a += line) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint32_t
desc_sum(const void *d)                     /* 0xff less the sum of the first 16 bytes */
{
	const uint8_t *b = d;
	uint32_t sum = 0;

	for (int i = 0; i < 16; i++) {
		sum += b[i];
	}
	return (0xffu - (sum & 0xffu)) & 0xffu;
}

static bool
dma_setup(void)
{
	uint32_t on = 1;

	if (PE_parse_boot_argn("emmc_dma", &on, sizeof(on)) && on == 0) {
		dma_off_why = "emmc_dma=0";
		return false;
	}
	dma_area_pa = kvtophys((vm_offset_t)dma_area);
	if (dma_area_pa == 0 || (dma_area_pa & 0xfff) || dma_area_pa + sizeof(dma_area) > (1ull << 36)) {
		dma_off_why = "the buffer has no usable physical address";
		return false;
	}
	for (size_t at = 4096; at < sizeof(dma_area); at += 4096) {
		if (kvtophys((vm_offset_t)(dma_area + at)) != dma_area_pa + at) {
			dma_off_why = "the buffer is not one run of physical memory";
			return false;
		}
	}
	return true;
}

/* blocks of 512 bytes from the card's block lba into dma_buf, by DMA */
static bool
read_blocks_dma(uint64_t lba, uint32_t blocks)
{
	uint64_t gpd_pa = dma_area_pa, bd_pa = dma_area_pa + 256, buf_pa = dma_area_pa + 4096;
	uint32_t len = blocks * 512, status = 0;
	bool ok;

	if (!wait_idle()) {
		return false;
	}
	memset(dma_area, 0, 4096);
	dma_bd[0].info = 1u | (uint32_t)(((buf_pa >> 32) & 0xf) << 28);        /* EOL, buffer's high bits */
	dma_bd[0].ptr = (uint32_t)buf_pa;
	dma_bd[0].len = len & 0xffffffu;
	dma_bd[0].info |= desc_sum(&dma_bd[0]) << 8;
	dma_gpd[0].info = 1u | 2u | (uint32_t)((((gpd_pa + sizeof(struct gpd)) >> 32) & 0xf) << 24) |
	    (uint32_t)(((bd_pa >> 32) & 0xf) << 28);                           /* HWO, BDP, high bits */
	dma_gpd[0].next = (uint32_t)(gpd_pa + sizeof(struct gpd));
	dma_gpd[0].ptr = (uint32_t)bd_pa;
	dma_gpd[0].info |= desc_sum(&dma_gpd[0]) << 8;
	cache_flush(dma_area, 4096);
	cache_flush(dma_buf, len);

	wr(MSDC_CFG, rd(MSDC_CFG) & ~MSDC_CFG_PIO);
	wr(MSDC_INT, rd(MSDC_INT));
	emmc_int_seen = 0;
	wr(MSDC_FIFOCS, rd(MSDC_FIFOCS) | MSDC_FIFOCS_CLR);
	for (uint32_t i = 0; i < 100000 && (rd(MSDC_FIFOCS) & MSDC_FIFOCS_CLR); i++) {
	}
	wr(MSDC_DMA_CFG, rd(MSDC_DMA_CFG) | DMA_CFG_DECSEN);
	wr(MSDC_DMA_CTRL, (rd(MSDC_DMA_CTRL) & ~(DMA_CTRL_BURST | DMA_CTRL_MODE)) | DMA_BURST_64B | DMA_CTRL_MODE);
	wr(MSDC_PATCH_BIT2, rd(MSDC_PATCH_BIT2) | PB2_SUPPORT_64G);    /* LK leaves it off: 32-bit addresses */
	wr(DMA_SA_H4BIT, (rd(DMA_SA_H4BIT) & ~0xfu) | (uint32_t)((gpd_pa >> 32) & 0xf));
	wr(MSDC_DMA_SA, (uint32_t)gpd_pa);

	wr(SDC_BLK_NUM, blocks);
	wr(SDC_ARG, (uint32_t)lba);
	wr(SDC_CMD, (blocks == 1 ? 17 : 18) | CMD_RSP_R1 | (blocks == 1 ? CMD_DTYPE_SINGLE : CMD_DTYPE_MULTI) | CMD_BLKLEN(512));
	if (!wait_int(INT_CMDRDY, INT_CMDTMO | INT_RSPCRCERR, "command (dma)")) {
		return false;
	}
	wr(MSDC_DMA_CTRL, rd(MSDC_DMA_CTRL) | DMA_CTRL_START);
	ok = wait_int(INT_XFER_COMPL, INT_DMA_BAD, "transfer end (dma)");
	if (!ok) {
		status = emmc_failed_status;
	}
	/* stopped either way, as mtk-sd.c's msdc_data_xfer_done does */
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
	if (!ok && status) {
		emmc_failed_status = status;
	}
	cache_flush(dma_buf, len);
	return ok;
}

/* the same into dst, by DMA while it works and by PIO after that */
static bool
read_any(uint64_t lba, uint32_t blocks, uint8_t *dst, uint32_t bytes)
{
	if (dma_on) {
		if (read_blocks_dma(lba, blocks)) {
			memcpy(dst, dma_buf, bytes);
			emmc_dma_reads++;
			return true;
		}
		emmc_errors++;
		printf("emmc: dma read of %u blocks at %llu: %s (%#x); PIO from now on\n", blocks, lba, emmc_failed,
		    emmc_failed_status);
		dma_on = false;
		dma_off_why = "an error";
		pio_mode();
		(void)stop();
	}
	if (bytes == blocks * 512 && !((uintptr_t)dst & 3)) {
		return read_retrying(lba, blocks, (uint32_t *)(void *)dst);
	}
	for (uint32_t done = 0; done < bytes; done += 512) {        /* a part block, or not 4-aligned */
		uint32_t n = bytes - done < 512 ? bytes - done : 512;

		if (!read_retrying(lba + done / 512, 1, sector)) {
			return false;
		}
		memcpy(dst + done, sector, n);
	}
	return true;
}

/* the first 4 MB by DMA and by PIO: on only if every byte agrees */
static void
dma_try(uint32_t *pio)
{
	uint64_t t0, ns;
	uint32_t at;

	if (!dma_setup()) {
		printf("emmc: PIO (%s)\n", dma_off_why);
		return;
	}
	for (at = 0; at < 4 * 2048 && at + DMA_MAX_BLOCKS <= emmc_blocks; at += DMA_MAX_BLOCKS) {
		if (!read_blocks_dma(emmc_lba + at, DMA_MAX_BLOCKS)) {
			printf("emmc: dma at block %u: %s (%#x; DMA_CFG %#x, PATCH_BIT2 %#x, SA %#x:%08x); PIO\n", at,
			    emmc_failed, emmc_failed_status, rd(MSDC_DMA_CFG), rd(MSDC_PATCH_BIT2), rd(DMA_SA_H4BIT),
			    rd(MSDC_DMA_SA));
			pio_mode();
			(void)stop();
			dma_off_why = "the test read failed";
			return;
		}
		for (uint32_t part = 0; part < DMA_MAX_BLOCKS; part += EMMC_MAX_BLOCKS) {
			if (!read_retrying(emmc_lba + at + part, EMMC_MAX_BLOCKS, pio) ||
			    memcmp(pio, dma_buf + part * 512, EMMC_MAX_BLOCKS * 512) != 0) {
				printf("emmc: dma and PIO differ at block %u; PIO\n", at + part);
				dma_off_why = "the test read differed";
				return;
			}
		}
	}
	/* and the time of the DMA alone */
	t0 = mach_absolute_time();
	for (at = 0; at < 4 * 2048 && at + DMA_MAX_BLOCKS <= emmc_blocks; at += DMA_MAX_BLOCKS) {
		if (!read_blocks_dma(emmc_lba + at, DMA_MAX_BLOCKS)) {
			dma_off_why = "the second test read failed";
			pio_mode();
			(void)stop();
			return;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	printf("emmc: DMA on, the same 4 MB in %llu ms (buffer at %#llx)\n", ns / 1000000, dma_area_pa);
	dma_on = true;
	dma_off_why = NULL;
}

/* ---- the partition ---- */

static uint32_t entries[128 * 128 / 4] __attribute__((aligned(16)));

static bool
find_partition(void)
{
	const uint8_t *s = (const uint8_t *)sector;
	uint64_t entries_lba, at;
	uint32_t count, size;

	if (!read_retrying(1, 1, sector) || memcmp(s, "EFI PART", 8) != 0) {
		printf("emmc: no GPT (%s)\n", emmc_failed ? emmc_failed : "no signature");
		return false;
	}
	memcpy(&entries_lba, s + 72, 8);
	memcpy(&count, s + 80, 4);
	memcpy(&size, s + 84, 4);
	if (count > 128 || size != 128 || !read_retrying(entries_lba, count * size / 512, entries)) {
		printf("emmc: the GPT's entries: %u of %u bytes\n", count, size);
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = (const uint8_t *)entries + i * size;
		size_t k;
		uint64_t first, last;

		for (k = 0; EMMC_PART[k] && e[56 + 2 * k] == (uint8_t)EMMC_PART[k] && e[57 + 2 * k] == 0; k++) {
		}
		if (EMMC_PART[k] != 0 || e[56 + 2 * k] != 0 || e[57 + 2 * k] != 0) {
			continue;
		}
		memcpy(&first, e + 32, 8);
		memcpy(&last, e + 40, 8);
		if (first == 0 || last < first) {
			return false;
		}
		emmc_lba = first;
		emmc_blocks = last - first + 1;
		break;
	}
	if (emmc_lba == 0) {
		printf("emmc: no " EMMC_PART " partition\n");
		return false;
	}
	/* as long as its APFS containers, back to back, as the loader measured it for md0 */
	for (at = 0; at < emmc_blocks * 512;) {
		uint32_t bs;
		uint64_t n;

		if (!read_retrying(emmc_lba + at / 512, 1, sector) || sector[8] != 0x4253584eu) {   /* "NXSB" */
			break;
		}
		bs = sector[9];
		memcpy(&n, s + 40, 8);
		if (bs < 512 || bs > 65536 || n == 0 || n * bs > emmc_blocks * 512 - at) {
			break;
		}
		at += n * bs;
	}
	if (at == 0) {
		printf("emmc: no APFS container in " EMMC_PART " (tools/system-flash.sh writes it)\n");
		return false;
	}
	printf("emmc: " EMMC_PART " at block %llu, %llu MB of %llu MB in its APFS containers\n", emmc_lba, at >> 20,
	    (emmc_blocks * 512) >> 20);
	emmc_blocks = at / 512;
	return true;
}

/* ---- the devices ---- */

static int
emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	return (flags & FWRITE) ? EROFS : 0;
}

static int
emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
emmc_strategy(buf_t bp)
{
	uint64_t block = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp), done = 0;
	uint8_t *data = (uint8_t *)buf_dataptr(bp);
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;                      /* this device never writes */
		goto out;
	}
	if (block >= emmc_blocks) {
		buf_setresid(bp, count);
		error = block == emmc_blocks ? 0 : EINVAL;
		goto out;
	}
	if (count > (emmc_blocks - block) * 512) {
		count = (uint32_t)((emmc_blocks - block) * 512);
	}
	lck_mtx_lock(&emmc_lock);
	while (done < count && error == 0) {
		uint32_t max = dma_on ? DMA_MAX_BLOCKS : EMMC_MAX_BLOCKS;
		uint32_t bytes = count - done > max * 512 ? max * 512 : count - done;
		uint32_t blocks = (bytes + 511) / 512;

		if (!read_any(emmc_lba + block + done / 512, blocks, data + done, bytes)) {
			error = EIO;
			break;
		}
		emmc_reads++;
		emmc_read_blocks += blocks;
		done += bytes;
	}
	lck_mtx_unlock(&emmc_lock);
	buf_setresid(bp, buf_count(bp) - done);
	if (error) {
		printf("emmc: block %llu, %u bytes: %s (%#x)\n", block, count, emmc_failed, emmc_failed_status);
	}
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCSETBLOCKSIZE:
		/* (spec_open sets 512 and back again, and takes a refusal for a broken device) */
		return *(uint32_t *)data == 512 ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = emmc_blocks;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = dma_on ? DMA_MAX_BLOCKS : EMMC_MAX_BLOCKS;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
emmc_size(__unused dev_t dev)
{
	return 512;
}

static int
emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(emmc_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))emmc_size, uio, 512);
}

static const struct bdevsw emmc_bdevsw = {
	.d_open     = emmc_open,
	.d_close    = emmc_close,
	.d_strategy = emmc_strategy,
	.d_ioctl    = emmc_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = emmc_size,
	.d_type     = D_DISK,
};

static const struct cdevsw emmc_cdevsw = {
	.d_open       = emmc_open,
	.d_close      = emmc_close,
	.d_read       = emmc_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = emmc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

int
redwood_emmc_root(dev_t *root)
{
	uint64_t t0 = mach_absolute_time(), ns;
	uint32_t *probe;

	if (emmc_bmajor >= 0) {
		*root = makedev(emmc_bmajor, 0);
		return 0;
	}
	msdc = ml_io_map(MSDC0_PHYS, 0x1000);
	if (!find_partition()) {
		return ENXIO;
	}
	/* how fast: the first 4 MB, which the root's mount reads anyway */
	probe = kalloc_data(EMMC_MAX_BLOCKS * 512, Z_WAITOK);
	if (probe != NULL) {
		for (uint32_t at = 0; at < 4 * 2048 && at + EMMC_MAX_BLOCKS <= emmc_blocks; at += EMMC_MAX_BLOCKS) {
			if (!read_retrying(emmc_lba + at, EMMC_MAX_BLOCKS, probe)) {
				break;
			}
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("emmc: 4 MB in %llu ms by PIO\n", ns / 1000000);
		dma_try(probe);
		kfree_data(probe, EMMC_MAX_BLOCKS * 512);
	}
	emmc_bmajor = bdevsw_add(-1, &emmc_bdevsw);
	if (emmc_bmajor < 0 || (emmc_cmajor = cdevsw_add_with_bdev(-1, &emmc_cdevsw, emmc_bmajor)) < 0) {
		printf("emmc: no room among the devices (%d, %d)\n", emmc_bmajor, emmc_cmajor);
		return ENXIO;
	}
	*root = makedev(emmc_bmajor, 0);
	return 0;
}

void
redwood_emmc_devfs(void)
{
	if (emmc_bmajor < 0 || emmc_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(emmc_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmc0") == NULL ||
	    devfs_make_node(makedev(emmc_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmc0") == NULL) {
		printf("emmc: no /dev nodes\n");
	}
}

/* sysctl kern.redwood.emmc: how much it has read, and its errors */
static int
emmc_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[200];

	snprintf(line, sizeof(line), "emmc: %llu reads (%llu by DMA), %llu MB, %llu errors; %s%s%s",
	    emmc_reads, emmc_dma_reads, emmc_read_blocks >> 11, emmc_errors, dma_on ? "DMA" : "PIO: ",
	    dma_on ? "" : dma_off_why, emmc_failed ? "; last error: " : "");
	if (emmc_failed) {
		strlcat(line, emmc_failed, sizeof(line));
	}
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, emmc, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, emmc_sysctl, "A",
    "the eMMC's reads so far");
