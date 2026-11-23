/*
 * The charger of the Samsung Galaxy A22 5G (SM-A226BR, MT6833) under XNU: an MT6360 sub-PMIC
 * (stock DT: "mediatek,mt6360_pmu_chg", primary_chg), NOT an RT9471. Boot-arg a22chg_step=N.
 *
 * Where it is (stock DT and gitlab.com/pessdres/a22x-mainline patch 0017):
 *   I2C5        0x11e01000 (MediaTek I2C, mt8192 layout "regs_v2"), its AP-DMA at 0x10217500
 *   I2C5 clock  imp_iic_wrap_w 0x11e03000: STA 0xe00, CLR 0xe04 (ungate), SET 0xe08; bit 1
 *   MT6360      7-bit address 0x34 = the PMU bank. Charger registers live there and, unlike the
 *               PMIC (0x1a) and LDO (0x64) banks, go WITHOUT CRC8: a plain register read.
 *
 * Steps:
 *   1  only reads MMIO: the clock gate and the I2C5 registers LK/Android left. Nothing written.
 *   2  ungates the I2C5 clock if needed and READS MT6360 registers over I2C (FIFO mode, no DMA,
 *      the timing LK left untouched). Nothing is written to the charger. Then every 10 s logs the
 *      charge state so one run shows whether the battery charges while iOS runs.
 *   3  as 2, and with a wall charger (type 4) raises AICR/ICHG to a22chg_aicr / a22chg_ichg (2000/2000 mA);
 *      with the PC port (type 2) it keeps 500/500. Only CTRL3 and CTRL7 are written.
 *   4  as 3, and the battery's percentage to iOS (redwood_battery_report -> RedwoodBattery, IOPMPowerSource) from the
 *      MT6360's ADC: VBAT, and IBAT while charging, by a Li-ion curve. Rough: without the discharge current (that is
 *      the MT6359's gauge) the voltage under load reads low. a22chg_mohm = the cell's resistance (150).
 *   5  as 4, and Android's own percentage (PMIC RTC 0x5aa bits 14:8) sets the starting point.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define I2C5_PHYS           0x11e01000
#define IICW_PHYS           0x11e03000
#define IICW_STA            0xe00
#define IICW_CLR            0xe04
#define IICW_I2C5           (1u << 1)

/* mt_i2c_regs_v2 (drivers/i2c/busses/i2c-mt65xx.c), 16-bit registers */
#define I2C_DATA_PORT       0x00
#define I2C_SLAVE_ADDR      0x04
#define I2C_INTR_MASK       0x08
#define I2C_INTR_STAT       0x0c
#define I2C_CONTROL         0x10
#define I2C_TRANSFER_LEN    0x14
#define I2C_TRANSAC_LEN     0x18
#define I2C_DELAY_LEN       0x1c
#define I2C_TIMING          0x20
#define I2C_START           0x24
#define I2C_EXT_CONF        0x28
#define I2C_LTIMING         0x2c
#define I2C_HS              0x30
#define I2C_IO_CONFIG       0x34
#define I2C_FIFO_ADDR_CLR   0x38
#define I2C_TRANSFER_LEN_AUX 0x44
#define I2C_CLOCK_DIV       0x48
#define I2C_SOFTRESET       0x50
#define I2C_DEBUGSTAT       0xe4
#define I2C_FIFO_STAT       0xf4

#define ST_COMP             (1u << 0)
#define ST_ACKERR           (1u << 1)
#define ST_HS_NACK          (1u << 2)
#define ST_ARB_LOST         (1u << 3)
#define ST_ALL              0x1f
#define CTL_RS              (1u << 1)
#define CTL_CLK_EXT         (1u << 3)
#define CTL_DIR_CHANGE      (1u << 4)
#define CTL_ACKERR_DET      (1u << 5)
#define CTL_LEN_CHANGE      (1u << 6)

#define MT6360_PMU          0x34

static vm_offset_t i2c, iicw;

static inline uint16_t r16(uint32_t at) { return *(volatile uint16_t *)(i2c + at); }
static inline void w16(uint32_t at, uint16_t v) { *(volatile uint16_t *)(i2c + at) = v; }
static inline uint32_t r32(vm_offset_t b, uint32_t at) { return *(volatile uint32_t *)(b + at); }
static inline void w32(vm_offset_t b, uint32_t at, uint32_t v) { *(volatile uint32_t *)(b + at) = v; }

static void
dump_i2c(const char *when)
{
	printf("a22chg: %s: clk STA %#x (I2C5 %s) | CTL %#x TIMING %#x LTIMING %#x HS %#x CLKDIV %#x "
	    "EXT %#x IO %#x STAT %#x FIFO %#x DBG %#x\n", when, r32(iicw, IICW_STA),
	    (r32(iicw, IICW_STA) & IICW_I2C5) ? "GATED" : "on", r16(I2C_CONTROL), r16(I2C_TIMING),
	    r16(I2C_LTIMING), r16(I2C_HS), r16(I2C_CLOCK_DIV), r16(I2C_EXT_CONF), r16(I2C_IO_CONFIG),
	    r16(I2C_INTR_STAT), r16(I2C_FIFO_STAT), r16(I2C_DEBUGSTAT));
}

/* one register of the MT6360 PMU bank: write the register number, repeated start, read 1 byte */
static int
mt6360_read(uint8_t reg, uint8_t *val)
{
	uint16_t st = 0;

	w16(I2C_INTR_STAT, ST_ALL);
	w16(I2C_FIFO_ADDR_CLR, 1);
	w16(I2C_SLAVE_ADDR, MT6360_PMU << 1);
	w16(I2C_INTR_MASK, ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST);
	w16(I2C_CONTROL, CTL_ACKERR_DET | CTL_CLK_EXT | CTL_DIR_CHANGE | CTL_RS | CTL_LEN_CHANGE);
	w16(I2C_TRANSFER_LEN, 1);
	w16(I2C_TRANSFER_LEN_AUX, 1);
	w16(I2C_TRANSAC_LEN, 2);
	w16(I2C_DATA_PORT, reg);
	w16(I2C_START, 1);
	for (int spin = 0; spin < 2000; spin++) {        /* up to ~20 ms; a byte at 400 kHz is ~25 us */
		st = r16(I2C_INTR_STAT);
		if (st & (ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST)) {
			break;
		}
		delay(10);
	}
	w16(I2C_INTR_STAT, ST_ALL);
	if (!(st & ST_COMP) || (st & (ST_ACKERR | ST_HS_NACK | ST_ARB_LOST))) {
		w16(I2C_SOFTRESET, 1);                  /* leave the controller clean for the next try */
		return st ? st : -1;
	}
	*val = r16(I2C_DATA_PORT) & 0xff;
	return 0;
}

static int
mt6360_write(uint8_t reg, uint8_t val)
{
	uint16_t st = 0;

	w16(I2C_INTR_STAT, ST_ALL);
	w16(I2C_FIFO_ADDR_CLR, 1);
	w16(I2C_SLAVE_ADDR, MT6360_PMU << 1);
	w16(I2C_INTR_MASK, ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST);
	w16(I2C_CONTROL, CTL_ACKERR_DET | CTL_CLK_EXT);
	w16(I2C_TRANSFER_LEN, 2);
	w16(I2C_TRANSFER_LEN_AUX, 0);
	w16(I2C_TRANSAC_LEN, 1);
	w16(I2C_DATA_PORT, reg);
	w16(I2C_DATA_PORT, val);
	w16(I2C_START, 1);
	for (int spin = 0; spin < 2000; spin++) {
		st = r16(I2C_INTR_STAT);
		if (st & (ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST)) {
			break;
		}
		delay(10);
	}
	w16(I2C_INTR_STAT, ST_ALL);
	if (!(st & ST_COMP) || (st & (ST_ACKERR | ST_HS_NACK | ST_ARB_LOST))) {
		w16(I2C_SOFTRESET, 1);
		return st ? st : -1;
	}
	return 0;
}

/*
 * Step 3: with a wall charger (USB_STATUS1 type 4 = DCP) raise the input limit (AICR, CTRL3 bits 7:2,
 * 100 mA + 50 mA/step) and the charge current (ICHG, CTRL7 bits 7:2, 100 mA + 100 mA/step). Anything else
 * (type 2 = PC port) goes back to 500/500, what LK sets. Stock DT: ac input 3.2 A, ac charge 2.05 A.
 */
static void
apply_limits(uint8_t usb, uint8_t ctrl3, uint8_t ctrl7, uint32_t aicr_ma, uint32_t ichg_ma)
{
	bool wall = ((usb >> 4) & 7) == 4;
	uint32_t a = wall ? aicr_ma : 500, i = wall ? ichg_ma : 500;
	uint8_t n3 = (uint8_t)((ctrl3 & 0x03) | (((a - 100) / 50) << 2));
	uint8_t n7 = (uint8_t)((ctrl7 & 0x03) | (((i - 100) / 100) << 2));

	if (n3 != ctrl3 || n7 != ctrl7) {
		int e3 = mt6360_write(0x13, n3), e7 = mt6360_write(0x17, n7);
		printf("a22chg: step 3: %s -> AICR %u mA (CTRL3 %#x->%#x %s), ICHG %u mA (CTRL7 %#x->%#x %s)\n",
		    wall ? "wall charger" : "not a wall charger", a, ctrl3, n3, e3 ? "ERROR" : "ok", i, ctrl7, n7,
		    e7 ? "ERROR" : "ok");
	}
}

/* n bytes from reg on, one transaction (the chip moves its pointer by itself): ADC reports are 3 bytes taken together */
static int
mt6360_read_n(uint8_t reg, uint8_t *buf, unsigned n)
{
	uint16_t st = 0;

	w16(I2C_INTR_STAT, ST_ALL);
	w16(I2C_FIFO_ADDR_CLR, 1);
	w16(I2C_SLAVE_ADDR, MT6360_PMU << 1);
	w16(I2C_INTR_MASK, ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST);
	w16(I2C_CONTROL, CTL_ACKERR_DET | CTL_CLK_EXT | CTL_DIR_CHANGE | CTL_RS | CTL_LEN_CHANGE);
	w16(I2C_TRANSFER_LEN, 1);
	w16(I2C_TRANSFER_LEN_AUX, (uint16_t)n);
	w16(I2C_TRANSAC_LEN, 2);
	w16(I2C_DATA_PORT, reg);
	w16(I2C_START, 1);
	for (int spin = 0; spin < 2000; spin++) {
		st = r16(I2C_INTR_STAT);
		if (st & (ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST)) {
			break;
		}
		delay(10);
	}
	w16(I2C_INTR_STAT, ST_ALL);
	if (!(st & ST_COMP) || (st & (ST_ACKERR | ST_HS_NACK | ST_ARB_LOST))) {
		w16(I2C_SOFTRESET, 1);
		return st ? st : -1;
	}
	for (unsigned i = 0; i < n; i++) {
		buf[i] = r16(I2C_DATA_PORT) & 0xff;
	}
	return 0;
}

static int
mt6360_write2(uint8_t reg, uint8_t hi, uint8_t lo)
{
	uint16_t st = 0;

	w16(I2C_INTR_STAT, ST_ALL);
	w16(I2C_FIFO_ADDR_CLR, 1);
	w16(I2C_SLAVE_ADDR, MT6360_PMU << 1);
	w16(I2C_INTR_MASK, ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST);
	w16(I2C_CONTROL, CTL_ACKERR_DET | CTL_CLK_EXT);
	w16(I2C_TRANSFER_LEN, 3);
	w16(I2C_TRANSFER_LEN_AUX, 0);
	w16(I2C_TRANSAC_LEN, 1);
	w16(I2C_DATA_PORT, reg);
	w16(I2C_DATA_PORT, hi);
	w16(I2C_DATA_PORT, lo);
	w16(I2C_START, 1);
	for (int spin = 0; spin < 2000; spin++) {
		st = r16(I2C_INTR_STAT);
		if (st & (ST_COMP | ST_ACKERR | ST_HS_NACK | ST_ARB_LOST)) {
			break;
		}
		delay(10);
	}
	w16(I2C_INTR_STAT, ST_ALL);
	if (!(st & ST_COMP) || (st & (ST_ACKERR | ST_HS_NACK | ST_ARB_LOST))) {
		w16(I2C_SOFTRESET, 1);
		return st ? st : -1;
	}
	return 0;
}

/*
 * Step 4: the MT6360's ADC (drivers/iio/adc/mt6360-adc.c): PMUADCCFG 0x56/0x57 (16 bits, big endian: bit 15 the ADC on,
 * bit N channel N), PMUADCRPT1 0x5a (bits 7:4 the channel wanted, 3:0 the channel the report is of) and the report in
 * 0x5b/0x5c (big endian). Channels: 4 VBAT (1.25 mV a count), 6 IBAT (2.5 mA a count: what the charger puts in).
 * The ADC also runs VBAT and TS by itself, so a report counts only when it says our channel.
 */
#define ADC_VBAT    4
#define ADC_IBAT    6

static int
adc_read(unsigned ch, uint32_t *raw)
{
	uint8_t rpt[3] = { 0, 0, 0 };
	int e;

	if ((e = mt6360_write(0x5a, (uint8_t)(ch << 4))) || (e = mt6360_write2(0x56, 0x80, 0)) ||
	    (e = mt6360_write2(0x56, (uint8_t)(0x80 | ((1u << ch) >> 8)), (uint8_t)((1u << ch) & 0xff)))) {
		return e;
	}
	delay(75 * 1000);
	e = -2;
	for (int t = 0; t < 50; t++) {
		if (!mt6360_read_n(0x5a, rpt, 3) && (rpt[0] & 0xf) == ch) {
			*raw = ((uint32_t)rpt[1] << 8) | rpt[2];
			e = 0;
			break;
		}
		delay(2000);
	}
	(void)mt6360_write2(0x56, 0x80, 0);           /* only the ADC on, no channel */
	(void)mt6360_write(0x5a, 0xf0);              /* no channel preferred */
	return e;
}

/* a Li-ion cell at rest, mV to hundredths of a percent (the Infinix's curve, redwood_mtkbatt.c: rough for this cell) */
static int32_t
percent_of(int32_t mv)
{
	static const struct { int32_t mv, hundredths; } curve[] = {
		{ 3400, 0 }, { 3620, 500 }, { 3690, 1000 }, { 3745, 2000 }, { 3785, 3000 }, { 3825, 4000 }, { 3885, 5000 },
		{ 3985, 6000 }, { 4100, 7000 }, { 4225, 8000 }, { 4345, 9000 }, { 4395, 9700 }, { 4410, 10000 },
	};

	if (mv <= curve[0].mv) {
		return 0;
	}
	for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
		if (mv <= curve[i].mv) {
			return curve[i - 1].hundredths + (mv - curve[i - 1].mv) * (curve[i].hundredths - curve[i - 1].hundredths) /
			       (curve[i].mv - curve[i - 1].mv);
		}
	}
	return 10000;
}

/*
 * Android's own percentage: its gauge leaves it in the PMIC's clock, bits 14:8 of RTC_AL_MTH (on the Infinix's MT6358,
 * 0x5aa = RTC base 0x588 + 0x22: redwood_mtkbatt.c). On the A22 it is the same 0x5aa (chg5: 0xbc0b = 60 with Android at 60). Read once, through the
 * PMIF as redwood_a22keys.c does; if it says 1..100 the voltage's percentage is moved by the difference from then on.
 */
#define PMIF_PHYS           0x10026000
#define PMIF_CMD            0x880
#define PMIF_RDATA          0x894
#define PMIF_VLDCLR         0x8a4
#define PMIF_STATE          0x8a8
#define PMIF_FSM(v)         (((v) >> 1) & 7u)

static bool
pmic_read(vm_offset_t pmif, uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (PMIF_FSM(r32(pmif, PMIF_STATE)) == 0) {
			w32(pmif, PMIF_CMD, reg);
			for (uint32_t s2 = 0; s2 < 200000; s2++) {
				if (PMIF_FSM(r32(pmif, PMIF_STATE)) == 6) {
					*value = r32(pmif, PMIF_RDATA) & 0xffff;
					w32(pmif, PMIF_VLDCLR, 1);
					ok = true;
					break;
				}
			}
			break;
		}
	}
	redwood_pwrap_unlock();
	return ok;
}

static int32_t android_offset;   /* hundredths: Android's percentage minus the voltage's, at the first look */
static int32_t android_pct = -1;

static int32_t kept_mv;          /* the cell at rest, smoothed */
static int32_t shown;            /* hundredths shown to iOS, -1 before the first */

static void
battery_look(uint8_t stat, uint8_t usb, uint8_t fod, uint32_t mohm, bool log)
{
	struct redwood_battery_state b = { .size = sizeof(b) };
	uint32_t vraw = 0, iraw = 0;
	int ev, ei;
	int32_t mv, ma, rest, pct;
	bool plugged = (fod & 0x10) != 0, charging = (stat >> 6) == 1, full = (stat >> 6) == 2;

	ev = adc_read(ADC_VBAT, &vraw);
	ei = charging ? adc_read(ADC_IBAT, &iraw) : 0;
	if (ev) {
		printf("a22chg: step 4: ADC VBAT no answer (%#x)\n", ev);
		return;
	}
	mv = (int32_t)(vraw * 1250 / 1000);
	ma = (charging && !ei) ? (int32_t)(iraw * 2500 / 1000) : 0;
	if (mv < 2800 || mv > 4600) {
		printf("a22chg: step 4: VBAT %u counts = %d mV: does not look like a battery, not reporting\n", vraw, mv);
		return;
	}
	/* the charge current lifts the voltage over the cell's resistance: brought back to rest */
	rest = mv - ma * (int32_t)mohm / 1000;
	kept_mv = kept_mv == 0 ? rest : (kept_mv * 3 + rest) / 4;
	pct = percent_of(kept_mv);
	if (android_pct > 0 && shown < 0) {
		android_offset = android_pct * 100 - pct;
		printf("a22chg: step 5: Android left %d%%, the voltage says %d.%02d%%: correcting by %+d.%02d points\n", android_pct,
		    pct / 100, pct % 100, android_offset / 100, (android_offset < 0 ? -android_offset : android_offset) % 100);
	}
	pct += android_offset;
	pct = full ? 10000 : pct < 0 ? 0 : pct > 10000 ? 10000 : pct;
	/* no jumps the wrong way: down only without charge, up only with it */
	if (shown >= 0 && ((plugged && pct < shown) || (!plugged && pct > shown))) {
		pct = shown;
	}
	/*
	 * Unplugged, the voltage sags at once under iOS's load (chg5: 3945 -> 3887 mV, 4.8 points) and nothing here knows
	 * that current. A cell this size cannot really move faster than ~3 points a minute (0.05 a second, ~2.5 A on
	 * 5000 mAh): so the shown percentage walks towards the voltage's at most 0.25 a look (5 s).
	 */
	if (shown >= 0) {
		pct = pct > shown + 25 ? shown + 25 : pct < shown - 25 ? shown - 25 : pct;
	}
	shown = pct;
	b.percent_hundredths = pct;
	b.voltage_uv = mv * 1000;
	b.current_ua = ma * 1000;
	b.temperature_hundredths = 2500;           /* not read: 25 degrees */
	b.present = 1;
	b.external = plugged;
	b.status = !plugged ? 2 : (full || pct >= 9950) ? 4 : charging ? 1 : 3;
	b.design_uah = 5000000;                    /* the A22 5G's 5000 mAh */
	b.usb_type = ((usb >> 4) & 7) == 4 ? 2 : ((usb >> 4) & 7) == 2 ? 1 : 0;
	redwood_battery_report(&b);
	if (log) {
		printf("a22chg: step 4: battery %d.%02d%% | VBAT %d mV (at rest ~%d mV) | IBAT %d mA%s | %s, iOS state %d\n",
		    pct / 100, pct % 100, mv, kept_mv, ma, ei ? " (ADC IBAT no answer)" : "",
		    plugged ? "charger YES" : "charger no", b.status);
	}
}

static void
dump_range(uint8_t first, uint8_t last)
{
	char line[200];
	int n = 0;
	uint8_t v;

	for (unsigned r = first; r <= last; r++) {
		int e = mt6360_read((uint8_t)r, &v);
		n += snprintf(line + n, sizeof(line) - n, e ? " %02x=ERR%x" : " %02x=%02x", r, e ? e : v);
		if (n > 150 || r == last) {
			printf("a22chg: regs%s\n", line);
			n = 0;
		}
	}
}

static const char *const chg_state[4] = { "ready (not charging)", "CHARGING", "charge complete", "FAULT" };

static void
charge_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t step = (uint32_t)(uintptr_t)parameter;
	uint8_t id = 0, stat, ctrl1, ctrl2, ctrl3, ctrl7, usb, fod;

	i2c = ml_io_map(I2C5_PHYS, 0x1000);
	iicw = ml_io_map(IICW_PHYS, 0x1000);
	dump_i2c("step 1, as LK left it");
	if (step < 2) {
		printf("a22chg: step 1 done (read-only). Next: a22chg_step=2\n");
		return;
	}
	if (r32(iicw, IICW_STA) & IICW_I2C5) {
		w32(iicw, IICW_CLR, IICW_I2C5);
		delay(100);
		dump_i2c("I2C5 clock turned on by us");
	}
	if (r16(I2C_TIMING) == 0 && r16(I2C_CLOCK_DIV) == 0) {
		printf("a22chg: I2C5 without LK's timing: not trying the bus (send me the log)\n");
		return;
	}
	int e = mt6360_read(0x00, &id);
	printf("a22chg: MT6360 DEV_INFO %s %#x (manufacturer %#x, expected 0x5, rev %u)\n",
	    e ? "ERROR" : "=", e ? e : id, id >> 4, id & 0xf);
	if (e) {
		dump_i2c("after the error");
		return;
	}
	dump_range(0x10, 0x1f);         /* CHG_CTRL1..CTRL16 */
	dump_range(0x20, 0x2f);         /* DEVICE_TYPE, USB_STATUS1 ... */
	dump_range(0x48, 0x4f);         /* CHG_STAT ... */
	dump_range(0x60, 0x63);         /* CHG_CTRL19 ... */
	dump_range(0xe7, 0xe7);         /* FOD_STAT: bit 4 = cargador enchufado */

	uint32_t aicr_ma = 2000, ichg_ma = 2000;
	(void)PE_parse_boot_argn("a22chg_aicr", &aicr_ma, sizeof(aicr_ma));
	(void)PE_parse_boot_argn("a22chg_ichg", &ichg_ma, sizeof(ichg_ma));
	if (aicr_ma < 500) aicr_ma = 500;
	if (aicr_ma > 3000) aicr_ma = 3000;
	if (ichg_ma < 500) ichg_ma = 500;
	if (ichg_ma > 2000) ichg_ma = 2000;   /* the stock DT charges at 2.05 A at most */
	uint32_t mohm = 150;
	(void)PE_parse_boot_argn("a22chg_mohm", &mohm, sizeof(mohm));
	shown = -1;
	if (step >= 5) {
		vm_offset_t pmif = ml_io_map(PMIF_PHYS, 0x1000);
		uint32_t v5a2 = 0, v5aa = 0;
		bool ok = pmic_read(pmif, 0x5aa, &v5aa);

		(void)pmic_read(pmif, 0x5a2, &v5a2);
		printf("a22chg: step 5: PMIC RTC 0x5a2 = %#x (%s, %u), 0x5aa = %#x (%u)\n", v5a2, ok ? "ok" : "no answer",
		    (v5a2 >> 8) & 0x7f, v5aa, (v5aa >> 8) & 0x7f);
		if (ok && ((v5aa >> 8) & 0x7f) >= 1 && ((v5aa >> 8) & 0x7f) <= 100) {
			android_pct = (int32_t)((v5aa >> 8) & 0x7f);
		}
	}
	if (step >= 4) {
		printf("a22chg: step 4 active: battery %% to iOS from the voltage (MT6360 ADC), resistance %u mOhm\n", mohm);
	}
	if (step >= 3) {
		printf("a22chg: step 3 active: with a wall charger AICR %u mA, ICHG %u mA; with the PC 500/500\n", aicr_ma, ichg_ma);
	}

	for (unsigned tick = 0;; tick++) {
		if (!mt6360_read(0x4a, &stat) && !mt6360_read(0x11, &ctrl1) && !mt6360_read(0x12, &ctrl2) &&
		    !mt6360_read(0x13, &ctrl3) && !mt6360_read(0x17, &ctrl7) && !mt6360_read(0x27, &usb) &&
		    !mt6360_read(0xe7, &fod)) {
			if (step < 4 || tick % 6 == 0)
			printf("a22chg: t=%us %s | VBUS %s, USB tipo %u | CTRL1 %#x (FSLP %u, OTG %u) CTRL2 %#x "
			    "AICR %u mA ICHG %u mA (CTRL3 %#x CTRL7 %#x) STAT %#x\n", tick * (step >= 3 ? 5 : 10),
			    chg_state[stat >> 6], (fod & 0x10) ? "SI" : "no", (usb >> 4) & 7, ctrl1, (ctrl1 >> 3) & 1,
			    ctrl1 & 1, ctrl2, 100 + ((ctrl3 >> 2) & 0x3f) * 50, 100 + ((ctrl7 >> 2) & 0x3f) * 100,
			    ctrl3, ctrl7, stat);
			if (step >= 3) {
				apply_limits(usb, ctrl3, ctrl7, aicr_ma, ichg_ma);
			}
			if (step >= 4) {
				battery_look(stat, usb, fod, mohm, tick % 6 == 0);
			}
		} else {
			printf("a22chg: t=%us lectura fallida\n", tick * (step >= 3 ? 5 : 10));
		}
		delay((step >= 3 ? 5 : 10) * 1000 * 1000);   /* step 3 looks every 5 s: a plug change is caught soon */
	}
}

void
redwood_a22charge_init(void)
{
	uint32_t step = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("a22chg_step", &step, sizeof(step)) || step == 0) {
		return;
	}
	if (kernel_thread_start(charge_thread, (void *)(uintptr_t)step, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("a22chg: could not start the charger thread\n");
	}
}
