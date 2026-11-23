/*
 * The buttons of the Samsung Galaxy A22 5G (SM-A226BR, MT6833), as a source of button presses for
 * /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg buttons=3 (1 is the POCO's,
 * redwood_buttons.c; 2 the Infinix's, redwood_mtkkeys.c).
 *
 * Where they are, as the a22x-mainline Linux port has them (gitlab.com/pessdres/a22x-mainline,
 * patches 0021 and 0023):
 *   volume up    the SoC's keypad (0x10010000), row 0 column 1 of a single-key matrix: bit 1 of
 *                KP_MEM1 (mt6779-keypad.c, calc_row_col_single), clear while the key is down
 *   power        the PMIC, an MT6359P: TOPSTATUS (0x2a) bit 1, PWRKEY_DEB, clear while down
 *   volume down  the same register, bit 3, HOMEKEY_DEB (mtk-pmic-keys.c, mt6359_regs)
 * The PMIC is reached through the MT6833's PMIF (0x10026000), the AP's channel SWINF_2, as the
 * loader does it (mtkloader/mtka22clk.h). Nothing interrupts: a thread looks every 20 ms, and a key
 * has to read the same twice in a row before it counts.
 *
 * If the keypad reads all keys down at the start (its clock off), volume up is left out rather than
 * held down for ever. Boot-arg keylog=1: a line for every change.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define KPD_PHYS            0x10010000
#define KP_MEM1             0x0004
#define VOLUP_BIT           (1u << 1)

#define PMIF_PHYS           0x10026000
#define PMIF_CMD            0x880
#define PMIF_RDATA          0x894
#define PMIF_VLDCLR         0x8a4
#define PMIF_STATE          0x8a8
#define PMIF_FSM(v)         (((v) >> 1) & 7u)
#define PMIF_IDLE           0
#define PMIF_WFVLDCLR       6

#define MT6359_TOPSTATUS    0x2a
#define PWRKEY_DEB          (1u << 1)
#define HOMEKEY_DEB         (1u << 3)

static vm_offset_t kpd, pmif;

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
}

static bool
pmif_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PMIF_FSM(rd(pmif, PMIF_STATE)) == state) {
			return true;
		}
	}
	return false;
}

static bool
pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (!pmif_wait(PMIF_IDLE)) {
		if (PMIF_FSM(rd(pmif, PMIF_STATE)) == PMIF_WFVLDCLR) {
			wr(pmif, PMIF_VLDCLR, 1);       /* an answer nobody took: let it go */
		}
	} else {
		wr(pmif, PMIF_CMD, reg);
		if (pmif_wait(PMIF_WFVLDCLR)) {
			*value = rd(pmif, PMIF_RDATA) & 0xffff;
			wr(pmif, PMIF_VLDCLR, 1);
			ok = true;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

struct key {
	const char *name;
	uint32_t button;                /* REDWOOD_BUTTON_* */
	bool down, seen;                /* what was reported, and what the last look found */
};

static struct key keys[3] = {
	{ "volume up", REDWOOD_BUTTON_VOLUME_UP, false, false },
	{ "volume down", REDWOOD_BUTTON_VOLUME_DOWN, false, false },
	{ "power", REDWOOD_BUTTON_LOCK, false, false },
};

static void
keys_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t log = 0, presses = 0, top = 0, mem;
	bool keypad_ok, pmic_ok;

	PE_parse_boot_argn("keylog", &log, sizeof(log));
	kpd = ml_io_map(KPD_PHYS, 0x1000);
	pmif = ml_io_map(PMIF_PHYS, 0x1000);
	mem = rd(kpd, KP_MEM1);
	keypad_ok = (mem & 0xffff) != 0;
	pmic_ok = pmic_read(MT6359_TOPSTATUS, &top);
	printf("a22keys: the buttons are looked at every 20 ms; keypad %#x (%s), PMIC TOPSTATUS %#x (%s)\n", mem,
	    keypad_ok ? "volume up from it" : "all keys read down: volume up left out", top,
	    pmic_ok ? "power and volume down from it" : "NOT REACHABLE");

	for (unsigned tick = 1;; tick++) {
		bool now[3] = { false, false, false };

		mem = rd(kpd, KP_MEM1);
		top = PWRKEY_DEB | HOMEKEY_DEB;
		if (pmic_ok && !pmic_read(MT6359_TOPSTATUS, &top)) {
			top = PWRKEY_DEB | HOMEKEY_DEB;         /* no answer this time: nothing down */
		}
		now[0] = keypad_ok && !(mem & VOLUP_BIT);
		now[1] = !(top & HOMEKEY_DEB);
		now[2] = !(top & PWRKEY_DEB);

		for (unsigned i = 0; i < 3; i++) {
			struct key *k = &keys[i];

			/* the same twice in a row, or it is a bounce */
			if (now[i] == k->seen && now[i] != k->down) {
				k->down = now[i];
				redwood_touch_post(k->down ? REDWOOD_BUTTON_DOWN : REDWOOD_BUTTON_UP, k->button, 0, 0);
				presses += k->down;
				if (log) {
					printf("a22keys: %s %s (keypad %#x, PMIC %#x)\n", k->name, k->down ? "down" : "up", mem, top);
				}
			}
			k->seen = now[i];
		}
		if (tick % 3000 == 0) {
			printf("a22keys: so far %u presses; keypad %#x, PMIC TOPSTATUS %#x\n", presses, mem, top);
		}
		delay(20000);
	}
}

void
redwood_a22keys_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("buttons", &which, sizeof(which)) || which != 3) {
		return;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("a22keys: cannot start the thread that reads the buttons\n");
	}
}
