/*
 * Samsung A22 5G touch controller probe: Himax HX83112F (JDI panel) over SPI1.
 * Protocol read from Samsung's official driver (drivers/input/touchscreen/mediatek/hx83112f):
 *   - SPI mode 3 (the clock rests high), 8 bits, MSB first.
 *   - read:   F3 <cmd> 00 and then N bytes; write: F2 <cmd> <data...>.
 *   - 32-bit register: the address (4 bytes, least significant first) to cmd 0x00,
 *     0x00 to cmd 0x0C, and the data comes out through cmd 0x08 (formerly "burst": cmd 0x13=0x31, 0x0D=0x10).
 *   - safe mode: cmd 0x31=0x27 and cmd 0x32=0x95.
 *   - identity: register 0x900000D0, bytes [3..1] = 83 11 2F for the HX83112F.
 * Pins (pinctrl-mt6833.c): SPI1 = GPIO27 CLK, GPIO28 CS, GPIO29 MOSI, GPIO30 MISO, all on
 * function 1; reset = GPIO15 (active low), interrupt = GPIO14.
 *
 * This probe does NOT use the SPI controller or touch clocks: it moves the four pins by hand (GPIO),
 * the slowest and safest way. It only does what Android's driver does when it starts the chip: a reset
 * pulse, enter safe mode and read registers. It sends no firmware. At the end it gives the pins back
 * to SPI and resets the chip, as Android's normal boot would leave it.
 *
 * Included in mtkloader.c after out(), out_hex() and mtkspi.h (rd32, wr32, pin_set, pin_get).
 */
#ifndef MTKHIMAX_H
#define MTKHIMAX_H

#include <stdint.h>

#if defined(HIMAX_TOUCH) && HIMAX_TOUCH && !defined(HIMAX_FW)
#define HIMAX_FW 1                              /* the XNU path loads the firmware */
#endif
#ifndef HIMAX_FW
#define HIMAX_FW 0
#endif

/* The request Android leaves in pmsg to run the probe once. */
static const char himax_probe_flag[] = "DARWIN-REDWOOD:PROBE-HIMAX";

#define HX_PIN_CLK      27
#define HX_PIN_CS       28
#define HX_PIN_MO       29
#define HX_PIN_MI       30
#define HX_PIN_RST      15
#define HX_PIN_IRQ      14
#define HX_SPI_FUNC     1

#define HX_GPIO_MODE(p) (0x300 + ((p) / 8) * 0x10)

static uint32_t hx_gpio_mode_get(uint32_t pin)
{
	return (rd32(GPIO_BASE + HX_GPIO_MODE(pin)) >> ((pin % 8) * 4)) & 0xfu;
}

static void hx_gpio_mode_set(uint32_t pin, uint32_t func)
{
	uint32_t reg = GPIO_BASE + HX_GPIO_MODE(pin);
	uint32_t shift = (pin % 8) * 4;

	wr32(reg, (rd32(reg) & ~(0xfu << shift)) | (func << shift));
	__asm__ volatile("dsb sy");
}

static void hx_pin_dir(uint32_t pin, int output)
{
	wr32(GPIO_BASE + (output ? GPIO_DIR0_SET : GPIO_DIR0_CLR), 1u << pin);
	__asm__ volatile("dsb sy");
}

static void hx_pins_to_gpio(void)
{
	hx_gpio_mode_set(HX_PIN_CLK, 0);
	hx_gpio_mode_set(HX_PIN_CS, 0);
	hx_gpio_mode_set(HX_PIN_MO, 0);
	hx_gpio_mode_set(HX_PIN_MI, 0);
	pin_set(HX_PIN_CS, 1);
	pin_set(HX_PIN_CLK, 1);
	hx_pin_dir(HX_PIN_CS, 1);
	hx_pin_dir(HX_PIN_CLK, 1);
	hx_pin_dir(HX_PIN_MO, 1);
	hx_pin_dir(HX_PIN_MI, 0);
	pin_settle();
}

static void hx_pins_to_spi(void)
{
	hx_gpio_mode_set(HX_PIN_CLK, HX_SPI_FUNC);
	hx_gpio_mode_set(HX_PIN_CS, HX_SPI_FUNC);
	hx_gpio_mode_set(HX_PIN_MO, HX_SPI_FUNC);
	hx_gpio_mode_set(HX_PIN_MI, HX_SPI_FUNC);
}

/* One transaction with CS low the whole time: len bytes out and the same number in. */
static void hx_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	pin_set(HX_PIN_CLK, 1);
	pin_set(HX_PIN_CS, 0);
	pin_settle();
	for (uint32_t i = 0; i < len; i++) {
		uint8_t o = tx ? tx[i] : 0xff, v = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(HX_PIN_MO, (o >> bit) & 1);
			pin_set(HX_PIN_CLK, 0);         /* falling edge: the data changes */
			pin_settle();
			pin_set(HX_PIN_CLK, 1);         /* rising edge: it is read */
			pin_settle();
			v = (uint8_t)((v << 1) | pin_get(HX_PIN_MI));
		}
		if (rx) {
			rx[i] = v;
		}
	}
	pin_set(HX_PIN_CS, 1);
	pin_settle();
}

static void hx_write(uint8_t cmd, const uint8_t *data, uint32_t len)
{
	uint8_t buf[16];

	buf[0] = 0xF2;
	buf[1] = cmd;
	for (uint32_t i = 0; i < len && i < sizeof(buf) - 2; i++) {
		buf[i + 2] = data[i];
	}
	hx_xfer(buf, 0, len + 2);
}

static void hx_read(uint8_t cmd, uint8_t *data, uint32_t len)
{
	uint8_t tx[3 + 64], rx[3 + 64];

	tx[0] = 0xF3;
	tx[1] = cmd;
	tx[2] = 0x00;
	for (uint32_t i = 0; i < len && i < 64; i++) {
		tx[3 + i] = 0xff;
	}
	hx_xfer(tx, rx, 3 + len);
	for (uint32_t i = 0; i < len; i++) {
		data[i] = rx[3 + i];
	}
}

/* 32-bit register, like himax_mcu_register_read() with len 4. */
static void hx_reg_read4(uint32_t addr, uint8_t out4[4])
{
	uint8_t a[4] = { (uint8_t)addr, (uint8_t)(addr >> 8), (uint8_t)(addr >> 16), (uint8_t)(addr >> 24) };
	uint8_t d;

	d = 0x31;
	hx_write(0x13, &d, 1);                  /* burst: continuous */
	d = 0x10;
	hx_write(0x0D, &d, 1);                  /* burst: increment of 4 */
	hx_write(0x00, a, 4);                   /* address */
	d = 0x00;
	hx_write(0x0C, &d, 1);                  /* access direction: read */
	hx_read(0x08, out4, 4);
}

static void hx_say4(const char *name, const uint8_t b[4])
{
	out(name);
	out(" = ");
	for (int i = 0; i < 4; i++) {
		out_hex(b[i]);
		out(" ");
	}
	out("\n");
}

static void hx_reset_pulse(void)
{
	hx_gpio_mode_set(HX_PIN_RST, 0);
	hx_pin_dir(HX_PIN_RST, 1);
	pin_set(HX_PIN_RST, 1);
	dbg_delay_ms(20);
	pin_set(HX_PIN_RST, 0);
	dbg_delay_ms(20);
	pin_set(HX_PIN_RST, 1);
	dbg_delay_ms(50);
}

/* ---------------------------------------------------------------------------------------------
 * Stage 2 (-DHIMAX_FW=1): load the firmware, turn the chip on and read fingers. Follows Samsung's
 * driver: himax_mcu_firmware_update_0f() -> himax_zf_part_info() -> sense_on(). The firmware is
 * the Himax_firmware_fw_jdi.bin of YOUR phone, converted to himaxfw.h with mkhimaxfw.py.
 * ------------------------------------------------------------------------------------------- */
#if HIMAX_FW
#include "himaxfw.h"

#define HX_CFG_TABLE      0x10400u      /* partition table inside the .bin (the driver finds it by its descriptor) */

static void hx_begin(void)
{
	pin_set(HX_PIN_CLK, 1);
	pin_set(HX_PIN_CS, 0);
	pin_settle();
}

static void hx_byte(uint8_t o)
{
	for (int bit = 7; bit >= 0; bit--) {
		pin_set(HX_PIN_MO, (o >> bit) & 1);
		pin_set(HX_PIN_CLK, 0);
		pin_settle();
		pin_set(HX_PIN_CLK, 1);
		pin_settle();
	}
}

static void hx_end(void)
{
	pin_set(HX_PIN_CS, 1);
	pin_settle();
}

static void hx_burst(int auto4)
{
	uint8_t d = 0x31;

	hx_write(0x13, &d, 1);
	d = (uint8_t)(0x10 | (auto4 ? 1 : 0));
	hx_write(0x0D, &d, 1);
}

/* himax_mcu_register_write(): the 4-byte address and the data in ONE single transaction (cmd 0x00). */
static void hx_reg_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
	hx_burst(len > 4);
	hx_begin();
	hx_byte(0xF2);
	hx_byte(0x00);
	hx_byte((uint8_t)addr);
	hx_byte((uint8_t)(addr >> 8));
	hx_byte((uint8_t)(addr >> 16));
	hx_byte((uint8_t)(addr >> 24));
	for (uint32_t i = 0; i < len; i++) {
		hx_byte(data[i]);
	}
	hx_end();
	dbg_delay_us(100);
}

static void hx_reg_write4(uint32_t addr, uint32_t value)
{
	uint8_t d[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };

	hx_reg_write(addr, d, 4);
}

static uint32_t hx_reg_read4v(uint32_t addr)
{
	uint8_t b[4];

	hx_reg_read4(addr, b);
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void hx_pin_reset(void)
{
	pin_set(HX_PIN_RST, 0);
	dbg_delay_ms(5);
	pin_set(HX_PIN_RST, 1);
	dbg_delay_ms(50);
}

/* Hardware CRC of a block of the SRAM (himax_mcu_check_CRC): returns the chip's CRC32. */
static uint32_t hx_crc_hw(uint32_t sram, uint32_t len)
{
	uint32_t words = len / 4;

	hx_reg_write4(0x80050020, sram);
	hx_reg_write4(0x80050028, (words & 0xffu) | ((words >> 8) << 8) | (0x99u << 16));
	for (int n = 0; n < 100; n++) {
		if ((hx_reg_read4v(0x80050000) & 1u) == 0) {
			return hx_reg_read4v(0x80050018);
		}
		dbg_delay_ms(1);
	}
	return 0xdeadbeefu;
}

/* himax_mcu_Calculate_CRC_with_AP: reflected CRC32C, little-endian 32-bit words. */
static uint32_t hx_crc_sw(const uint8_t *d, uint32_t len)
{
	uint32_t c = 0xFFFFFFFFu;

	for (uint32_t i = 0; i + 4 <= len; i += 4) {
		c ^= (uint32_t)d[i] | ((uint32_t)d[i + 1] << 8) | ((uint32_t)d[i + 2] << 16) | ((uint32_t)d[i + 3] << 24);
		for (int j = 0; j < 32; j++) {
			c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
		}
	}
	return c;
}

static uint32_t hx_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* hx83112_sense_off(false): stop the firmware and enter safe mode. */
static int hx_sense_off(void)
{
	uint8_t d;
	uint32_t st;

	hx_reg_write4(0x9000005C, 0xA5);                /* fw_data_fw_stop */
	dbg_delay_ms(10);
	for (int cnt = 0; cnt < 6; cnt++) {
		d = 0x27;
		hx_write(0x31, &d, 1);
		d = 0x95;
		hx_write(0x32, &d, 1);
		st = hx_reg_read4v(0x900000A8) & 0xff;
		if (st == 0x0C) {
			hx_reg_write4(0x80020020, 0);           /* TCON reset */
			dbg_delay_ms(1);
			hx_reg_write4(0x80020020, 1);
			hx_reg_write4(0x80020094, 0);           /* ADC reset */
			dbg_delay_ms(1);
			hx_reg_write4(0x80020094, 1);
			return 1;
		}
		hx_pin_reset();
	}
	return 0;
}

static int hx_write_sram_crc(uint32_t sram, const uint8_t *data, uint32_t len, int against_sw, uint32_t sw)
{
	for (int retry = 0; retry < 3; retry++) {
		hx_burst(1);
		hx_reg_write(sram, data, len);
		uint32_t hw = hx_crc_hw(sram, len);

		out("  chip CRC "); out_hex(hw);
		if (against_sw) {
			out(", expected "); out_hex(sw);
		}
		out("\n");
		if (against_sw ? hw == sw : hw == 0) {
			return 1;
		}
	}
	return 0;
}

static int hx_load_firmware(void)
{
	const uint8_t *fw = himax_fw;
	static uint8_t cfg[8192];
	uint32_t pnum = fw[HX_CFG_TABLE + 12];
	uint32_t base = 0xffffffffu, top = 0, top_size = 0;

	if (HIMAX_FW_SIZE < 0x10000 || pnum < 2 || pnum > 16) {
		out("firmware not recognised (partition table)\n");
		return 0;
	}
	out("firmware: "); out_hex(HIMAX_FW_SIZE); out(" bytes, "); out_hex(pnum); out(" partitions\n");

	hx_reg_write4(0x90000018, 0x55);                /* system reset */
	if (!hx_sense_off()) {
		out("does not enter safe mode\n");
		return 0;
	}
	out("safe mode OK\n");

	/* partition 0: the main firmware, to its SRAM (0x20000000) */
	const uint8_t *e0 = fw + HX_CFG_TABLE;
	uint32_t sram0 = hx_le32(e0), size0 = hx_le32(e0 + 4), off0 = hx_le32(e0 + 8);

	out("writing the main firmware ("); out_hex(size0); out(" bytes to "); out_hex(sram0); out(")\n");
	if (!hx_write_sram_crc(sram0, fw + off0, size0, 0, 0)) {
		out("main firmware CRC BAD\n");
		return 0;
	}

	/* the rest: configuration, gathered into one block from the lowest data SRAM */
	for (uint32_t i = 1; i < pnum; i++) {
		const uint8_t *e = fw + HX_CFG_TABLE + 16 * i;
		uint32_t a = hx_le32(e) & ~3u;

		if (a < base) {
			base = a;
		}
		if (a > top) {
			top = a;
			top_size = hx_le32(e + 4);
		}
	}
	uint32_t cfg_sz = (top - base) + top_size;

	cfg_sz += (16 - (cfg_sz % 16)) % 16;
	if (cfg_sz > sizeof(cfg)) {
		out("configuration too large\n");
		return 0;
	}
	for (uint32_t i = 0; i < cfg_sz; i++) {
		cfg[i] = 0;
	}
	for (uint32_t i = 1; i < pnum; i++) {
		const uint8_t *e = fw + HX_CFG_TABLE + 16 * i;
		uint32_t a = hx_le32(e) & ~3u, n = hx_le32(e + 4), o = hx_le32(e + 8);

		for (uint32_t k = 0; k < n; k++) {
			cfg[a - base + k] = fw[o + k];
		}
	}
	out("writing the configuration ("); out_hex(cfg_sz); out(" bytes to "); out_hex(base); out(")\n");
	if (!hx_write_sram_crc(base, cfg, cfg_sz, 1, hx_crc_sw(cfg, cfg_sz))) {
		out("configuration CRC BAD\n");
		return 0;
	}

	/* The DSRAM after the configuration (0x10007E50..0x10007FFF) holds power-up garbage, and that is
	 * where the firmware's command registers are (0x10007Fxx: mode, gestures, USB, diagnostics...).
	 * Android has them clean; I leave them at zero. */
	{
		static const uint8_t zeros[0x1B0];

		hx_burst(1);
		hx_reg_write(0x10007E50, zeros, sizeof(zeros));
		out("  DSRAM 0x10007E50-0x10007FFF zeroed: F10 "); out_hex(hx_reg_read4v(0x10007F10));
		out(", FD0 "); out_hex(hx_reg_read4v(0x10007FD0)); out(", F38 "); out_hex(hx_reg_read4v(0x10007F38));
		out("\n");
	}
	/* What Samsung does after himax_zf_part_info(): reload_disable(0) + power_on_init() */
	/* 0x10007F00: 0 = "there is flash, reload from it"; 0x9AA9 = "no flash" (Himax's HX83102J
	 * driver in mainline). The A22 has no flash and Android leaves 0xA99A9AA9 here: with 0 the firmware
	 * waits for a flash that does not exist and never sets 0x72C0. */
	hx_reg_write4(0x10007F00, 0xA99A9AA9);
	hx_reg_write4(0x800204B4, 0);                   /* raw_out_sel = 0 */
	hx_reg_write4(0x10007F04, 0);                   /* assign_sorting_mode = 0 */
	hx_reg_write4(0x10007294, 1);                   /* N frame = 1 (normal mode) */
	hx_reg_write4(0x100072C0, 0);                   /* "FW reload done" flag to 0 */
	out("  re-read 0x10007F00 "); out_hex(hx_reg_read4v(0x10007F00));
	out(", 0x10007294 "); out_hex(hx_reg_read4v(0x10007294));
	out(", 0x100072C0 "); out_hex(hx_reg_read4v(0x100072C0)); out("\n");

	/* two attempts: 0 = reset by the pin with Samsung's zero-flash timings (5 ms low,
	 * 5 ms high and reload_to_active at once); 1 = the same, in case the first came too late. */
	for (int how = 0; how < 2; how++) {
		uint8_t c = 0;

		uint32_t rv[5];
		int ok = 0, n = 0;

		(void)c;
		out(how ? "second attempt\n" : "reset by pin GPIO15 (5/5 ms, no printing until the end)\n");
		hx_reg_write4(0x9000005C, 0);           /* sense_on: data_clear */
		dbg_delay_ms(10);
		pin_set(HX_PIN_RST, 0);
		dbg_delay_ms(5);
		pin_set(HX_PIN_RST, 1);
		dbg_delay_ms(5);
		for (int retry = 0; retry < 5 && !ok; retry++) {
			hx_reg_write4(0x90000048, 0xEC);    /* reload_to_active */
			dbg_delay_ms(1);
			rv[n] = hx_reg_read4v(0x90000048);
			ok = (rv[n++] & 0xffff) == 0x01EC;
		}
		uint32_t first72 = hx_reg_read4v(0x100072C0);

		for (int i = 0; i < n; i++) {
			out("reload_to_active: "); out_hex(rv[i]); out("\n");
		}
		out("0x100072C0 right after: "); out_hex(first72); out("\n");
		if (first72 == 0x72C0) {
			out("FW reload done at once (as in Android)\n");
			return 1;
		}
		if (!ok) {
			continue;
		}
		/* power_on_init waits for the firmware to write 0x72C0 into 0x100072C0 */
		for (int retry = 0; retry < 40; retry++) {
			uint32_t v = hx_reg_read4v(0x100072C0);

			if (v == 0x72C0) {
				out("FW reload done (0x100072C0 = 0x72C0) after "); out_hex(retry * 10); out(" ms\n");
				return 1;
			}
			if (retry % 10 == 0) {
				out("  waiting for FW reload, 0x100072C0 = "); out_hex(v);
				out(", A8 = "); out_hex(hx_reg_read4v(0x900000A8)); out("\n");
			}
			dbg_delay_ms(10);
		}
		out("WARNING: the firmware did not set 'reload done'\n");
		hx_reg_write4(0x100072C0, 0);
	}
	return 1;
}

/* Firmware status, like himax_mcu_read_FW_status() of Samsung's driver. */
static void hx_fw_status(void)
{
	static const uint32_t regs[] = {
		0x900000A8, /* central status (0x05 = firmware running) */
		0x900000E8, /* dd status: sync with the panel (TDDI) */
		0x900000E4, /* reset event */
		0x900000F8, /* fw state */
		0x10007F40, /* dbg msg */
		0x10007088, /* fw mode status */
	};

	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		out("    "); out_hex(regs[i]); out(" = "); out_hex(hx_reg_read4v(regs[i])); out("\n");
	}
}

/* Dump to compare with Android (tools/himax-dump-android.sh): rows of 8 words, without the
 * rows of zeros. Read-only. */
static void hx_dump_regs(void)
{
	static const uint32_t areas[][2] = {
		{ 0x10006A00, 0x1600 },                     /* DSRAM: configuration + FW variables */
		{ 0x90000000, 0x100 },
		{ 0x80020000, 0x100 },
		{ 0x80020480, 0x80 },
	};

	out("== register dump ==\n");
	for (unsigned k = 0; k < sizeof(areas) / sizeof(areas[0]); k++) {
		for (uint32_t a = areas[k][0]; a < areas[k][0] + areas[k][1]; a += 32) {
			uint32_t w[8], any = 0;

			for (int i = 0; i < 8; i++) {
				w[i] = hx_reg_read4v(a + 4 * i);
				any |= w[i];
			}
			if (!any) {
				continue;
			}
			out("R "); out_hex(a); out(":");
			for (int i = 0; i < 8; i++) {
				out(" "); out_hex(w[i]);
			}
			out("\n");
		}
	}
	out("== end of dump ==\n");
}

/* Reads the event stack every ~20 ms (without waiting for the interrupt) and reports the IRQ separately. */
static void hx_watch_touch(uint32_t seconds)
{
	uint8_t buf[64], last[64];
	uint32_t shown = 0, irqs = 0, reads = 0;

	for (int i = 0; i < 64; i++) {
		last[i] = 0xA5;
	}
	hx_gpio_mode_set(HX_PIN_IRQ, 0);
	hx_pin_dir(HX_PIN_IRQ, 0);
	out("firmware status at the start:\n");
	hx_fw_status();
	out("TOUCH THE SCREEN (");
	out_hex(seconds);
	out(" s)\n");

	for (uint32_t ms = 0; ms < seconds * 1000 && shown < 200; ms += 20) {
		if (ms % 5000 == 0) {
			out("  t "); out_hex(ms / 1000); out(" s, IRQ lows "); out_hex(irqs);
			out(", IRQ pin "); out_hex(pin_get(HX_PIN_IRQ) ? 1 : 0);
			out(", reads "); out_hex(reads); out("\n");
			if (ms % 20000 == 0) {
				hx_fw_status();
			}
		}
		if (!pin_get(HX_PIN_IRQ)) {
			irqs++;
		}
		uint8_t d = 0x00;

		hx_write(0x11, &d, 1);                  /* AHB burst off */
		hx_read(0x30, buf, 64);                 /* event stack */
		d = 0x01;
		hx_write(0x11, &d, 1);                  /* AHB burst on */
		reads++;

		int changed = 0;

		for (int i = 0; i < 64; i++) {
			if (buf[i] != last[i]) {
				changed = 1;
			}
			last[i] = buf[i];
		}
		if (changed) {
			uint32_t n = buf[52] == 0xff ? 0 : (buf[52] & 0x0f);

			out("ev "); out_hex(ms); out(" ms fingers "); out_hex(n);
			out(" x "); out_hex(((uint32_t)buf[0] << 8) | buf[1]);
			out(" y "); out_hex(((uint32_t)buf[2] << 8) | buf[3]);
			out(" |");
			for (int i = 0; i < 8; i++) {
				out(" "); out_hex(buf[i]);
			}
			out(" ..");
			for (int i = 52; i < 56; i++) {
				out(" "); out_hex(buf[i]);
			}
			out("\n");
			shown++;
		}
		dbg_delay_ms(20);
	}
	out("firmware status at the end:\n");
	hx_fw_status();
	hx_dump_regs();
}
#endif /* HIMAX_FW */


static int hx_ran;

static void himax_probe(void)
{
	uint8_t b[4], d;
	int ok = 0;

	hx_ran = 1;

	out("\nHimax HX83112F (A22), SPI1 by GPIO. State LK left:\n");
	out("  mode GPIO14 "); out_hex(hx_gpio_mode_get(HX_PIN_IRQ));
	out(", GPIO15 "); out_hex(hx_gpio_mode_get(HX_PIN_RST));
	out(", GPIO27 "); out_hex(hx_gpio_mode_get(HX_PIN_CLK));
	out(", GPIO28 "); out_hex(hx_gpio_mode_get(HX_PIN_CS));
	out(", GPIO29 "); out_hex(hx_gpio_mode_get(HX_PIN_MO));
	out(", GPIO30 "); out_hex(hx_gpio_mode_get(HX_PIN_MI));
	out("\n  DIN0 "); out_hex(rd32(GPIO_BASE + GPIO_DIN0));
	out(", DIR0 "); out_hex(rd32(GPIO_BASE));
	out(", interrupt "); out(pin_get(HX_PIN_IRQ) ? "high\n" : "low\n");

	hx_pins_to_gpio();
#if HIMAX_FW
	/* After a warm reboot the chip may still be running the firmware Android loaded */
	out("before touching anything: A8 = "); out_hex(hx_reg_read4v(0x900000A8));
	out(", 0x100072C0 = "); out_hex(hx_reg_read4v(0x100072C0)); out("\n");
	if ((hx_reg_read4v(0x900000A8) & 0xff) == 0x05) {
		out("ANDROID'S FIRMWARE IS STILL RUNNING: watching fingers for 15 s without reloading anything\n");
		hx_watch_touch(15);
	}
#endif
	out("chip reset (1-0-1, 20 ms)\n");
	hx_reset_pulse();

	d = 0;
	hx_read(0x13, &d, 1);
	out("check read (cmd 0x13): "); out_hex(d); out("\n");

	for (int try = 0; try < 5 && !ok; try++) {
		d = 0x27;
		hx_write(0x31, &d, 1);
		d = 0x95;
		hx_write(0x32, &d, 1);
		hx_reg_read4(0x900000A8, b);
		hx_say4("estado central (0x900000A8; 0x0C = modo seguro)", b);
		hx_reg_read4(0x900000D0, b);
		hx_say4("chip ID (0x900000D0; expected byte3..1 = 83 11 2F)", b);
		ok = (b[3] == 0x83 && b[2] == 0x11 && b[1] == 0x2f);
		if (!ok) {
			dbg_delay_ms(20);
		}
	}
	if (ok) {
		out("THE CHIP ANSWERS: it is an HX83112F. The SPI bus by GPIO works.\n");
#if HIMAX_FW
		if (hx_load_firmware()) {
			out("THE FIRMWARE RUNS: the chip is on.\n");
			hx_watch_touch(40);
		} else {
			out("loading the firmware failed (see above)\n");
		}
#endif
	} else {
		out("the chip did not give the expected ID (see the reads above)\n");
	}
}


#if HIMAX_FW
/*
 * XNU path (HIMAX_TOUCH): leaves the touch controller scanning for bsd/dev/redwood_himax.c
 * (boot-arg touchscreen=3), which reads it through the same pins. No finger test.
 */
static void himax_bring_up(void)
{
	uint8_t b[4], d;
	int ok = 0;

	hx_ran = 1;                             /* on the way back, Android gets the pins back as SPI */
	out("touch: Himax HX83112F, loading its firmware\n");
	hx_pins_to_gpio();
	hx_reset_pulse();
	for (int try = 0; try < 5 && !ok; try++) {
		d = 0x27;
		hx_write(0x31, &d, 1);
		d = 0x95;
		hx_write(0x32, &d, 1);
		hx_reg_read4(0x900000D0, b);
		ok = (b[3] == 0x83 && b[2] == 0x11 && b[1] == 0x2f);
		if (!ok) {
			dbg_delay_ms(20);
		}
	}
	if (!ok) {
		out("touch: the chip did not give its ID; no touch\n");
		return;
	}
	if (hx_load_firmware()) {
		out("touch: ready, the firmware is scanning\n");
	} else {
		out("touch: loading the firmware failed; no touch\n");
	}
}
#endif

/* Before starting Android: pins back to SPI and the chip in a clean reset, as its driver expects. */
static void himax_hand_back(void)
{
	if (!hx_ran) {
		return;
	}
	hx_pins_to_spi();
	hx_reset_pulse();
}

#endif /* MTKHIMAX_H */
