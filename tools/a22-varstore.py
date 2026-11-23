#!/usr/bin/env python3
"""
A22, step 2 of keeping /private/var between boots: the store goes into the tail of the eMMC partition
xnusys (the 512 MB that a22-emmcwrite.py opened for writing), through a new character device
/dev/remmc_var. Needs a22-emmcwrite.py applied first. It changes three files:

  bsd/dev/redwood_emmc.c   the tail grows to 512 MB; with boot-arg var_store=1 writing is enabled
                           (without emmc_wtest=1 there is no test: the one that passed is trusted) and
                           /dev/remmc_var appears: reads and writes of the tail only, offset 0 = the
                           start of the tail, and a write only by the process that holds kern.redwood.var_busy
  bsd/dev/redwood_ufs.c    redwood_var_save_on_return (the kernel asking the saver to save when the run
                           ends by Vol+ and Vol- or by the time limit) also works when the store is in the eMMC
  userland/src/var_store.c uses /dev/remmc_var when it exists (two slots of 256 MB from offset 0);
                           otherwise the Infinix's partition darwin as before

Nothing happens without boot-arg var_store=1. The preloader, lk, seccfg, efs, nvdata and PARTITION_CONFIG
are not touched: the tail is inside the GPT entry of xnusys, and the guard of step 1 still applies.

  python3 tools/a22-varstore.py ~/dev/hakios          -> only checks
  python3 tools/a22-varstore.py ~/dev/hakios apply    -> applies (saves *.var.orig the first time)
"""
import os, shutil, sys

XNU = "xnu-src/xnu-8796.121.2/bsd/dev/"
EMMC_VAR = r'''/* ---- /dev/remmc_var: the store of /private/var (userland/src/var_store.c), in the write tail ---- */

extern int redwood_var_claim_held(void);            /* bsd/dev/redwood_ufs.c: the caller holds kern.redwood.var_busy */
#define VAR_IO_BYTES        (DMA_MAX_BLOCKS * 512)

static int emmc_var_cmajor = -1;
static uint64_t emmc_var_refused;

int
redwood_emmc_var_ready(void)                        /* the store may be written: redwood_var_save_on_return asks */
{
	return emmc_var_on && emmc_w_first != 0;
}

static int
var_open(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return redwood_emmc_var_ready() ? 0 : ENXIO;
}

static int
var_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

/* a read, a bufferful at a time into memory of this kernel's and out to the reader with uiomove (PAN) */
static int
var_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t *bounce;
	int error = 0;

	if (!redwood_emmc_var_ready()) {
		return ENXIO;
	}
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % 512 != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(VAR_IO_BYTES, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < EMMC_W_BYTES) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > VAR_IO_BYTES) {
			want = VAR_IO_BYTES;
		}
		if (want > EMMC_W_BYTES - at) {
			want = EMMC_W_BYTES - at;
		}
		lck_mtx_lock(&emmc_lock);
		if (!read_any(emmc_w_first + at / 512, (uint32_t)((want + 511) / 512), bounce, (uint32_t)want)) {
			error = EIO;
		}
		lck_mtx_unlock(&emmc_lock);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, VAR_IO_BYTES);
	return error;
}

/*
 * A write: whole blocks, only inside the tail, and only by the process that holds the claim on the store
 * (kern.redwood.var_busy). The bytes are taken first and written after, so that nothing of this driver
 * is held while user memory is touched.
 */
static int
var_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t *bounce;
	int error = 0;

	if (!redwood_emmc_var_ready()) {
		return EROFS;
	}
	if (!redwood_var_claim_held()) {
		emmc_var_refused++;
		return EPERM;
	}
	if (uio_offset(uio) < 0 || (uint64_t)uio_offset(uio) % 512 != 0 || (uint64_t)uio_resid(uio) % 512 != 0) {
		return EINVAL;
	}
	if ((uint64_t)uio_offset(uio) + (uint64_t)uio_resid(uio) > EMMC_W_BYTES) {
		emmc_var_refused++;
		return ENOSPC;
	}
	bounce = kalloc_data(VAR_IO_BYTES, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0) {
		user_ssize_t want = uio_resid(uio);
		off_t at = uio_offset(uio);

		if (want > (user_ssize_t)VAR_IO_BYTES) {
			want = (user_ssize_t)VAR_IO_BYTES;
		}
		error = uiomove((caddr_t)bounce, (int)want, uio);
		if (error == 0) {
			lck_mtx_lock(&emmc_lock);
			memcpy(dma_buf, bounce, (size_t)want);
			if (!write_blocks_dma(emmc_w_first + (uint64_t)at / 512, (uint32_t)(want / 512))) {
				printf("emmc: store write at %lld, %lld bytes: %s (%#x)\n", (long long)at, (long long)want,
				    emmc_failed, emmc_failed_status);
				pio_mode();
				(void)stop();
				error = EIO;
			}
			lck_mtx_unlock(&emmc_lock);
			if (error != 0) {
				uio_setoffset(uio, at);                 /* these bytes did not go in */
				uio_setresid(uio, uio_resid(uio) + want);
			}
		}
	}
	kfree_data(bounce, VAR_IO_BYTES);
	return error;
}

static int
var_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == 512 ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = EMMC_W_BYTES / 512;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw emmc_var_cdevsw = {
	.d_open       = var_open,
	.d_close      = var_close,
	.d_read       = var_read,
	.d_write      = var_write,
	.d_ioctl      = var_ioctl,
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

'''

TARGETS = {
    XNU + "redwood_emmc.c": ("emmc_var_on", [
        ("#include <sys/systm.h>\n", "#include <sys/systm.h>\n#include <sys/uio.h>\n"),
        ("#define EMMC_W_BYTES        (256ull << 20)          /* the tail of the partition that may be written */\n",
         "#define EMMC_W_BYTES        (512ull << 20)          /* the tail of the partition that may be written */\n"),
        ("static uint64_t emmc_writes, emmc_write_blocks;\n",
         "static uint64_t emmc_writes, emmc_write_blocks;\nstatic bool emmc_var_on;                            /* var_store=1 and writing works: /dev/remmc_var */\n"),
        ("\tuint32_t want = 0;\n\tuint64_t at, t0, ns;\n\tuint32_t bad_dma = 0, bad_pio = 0;\n\n"
         "\tif (!PE_parse_boot_argn(\"emmc_wtest\", &want, sizeof(want)) || want == 0) {\n\t\treturn;\n\t}\n",
         "\tuint32_t want = 0, var = 0;\n\tuint64_t at, t0, ns;\n\tuint32_t bad_dma = 0, bad_pio = 0;\n\n"
         "\t(void)PE_parse_boot_argn(\"emmc_wtest\", &want, sizeof(want));\n"
         "\t(void)PE_parse_boot_argn(\"var_store\", &var, sizeof(var));\n"
         "\tif (want == 0 && var == 0) {\n\t\treturn;\n\t}\n"),
        ("\tt0 = mach_absolute_time();\n\tfor (at = 0; at < 2048; at += DMA_MAX_BLOCKS) {\n\t\ttest_fill(",
         "\tif (want == 0) {\n\t\temmc_var_on = true;\n"
         "\t\tprintf(\"emmc: the store of /private/var may be written (var_store=1), %llu MB\\n\", EMMC_W_BYTES >> 20);\n"
         "\t\treturn;\n\t}\n"
         "\tt0 = mach_absolute_time();\n\tfor (at = 0; at < 2048; at += DMA_MAX_BLOCKS) {\n\t\ttest_fill("),
        ("\tif (bad_dma != 0 || bad_pio != 0) {\n\t\temmc_w_first = 0;\n\t}\n}\n",
         "\tif (bad_dma != 0 || bad_pio != 0) {\n\t\temmc_w_first = 0;\n\t} else if (var != 0) {\n\t\temmc_var_on = true;\n"
         "\t\tprintf(\"emmc: the store of /private/var may be written (var_store=1)\\n\");\n\t}\n}\n"),
        ("/* ---- the partition ---- */\n", EMMC_VAR + "/* ---- the partition ---- */\n"),
        ("\t\tprintf(\"emmc: no /dev nodes\\n\");\n\t}\n}\n",
         "\t\tprintf(\"emmc: no /dev nodes\\n\");\n\t}\n"
         "\tif (redwood_emmc_var_ready()) {\n"
         "\t\temmc_var_cmajor = cdevsw_add(-1, &emmc_var_cdevsw);\n"
         "\t\tif (emmc_var_cmajor < 0 ||\n"
         "\t\t    devfs_make_node(makedev(emmc_var_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0600, \"remmc_var\") == NULL) {\n"
         "\t\t\tprintf(\"emmc: no /dev/remmc_var\\n\");\n\t\t} else {\n"
         "\t\t\tprintf(\"emmc: /dev/remmc_var: the store of /private/var, %llu MB\\n\", EMMC_W_BYTES >> 20);\n\t\t}\n\t}\n}\n"),
    ]),
    XNU + "redwood_ufs.c": ("redwood_emmc_var_ready", [
        ("static int ufs_var_busy_pid;\n",
         "static int ufs_var_busy_pid;\n"
         "extern int redwood_emmc_var_ready(void);       /* bsd/dev/redwood_emmc.c: the A22's store, in the eMMC */\n"),
        ("/*\n * The orderly end of a run, from osfmk/arm64/loader_return.c",
         "/* does the calling process hold the claim on the store (kern.redwood.var_busy)? */\n"
         "int\nredwood_var_claim_held(void)\n{\n\treturn ufs_var_busy_pid != 0 && ufs_var_busy_pid == proc_selfpid();\n}\n\n"
         "/*\n * The orderly end of a run, from osfmk/arm64/loader_return.c"),
        ("\tif (!ufs_ready || ufs_write_part < 0 || ufs_var_save != VAR_SAVE_IDLE) {\n",
         "\tif (!((ufs_ready && ufs_write_part >= 0) || redwood_emmc_var_ready()) || ufs_var_save != VAR_SAVE_IDLE) {\n"),
    ]),
    "userland/src/var_store.c": ("pick_store", [
        ("#define STORE_AT        8589934592LL            /* darwin's block 2097152: past the cryptexes, with room to spare */\n"
         "#define SLOT_BYTES      0x80000000LL            /* 2 GiB a slot, two slots */\n"
         "#define SLOTS           2\n"
         "#define DEVICE          \"/dev/rufs_darwin\"\n",
         "/* the Infinix's partition darwin, or the A22's tail of xnusys (bsd/dev/redwood_emmc.c): pick_store() chooses */\n"
         "#define STORE_AT        store_at\n"
         "#define SLOT_BYTES      slot_bytes\n"
         "#define SLOTS           2\n"
         "#define DEVICE          store_device\n"
         "#define DEVICE_EMMC     \"/dev/remmc_var\"\n"
         "static long long store_at = 8589934592LL;       /* darwin's block 2097152: past the cryptexes, with room to spare */\n"
         "static long long slot_bytes = 0x80000000LL;     /* 2 GiB a slot, two slots */\n"
         "static const char *store_device = \"/dev/rufs_darwin\";\n\n"
         "static void\npick_store(void)\n{\n\tint probe = open(DEVICE_EMMC, O_RDONLY, 0);\n\n"
         "\tif (probe >= 0) {\n\t\tclose(probe);\n\t\tstore_at = 0;                           /* the whole tail the kernel lets through */\n"
         "\t\tslot_bytes = 0x10000000LL;              /* 256 MiB a slot, two slots */\n\t\tstore_device = DEVICE_EMMC;\n\t}\n}\n"),
        ("\tif (strcmp(what, \"watch\") == 0) {\n\t\tif (!boot_arg_is(\"var_store\", \"1\")) {\n",
         "\tpick_store();\n\tif (strcmp(what, \"watch\") == 0) {\n\t\tif (!boot_arg_is(\"var_store\", \"1\")) {\n"),
    ]),
}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    root = os.path.expanduser(sys.argv[1])
    apply = len(sys.argv) > 2 and sys.argv[2] in ("apply", "aplicar")
    new_texts, bad = {}, 0
    for rel, (mark, changes) in TARGETS.items():
        path = os.path.join(root, rel)
        if not os.path.isfile(path):
            print("MISSING:", path)
            bad += 1
            continue
        text = open(path, encoding="utf-8").read()
        if mark in text:
            print("already applied:", rel)
            continue
        if rel.endswith("redwood_emmc.c") and "emmc_wtest" not in text:
            print("redwood_emmc.c has no write test yet: run a22-emmcwrite.py first")
            bad += 1
            continue
        for old, new in changes:
            n = text.count(old)
            if n != 1:
                print("NOT FOUND (%d times) |" % n, rel, "|", old.strip().splitlines()[0][:60])
                bad += 1
                continue
            text = text.replace(old, new)
            print("changes    |", rel, "|", new.strip().splitlines()[0][:50])
        new_texts[path] = text
    if bad:
        raise SystemExit("\n%d change(s) do not fit: touching nothing. Send me this output." % bad)
    if not apply:
        print("\nEverything fits. To apply it: python3 a22-varstore.py %s apply" % sys.argv[1])
        return
    for path, text in new_texts.items():
        if not os.path.exists(path + ".var.orig"):
            shutil.copy2(path, path + ".var.orig")
        open(path, "w", encoding="utf-8").write(text)
    print("\nApplied. Originals in *.var.orig")


main()
