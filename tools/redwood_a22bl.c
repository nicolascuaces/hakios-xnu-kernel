/*
 * The backlight of the Samsung Galaxy A22 5G's JDI panel. WORKS (ios-bl5.log, seen on the screen).
 * Boot-arg a22bl=1: light at a22bl_level (default 0x60, Android's medium) 5 s after start; the power key toggles it
 * between that and a22bl_off (default 0). Boot-arg a22bl_test=1 is the old three-step experiment.
 *
 * What Android does (dmesg of a22-backlight-map.txt): jdi_setbacklight_cmdq(), a one-byte brightness
 * command to the PANEL over DSI (bl_tb0[1] = 0x0a .. 0x60); there is no PWM or I2C chip in the way. The DSI is
 * at 0x14013000. LK leaves it in video mode, so a command goes in through VM_CMD (mtk_dsi.c, DSI_VM_CMD_CON,
 * 0x130) and is sent in the next blanking.
 *
 * ASSUMED, not seen in the log: that the command is the MIPI DCS "write display brightness" (0x51, one
 * parameter, short packet data id 0x15). The register layout of VM_CMD_CON is from memory of mtk_dsi.c:
 *   bit0 VM_CMD_EN, bit1 LONG_PKT, bit2 TIME_SEL, bit3 TS_VSA_EN, bit4 TS_VBP_EN, bit5 TS_VFP_EN,
 *   bits 15:8 data id, 23:16 data 0, 31:24 data 1.
 * This only asks for brightness, never for sleep or reset. It prints the DSI registers before and after every
 * step, so whatever happens is in the log.
 *
 * Boot-args: a22bl_test=1 (run it), a22bl_at=N seconds after the thread starts (default 20),
 *            a22bl_cmd=N the DCS command (default 0x51 = 81).
 * Sequence: 0x60 (Android's level at medium), 0x20, 0x04, back to 0x60; 4 s each.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define DSI_PHYS            0x14013000
#define DSI_VM_CMD_CON      0x130
#define VM_CMD_EN           (1u << 0)
#define VM_CMD_TS_VFP_EN    (1u << 5)

static vm_offset_t dsi;
static uint32_t trig = 1;

static void
wait_s(unsigned s)
{
	for (unsigned i = 0; i < s * 10; i++) {
		delay(100000);
	}
}

static void
dsi_dump(const char *when)
{
	printf("a22bl: %s: START %#x INTEN %#x INTSTA %#x CON_CTRL %#x MODE %#x TXRX %#x\n", when,
	    *(volatile uint32_t *)(dsi + 0x00), *(volatile uint32_t *)(dsi + 0x08),
	    *(volatile uint32_t *)(dsi + 0x0c), *(volatile uint32_t *)(dsi + 0x10),
	    *(volatile uint32_t *)(dsi + 0x14), *(volatile uint32_t *)(dsi + 0x18));
	printf("a22bl: %s: VM_CMD_CON %#x DATA0..3 %#x %#x %#x %#x\n", when, *(volatile uint32_t *)(dsi + DSI_VM_CMD_CON),
	    *(volatile uint32_t *)(dsi + 0x134), *(volatile uint32_t *)(dsi + 0x138),
	    *(volatile uint32_t *)(dsi + 0x13c), *(volatile uint32_t *)(dsi + 0x140));
}

static void
map_dsi(void)
{
	if (dsi == 0) {
		dsi = ml_io_map(DSI_PHYS, 0x1000);
	}
}

/* The brightness command that was seen to work on the A22 (ios-bl5.log): data with EN = 0, EN = 1, and a pulse of START bit 16. */
static void
bl_write(uint32_t cmd, uint32_t value)
{
	uint32_t con = VM_CMD_TS_VFP_EN | (0x15u << 8) | ((cmd & 0xff) << 16) | ((value & 0xff) << 24);
	uint32_t st;

	map_dsi();
	*(volatile uint32_t *)(dsi + DSI_VM_CMD_CON) = con;
	delay(1000);
	*(volatile uint32_t *)(dsi + DSI_VM_CMD_CON) = con | VM_CMD_EN;
	st = *(volatile uint32_t *)(dsi + 0x00);
	*(volatile uint32_t *)(dsi + 0x00) = st & ~(1u << 16);
	delay(100);
	*(volatile uint32_t *)(dsi + 0x00) = st | (1u << 16);
}

static uint32_t bl_level = 0x60, bl_off = 0, bl_now = 0xff;
static bool bl_on = true, bl_enabled, bl_seen;
static uint64_t bl_off_at;

/* Called by the power key (redwood_a22keys.c): light off, light on again at the level of boot-arg a22bl_level. */
void
redwood_a22bl_toggle(void)
{
	if (!bl_enabled) {
		return;
	}
	bl_on = !bl_on;
	bl_off_at = mach_absolute_time();
	bl_seen = false;
	bl_now = bl_on ? bl_level : bl_off;
	bl_write(0x51, bl_now);
	printf("a22bl: power key: light %s (DCS 0x51 = %#x)\n", bl_on ? "on" : "off", bl_now);
}

static void
set_brightness(uint32_t cmd, uint32_t value)
{
	uint32_t con = VM_CMD_TS_VFP_EN | (0x15u << 8) | ((cmd & 0xff) << 16) | ((value & 0xff) << 24);
	uint32_t spin;

	/*
	 * The log of the first try showed VM_CMD_CON = 0xff511521 BEFORE we wrote anything: id 0x15, command 0x51,
	 * value 0xff, and VM_CMD_EN still 1 (LK's last brightness). So the layout is right, but with EN already 1 a
	 * write that keeps it 1 starts nothing. A command goes out on the edge: data with EN = 0, then EN = 1.
	 */
	printf("a22bl: sending DCS %#x = %#x (VM_CMD_CON %#x, then EN)\n", cmd, value, con | VM_CMD_EN);
	*(volatile uint32_t *)(dsi + DSI_VM_CMD_CON) = con;
	delay(1000);
	*(volatile uint32_t *)(dsi + DSI_VM_CMD_CON) = con | VM_CMD_EN;
	/*
	 * Second try (ios-bl4.log): with EN set the DSI still did not clear it. START read 0x10001: bit 0 is the video
	 * running, bit 16 is VM_CMD_START in mtk_dsi.c (from memory). Pulse only bit 16, keep bit 0 as it is.
	 * boot-arg a22bl_trig=0 leaves it out.
	 */
	if (trig) {
		uint32_t st = *(volatile uint32_t *)(dsi + 0x00);

		*(volatile uint32_t *)(dsi + 0x00) = st & ~(1u << 16);
		delay(100);
		*(volatile uint32_t *)(dsi + 0x00) = st | (1u << 16);
		printf("a22bl: START pulsed: was %#x, now %#x\n", st, *(volatile uint32_t *)(dsi + 0x00));
	}
	for (spin = 0; spin < 2000; spin++) {
		if (!(*(volatile uint32_t *)(dsi + DSI_VM_CMD_CON) & VM_CMD_EN)) {
			break;
		}
		delay(10);
	}
	printf("a22bl: EN %s after %u polls (VM_CMD_CON now %#x, INTSTA %#x)\n", spin < 2000 ? "cleared by the DSI" : "still 1",
	    spin, *(volatile uint32_t *)(dsi + DSI_VM_CMD_CON), *(volatile uint32_t *)(dsi + 0x0c));
	if (spin >= 2000) {
		*(volatile uint32_t *)(dsi + DSI_VM_CMD_CON) = con;     /* leave EN at 0 for the next edge */
	}
}

static void
bl_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t at = 20, cmd = 0x51;
	static const uint32_t steps[] = { 0x60, 0x20, 0x04, 0x60 };

	PE_parse_boot_argn("a22bl_at", &at, sizeof(at));
	PE_parse_boot_argn("a22bl_cmd", &cmd, sizeof(cmd));
	PE_parse_boot_argn("a22bl_trig", &trig, sizeof(trig));
	dsi = ml_io_map(DSI_PHYS, 0x1000);
	printf("a22bl: test in %u s, DCS command %#x\n", at, cmd);
	wait_s(at);
	dsi_dump("before");
	for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		set_brightness(cmd, steps[i]);
		dsi_dump("after");
		wait_s(4);
	}
	printf("a22bl: test done\n");
}

/*
 * Tap to wake. redwood_touch_post() (redwood_input.c) asks here before it passes a touch on. With the light off, the
 * finger that lands turns it on, and that whole touch (until it lifts) is kept from iOS so it does not press
 * something in a screen that was not seen. Returns true: swallow this event.
 */
static bool bl_swallow;

bool
redwood_a22bl_swallow(uint32_t type)
{
	if (!bl_enabled) {
		return false;
	}
	if (!bl_seen) {
		uint64_t ns;

		bl_seen = true;
		absolutetime_to_nanoseconds(mach_absolute_time() - bl_off_at, &ns);
		printf("a22bl: first touch event (type %u, light %s) %llu ms after the power key\n", type, bl_on ? "on" : "off", ns / 1000000);
	}
	if (type == 1 /* REDWOOD_TOUCH_DOWN */ && !bl_on) {
		bl_on = true;
		bl_now = bl_level;
		bl_write(0x51, bl_now);
		bl_swallow = true;
		printf("a22bl: touch: light on (DCS 0x51 = %#x)\n", bl_now);
		return true;
	}
	if (bl_swallow) {
		if (type == 3 /* REDWOOD_TOUCH_UP */) {
			bl_swallow = false;
		}
		return true;
	}
	return false;
}

static void
bl_start_thread(__unused void *parameter, __unused wait_result_t result)
{
	wait_s(5);                  /* the display is already running from LK; only the level is changed */
	bl_write(0x51, bl_level);
	bl_now = bl_level;
	printf("a22bl: light at %#x (boot-arg a22bl_level), off = %#x (a22bl_off); the power key toggles it\n", bl_level, bl_off);
}

void
redwood_a22bl_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	PE_parse_boot_argn("a22bl_level", &bl_level, sizeof(bl_level));
	PE_parse_boot_argn("a22bl_off", &bl_off, sizeof(bl_off));
	if (PE_parse_boot_argn("a22bl", &which, sizeof(which)) && which == 1) {
		bl_enabled = true;
		if (kernel_thread_start(bl_start_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	which = 0;
	if (!PE_parse_boot_argn("a22bl_test", &which, sizeof(which)) || which != 1) {
		return;
	}
	if (kernel_thread_start(bl_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("a22bl: cannot start the thread\n");
	}
}
