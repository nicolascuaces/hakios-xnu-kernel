/*
 * mtka22boot.h -- Samsung A22 5G (MT6833): choose iOS without a PC, and give XNU the time.
 * Included by mtkloader.c (a22-autoboot.py puts it in, right before run_xnu()).
 *
 *  1. A22_KEY_BOOT=1: at power-on the loader draws "Hold VOL- = iOS" and watches the Vol- key
 *     for A22_KEY_WINDOW_MS. If Vol- stays down for at least A22_KEY_HOLD_MS in a row, XNU
 *     boots. If not, Android does (as always). The pmsg request still works.
 *     Vol- = MT6359P TOPSTATUS (0x2a) bit 3 HOMEKEY_DEB, clear while pressed
 *     (the same bit redwood_a22keys.c reads in iOS, tested in ios8).
 *     It only READS the PMIC through the PMIF (SWINF_2), like mtka22clk.h.
 *
 *  2. A22_RTC_EPOCH=1: reads the MT6359P's RTC (base 0x588, TC_SEC +0x0a ... YEA +0x16,
 *     like Linux's rtc-mt6397.c) and appends " epoch=<UTC seconds>" to the boot-args if they do
 *     not carry one already (xnu-cycle-a22.sh puts in the Mac's). If the value does not look like
 *     a date between 2025 and 2035, it adds nothing. Read-only.
 */
#ifndef A22_KEY_BOOT
#define A22_KEY_BOOT        0
#endif
#ifndef A22_KEY_WINDOW_MS
#define A22_KEY_WINDOW_MS   2500
#endif
#ifndef A22_KEY_HOLD_MS
#define A22_KEY_HOLD_MS     300
#endif
#ifndef A22_RTC_EPOCH
#define A22_RTC_EPOCH       1
#endif

#define A22B_PMIF_BASE      0x10026000ULL
#define A22B_PMIF_CMD       0x880
#define A22B_PMIF_RDATA     0x894
#define A22B_PMIF_VLDCLR    0x8a4
#define A22B_PMIF_STATE     0x8a8
#define A22B_PMIF_FSM(v)    (((v) >> 1) & 7u)
#define A22B_TOPSTATUS      0x2a
#define A22B_HOMEKEY_DEB    (1u << 3)       /* volumen abajo */
#define A22B_RTC_TC_SEC     (0x588 + 0x0a)

static int a22b_pmif_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (A22B_PMIF_FSM(rd32(A22B_PMIF_BASE + A22B_PMIF_STATE)) == state) {
			return 1;
		}
	}
	return 0;
}

static int a22b_pmic_read(uint32_t reg, uint32_t *value)
{
	if (!a22b_pmif_wait(0)) {
		if (A22B_PMIF_FSM(rd32(A22B_PMIF_BASE + A22B_PMIF_STATE)) == 6) {
			wr32(A22B_PMIF_BASE + A22B_PMIF_VLDCLR, 1);
		}
		return 0;
	}
	wr32(A22B_PMIF_BASE + A22B_PMIF_CMD, reg);
	if (!a22b_pmif_wait(6)) {
		return 0;
	}
	*value = rd32(A22B_PMIF_BASE + A22B_PMIF_RDATA) & 0xffffu;
	wr32(A22B_PMIF_BASE + A22B_PMIF_VLDCLR, 1);
	return 1;
}

static void a22b_text(uint32_t y, uint32_t color, const char *s)
{
	fbc.scale = 3;
	fbc.x = 48;
	fbc.y = y;
	fbc.fg = color;
	for (; *s; s++) {
		fbcon_putc(*s);
	}
	fbc.scale = 1;
	fbc.fg = 0xffffffffu;
}

/* 1 = Vol- se mantuvo apretado: el usuario quiere iOS */
static int a22_key_wants_xnu(void)
{
#if A22_KEY_BOOT
	uint32_t top = 0, held = 0, step = 20, need = A22_KEY_HOLD_MS / 20;
	uint64_t screen = fbc.base;

	if (!a22b_pmic_read(A22B_TOPSTATUS, &top)) {
		fbc.base = 0;
		dbg_puts("boot: PMIC sin respuesta: no miro Vol-, arranca Android\n");
		fbc.base = screen;
		return 0;
	}
	fb_rect(0, 600, PANEL_W, 160, 0xff000000u);
	a22b_text(640, 0xffffffffu, "Hold VOL- = iOS");
	for (uint32_t t = 0; t < A22_KEY_WINDOW_MS; t += step) {
		if (a22b_pmic_read(A22B_TOPSTATUS, &top) && !(top & A22B_HOMEKEY_DEB)) {
			if (++held >= need) {
				fbc.base = 0;
				dbg_puts("boot: Vol- mantenido al encender: Darwin / XNU\n");
				fbc.base = screen;
				return 1;
			}
		} else {
			held = 0;
		}
		dbg_delay_ms(step);
	}
	fbc.base = 0;
	dbg_puts("boot: no Vol- in the window: Android\n");
	fbc.base = screen;
#endif
	return 0;
}

/* days since 1970-01-01 (Howard Hinnant's civil algorithm) */
static int64_t a22b_days(int64_t y, uint32_t m, uint32_t d)
{
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	uint32_t yoe = (uint32_t)(y - era * 400);
	uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

static void a22_append_epoch(char *line, uint32_t cap)
{
#if A22_RTC_EPOCH
	uint32_t r[7], sec2 = 0, len = 0, base;
	int64_t year, epoch;
	char num[24];
	int n = 0;

	for (const char *p = line; p[0]; p++) {
		if ((p == line || p[-1] == ' ') && p[0] == 'e' && p[1] == 'p' && p[2] == 'o' && p[3] == 'c' &&
		    p[4] == 'h' && p[5] == '=') {
			out("RTC: the boot-args already carry an epoch, leaving it alone\n");
			return;
		}
	}
	for (int tries = 0; tries < 3; tries++) {
		for (int i = 0; i < 7; i++) {
			if (!a22b_pmic_read(A22B_RTC_TC_SEC + 2 * i, &r[i])) {
				out("RTC: the PMIC does not answer, no epoch\n");
				return;
			}
		}
		if (a22b_pmic_read(A22B_RTC_TC_SEC, &sec2) && sec2 >= r[0]) {
			break;          /* the minute did not change between reads */
		}
	}
	out("RTC raw: s ");
	out_dec(r[0]); out(" m "); out_dec(r[1]); out(" h "); out_dec(r[2]);
	out(" d "); out_dec(r[3]); out(" mes "); out_dec(r[5] & 0xf); out(" anio "); out_dec(r[6]);
	out("\n");
	/* mainline Linux keeps years since 1900; MTK/Samsung's kernel, since 1968 */
	base = r[6] >= 100 ? 1900 : 1968;
	year = (int64_t)base + (r[6] & 0x7f);
	if (year < 2025 || year > 2035 || (r[5] & 0xf) < 1 || (r[5] & 0xf) > 12 || r[3] < 1 || r[3] > 31 ||
	    r[2] > 23 || r[1] > 59 || r[0] > 59) {
		out("RTC: not a valid-looking date, no epoch\n");
		return;
	}
	epoch = a22b_days(year, r[5] & 0xf, r[3]) * 86400 + r[2] * 3600 + r[1] * 60 + r[0];
	while (line[len]) {
		len++;
	}
	if (len + 8 + 20 >= cap) {
		out("RTC: does not fit in the boot-args, no epoch\n");
		return;
	}
	for (int64_t v = epoch; v > 0; v /= 10) {
		num[n++] = (char)('0' + v % 10);
	}
	for (const char *s = " epoch="; *s; s++) {
		line[len++] = *s;
	}
	while (n) {
		line[len++] = num[--n];
	}
	line[len] = 0;
	out("RTC: epoch ");
	out_dec((uint64_t)epoch);
	out(" (UTC) added to the boot-args\n");
#else
	(void)line;
	(void)cap;
#endif
}
