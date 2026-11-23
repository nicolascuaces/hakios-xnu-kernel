/*
 * The touch screen of the Samsung Galaxy A22 5G (SM-A226BR, MT6833): a Himax HX83112F, the touch
 * half of the panel's own display driver chip (TDDI), on SPI1. A source of touches for
 * /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg touchscreen=3.
 *
 * The chip has no flash: its firmware lives in its RAM and is gone at every reset. The loader
 * (mtkloader/mtkhimax.h, himax_bring_up) hands it the phone's own Himax_firmware_fw_jdi.bin on the
 * way here -- main code to 0x20000000, configuration to 0x10006A00, 0x10007F00 = 0xA99A9AA9 ("no
 * flash": with 0 the firmware waits for a flash that is not there and never scans), a pin reset
 * and reload_to_active. By the time this runs the chip is scanning, and all that is left is to
 * read what it says.
 *
 * There is no SPI framework here, and the loader already speaks to the chip by driving the four
 * pins by hand, so this does the same:
 *   pins     GPIO27 clock, 28 chip select, 29 data out, 30 data in, all at function 0 (GPIO);
 *            SPI mode 3: the clock idles high, data changes on its falling edge
 *   chip     a write is F2 <cmd> <data...>, a read F3 <cmd> 00 <data...>; the touch report is
 *            command 0x30 with the AHB burst (command 0x11) off around it
 *   report   56 bytes: 10 points of 4 bytes (x high, x low, y high, y low; 0xFFFF = none),
 *            byte 52 the number of fingers (0xFF = none), and a checksum: all 56 bytes add up to
 *            0 modulo 256. All zeros means nothing new.
 * The interrupt line (GPIO14) pulses too briefly to be seen by looking at it, so a thread reads
 * the report every 10 ms (boot-arg touch_poll_us=N to change it).
 *
 * Coordinates are 0..1080 by 0..2408 and the panel shows 1080 x 2400.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define GPIO_PHYS       0x10005000
#define GPIO_SIZE       0x1000

#define GPIO_DIR0_SET   0x004
#define GPIO_DIR0_CLR   0x008
#define GPIO_DOUT0_SET  0x104
#define GPIO_DOUT0_CLR  0x108
#define GPIO_DIN0       0x200
#define GPIO_MODE(p)    (0x300 + ((p) / 8) * 0x10)

#define PIN_CLK         27
#define PIN_CS          28
#define PIN_MO          29
#define PIN_MI          30

#define TOUCH_W         1080            /* the controller's range */
#define TOUCH_H         2408
#define PANEL_W         1080
#define PANEL_H         2400
#define MAX_FINGERS     10
#define REPORT_SIZE     56
#define POINT_COUNT_AT  52

static vm_offset_t gpio;

static inline uint32_t
rd(uint32_t at)
{
	return *(volatile uint32_t *)(gpio + at);
}

static inline void
wr(uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(gpio + at) = value;
}

static inline void
pin_set(uint32_t pin, int high)
{
	wr(high ? GPIO_DOUT0_SET : GPIO_DOUT0_CLR, 1u << pin);
}

static inline int
pin_get(uint32_t pin)
{
	return (rd(GPIO_DIN0) >> pin) & 1u;
}

static inline void
pin_settle(void)
{
	(void)rd(GPIO_DIN0);
	(void)rd(GPIO_DIN0);
}

static void
pin_mode(uint32_t pin, uint32_t func)
{
	uint32_t at = GPIO_MODE(pin), shift = (pin % 8) * 4;

	wr(at, (rd(at) & ~(0xfu << shift)) | (func << shift));
}

static void
pins_to_gpio(void)
{
	pin_mode(PIN_CLK, 0);
	pin_mode(PIN_CS, 0);
	pin_mode(PIN_MO, 0);
	pin_mode(PIN_MI, 0);
	pin_set(PIN_CS, 1);
	pin_set(PIN_CLK, 1);
	wr(GPIO_DIR0_SET, (1u << PIN_CS) | (1u << PIN_CLK) | (1u << PIN_MO));
	wr(GPIO_DIR0_CLR, 1u << PIN_MI);
	__asm__ volatile ("dsb sy");
	pin_settle();
}

/* One transaction with the chip select held low: len bytes out and as many in. */
static void
xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	pin_set(PIN_CLK, 1);
	pin_set(PIN_CS, 0);
	pin_settle();
	for (uint32_t i = 0; i < len; i++) {
		uint8_t o = tx ? tx[i] : 0xff, v = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(PIN_MO, (o >> bit) & 1);
			pin_set(PIN_CLK, 0);
			pin_settle();
			pin_set(PIN_CLK, 1);
			pin_settle();
			v = (uint8_t)((v << 1) | pin_get(PIN_MI));
		}
		if (rx) {
			rx[i] = v;
		}
	}
	pin_set(PIN_CS, 1);
	pin_settle();
}

static void
chip_write1(uint8_t cmd, uint8_t value)
{
	uint8_t tx[3] = { 0xF2, cmd, value };

	xfer(tx, NULL, sizeof(tx));
}

static void
read_report(uint8_t *report)
{
	uint8_t tx[3 + REPORT_SIZE], rx[3 + REPORT_SIZE];

	memset(tx, 0xff, sizeof(tx));
	tx[0] = 0xF3;
	tx[1] = 0x30;
	tx[2] = 0x00;
	chip_write1(0x11, 0x00);        /* AHB burst off */
	xfer(tx, rx, sizeof(tx));
	chip_write1(0x11, 0x01);        /* and on again, as the Android driver leaves it */
	memcpy(report, rx + 3, REPORT_SIZE);
}

/* ---- where a finger lands on the picture ---- */

static uint32_t fit;                    /* boot-arg fbfit: how the picture sits on the panel */

/* fbfit=1: the iPhone's proportions, PICTURE_H rows in the middle of the panel */
#define PICTURE_H       (PANEL_W * 2436 / 1125)
#define PICTURE_TOP     ((PANEL_H - PICTURE_H) / 2)
/* fbfit=2: the picture fills the height, its sides cut off */
#define PICTURE_W       (PANEL_H * 1125 / 2436)
#define PICTURE_CROP    ((PICTURE_W - PANEL_W) / 2)

static uint32_t
picture_x(uint32_t x)
{
	x = x * (PANEL_W - 1) / TOUCH_W;
	if (fit != 2) {
		return x * 65535 / (PANEL_W - 1);
	}
	return (x + PICTURE_CROP) * 65535 / (PICTURE_W - 1);
}

static uint32_t
picture_y(uint32_t y)
{
	y = y * (PANEL_H - 1) / TOUCH_H;
	if (fit != 1) {
		return y * 65535 / (PANEL_H - 1);
	}
	if (y <= PICTURE_TOP) {
		return 0;
	}
	y -= PICTURE_TOP;
	return y >= PICTURE_H ? 65535 : y * 65535 / (PICTURE_H - 1);
}

/* ---- reports ---- */

static bool down[MAX_FINGERS];
static uint32_t last_x[MAX_FINGERS], last_y[MAX_FINGERS];
static unsigned reports, touches, bad_sums;

/* false when the report is not one: nothing new (all zeros) or a bad checksum */
static bool
report_valid(const uint8_t *r)
{
	uint32_t sum = 0, zeros = 0;

	for (int i = 0; i < REPORT_SIZE; i++) {
		sum += r[i];
		zeros += r[i] == 0;
	}
	if (zeros == REPORT_SIZE) {
		return false;
	}
	if (sum % 256 != 0) {
		bad_sums++;
		return false;
	}
	return true;
}

static void
report(const uint8_t *r)
{
	bool seen[MAX_FINGERS] = { false };

	if (r[POINT_COUNT_AT] != 0xff) {
		for (unsigned which = 0; which < MAX_FINGERS; which++) {
			uint32_t x = ((uint32_t)r[4 * which] << 8) | r[4 * which + 1];
			uint32_t y = ((uint32_t)r[4 * which + 2] << 8) | r[4 * which + 3];

			if (x == 0xffff || y == 0xffff || x > TOUCH_W || y > TOUCH_H) {
				continue;
			}
			seen[which] = true;
			touches++;
			if (!down[which] || x != last_x[which] || y != last_y[which]) {
				redwood_touch_post(down[which] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, which,
				    picture_x(x), picture_y(y));
			}
			down[which] = true;
			last_x[which] = x;
			last_y[which] = y;
		}
	}
	for (unsigned which = 0; which < MAX_FINGERS; which++) {
		if (down[which] && !seen[which]) {
			redwood_touch_post(REDWOOD_TOUCH_UP, which, picture_x(last_x[which]), picture_y(last_y[which]));
			down[which] = false;
		}
	}
	reports++;
}

static void
himax_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint8_t r[REPORT_SIZE];
	uint32_t poll_us = 10000;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	PE_parse_boot_argn("fbfit", &fit, sizeof(fit));
	if (poll_us < 1000 || poll_us > 100000) {
		poll_us = 10000;
	}
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	pins_to_gpio();
	printf("himax: reading the touch screen every %u us (fbfit=%u)\n", poll_us, fit);

	for (unsigned tick = 1;; tick++) {
		read_report(r);
		if (report_valid(r)) {
			report(r);
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("himax: so far %u reports, %u finger positions, %u bad checksums; "
			    "last finger 0 at %u, %u\n", reports, touches, bad_sums, last_x[0], last_y[0]);
		}
		delay(poll_us);
	}
}

void
redwood_himax_init(void)
{
	uint32_t which = 0;
	thread_t thread;

	/* 1 Goodix (POCO), 2 OmniVision TD4160 (Infinix), 3 this phone's Himax */
	if (!PE_parse_boot_argn("touchscreen", &which, sizeof(which)) || which != 3) {
		return;
	}
	if (kernel_thread_start(himax_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("himax: cannot start the thread that reads the touch screen\n");
	}
}
