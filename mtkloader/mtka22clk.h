/*
 * A22 (MT6833): the two Cortex-A76 (the "big" cluster, cpu6-7) at their real speed before jumping to XNU.
 *
 * LK leaves them at the lowest step: 725 MHz at 0.65 V. This file raises the voltage first and then
 * the PLL, like mediatek-cpufreq in the mainline Linux port for the A22 (gitlab.com/pessdres/a22x-mainline,
 * patch 0028, tested on the phone at 2.0 GHz):
 *   voltage   VGPU11 of the MT6359P = Vproc of the big cluster, VSRAM_PROC1 = its SRAM. Always
 *             0.1 V <= VSRAM - VPROC <= 0.25 V and VSRAM <= 1.0 V (MediaTek's kernel limits).
 *             MT6359P registers: RG_BUCK_VGPU11_VOSEL 0x15b4 (0.4 V + 6.25 mV * n),
 *             RG_LDO_VSRAM_PROC1_VOSEL 0x1b44 (0.5 V + 6.25 mV * n), mask 0x7f.
 *   PMIC      through the MT6833's PMIF (0x10026000), channel SWINF_2: CMD 0x880, WDATA 0x884,
 *             RDATA 0x894, VLDCLR 0x8a4, status 0x8a8 (FSM in bits 1..3).
 *   clock     ARMPLL_BL0: apmixedsys 0x1000c000 + 0x21c (bit 31 asks for the change, 24..26 post
 *             divider, 0..21 multiplier of 26 MHz / 2^14). The cluster's mux (mcusys 0x0c53a000
 *             + 0x2a4, bits 9..10: 0 = 26 MHz, 1 = the PLL) is parked at 26 MHz while it changes.
 * OPP table of the big cluster (B20G, voltages without EEM, conservative): see a22_big_opp[].
 * With -DA22_BIG_MHZ=0 nothing is touched.
 */
#ifndef MTKA22CLK_H
#define MTKA22CLK_H

#include <stdint.h>

#ifndef A22_BIG_MHZ
#define A22_BIG_MHZ         2000u
#endif

#define A22_PMIF_BASE       0x10026000ULL
#define A22_PMIF_CMD        0x880
#define A22_PMIF_WDATA      0x884
#define A22_PMIF_RDATA      0x894
#define A22_PMIF_VLDCLR     0x8a4
#define A22_PMIF_STATE      0x8a8
#define A22_PMIF_FSM(v)     (((v) >> 1) & 7u)
#define A22_PMIF_IDLE       0
#define A22_PMIF_WFVLDCLR   6

#define MT6359P_HWCID               0x0008
#define MT6359P_CHIP_VER            0x5930
#define MT6359P_VGPU11_VOSEL        0x15b4
#define MT6359P_VSRAM_PROC1_VOSEL   0x1b44

#define A22_APMIXED_BASE    0x1000c000ULL
#define A22_ARMPLL_BL_CON1  0x21c
#define A22_MCUSYS_BASE     0x0c53a000ULL
#define A22_CPU_PLLDIV_CFG1 0x2a4

static int a22_pmif_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (A22_PMIF_FSM(rd32(A22_PMIF_BASE + A22_PMIF_STATE)) == state) {
			return 1;
		}
	}
	return 0;
}

static int a22_pmic_read(uint32_t reg, uint16_t *value)
{
	if (!a22_pmif_wait(A22_PMIF_IDLE)) {
		if (A22_PMIF_FSM(rd32(A22_PMIF_BASE + A22_PMIF_STATE)) == A22_PMIF_WFVLDCLR) {
			wr32(A22_PMIF_BASE + A22_PMIF_VLDCLR, 1);
		}
		return 0;
	}
	wr32(A22_PMIF_BASE + A22_PMIF_CMD, reg);
	if (!a22_pmif_wait(A22_PMIF_WFVLDCLR)) {
		return 0;
	}
	*value = (uint16_t)rd32(A22_PMIF_BASE + A22_PMIF_RDATA);
	wr32(A22_PMIF_BASE + A22_PMIF_VLDCLR, 1);
	return 1;
}

static int a22_pmic_write(uint32_t reg, uint16_t value)
{
	if (!a22_pmif_wait(A22_PMIF_IDLE)) {
		return 0;
	}
	wr32(A22_PMIF_BASE + A22_PMIF_WDATA, value);
	wr32(A22_PMIF_BASE + A22_PMIF_CMD, (1u << 29) | reg);
	return a22_pmif_wait(A22_PMIF_IDLE);
}

/* voltajes en microvoltios */
static uint32_t a22_vproc_uv(uint16_t sel)
{
	return 400000u + 6250u * (sel & 0x7fu);
}

static uint32_t a22_vsram_uv(uint16_t sel)
{
	return 500000u + 6250u * (sel & 0x7fu);
}

/* writes a VOSEL keeping the other bits, and reads it back */
static int a22_set_vosel(uint32_t reg, uint32_t base_uv, uint32_t uv)
{
	uint16_t v;

	if (!a22_pmic_read(reg, &v)) {
		return 0;
	}
	uint16_t sel = (uint16_t)((uv - base_uv + 6249u) / 6250u);     /* redondeo hacia arriba */

	if (!a22_pmic_write(reg, (uint16_t)((v & ~0x7fu) | (sel & 0x7fu)))) {
		return 0;
	}
	dbg_delay_us(200);              /* the buck's ramp: 5 mV/us, plenty */
	return a22_pmic_read(reg, &v) && (v & 0x7fu) == sel;
}

static const struct {
	uint32_t mhz, uv;
} a22_big_opp[] = {
	{ 725, 650000 }, { 840, 681250 }, { 898, 700000 }, { 985, 725000 }, { 1042, 737500 },
	{ 1129, 756250 }, { 1274, 800000 }, { 1418, 843750 }, { 1534, 875000 }, { 1650, 906250 },
	{ 1720, 925000 }, { 1790, 943750 }, { 1836, 950000 }, { 1906, 968750 }, { 1953, 981250 },
	{ 2000, 993750 },
};

static uint32_t a22_pll_mhz(uint32_t con1)
{
	uint32_t post = (con1 >> 24) & 7u;

	return (uint32_t)(((26ull * (con1 & 0x3fffffu)) >> 14) >> post);
}

static void a22_big_cores_clock(void)
{
	uint16_t hw = 0, sproc = 0, ssram = 0;
	uint32_t target_mhz = 0, target_uv = 0;

	for (unsigned i = 0; i < sizeof(a22_big_opp) / sizeof(a22_big_opp[0]); i++) {
		if (a22_big_opp[i].mhz <= A22_BIG_MHZ) {
			target_mhz = a22_big_opp[i].mhz;
			target_uv = a22_big_opp[i].uv;
		}
	}
	uint32_t before = rd32(A22_APMIXED_BASE + A22_ARMPLL_BL_CON1);

	out("big cores (A76): PLL a ");
	out_dec(a22_pll_mhz(before));
	out(" MHz");
	if (!a22_pmic_read(MT6359P_HWCID, &hw) || !a22_pmic_read(MT6359P_VGPU11_VOSEL, &sproc) ||
	    !a22_pmic_read(MT6359P_VSRAM_PROC1_VOSEL, &ssram)) {
		out(", the PMIC does not answer: touching nothing\n");
		return;
	}
	uint32_t vproc = a22_vproc_uv(sproc), vsram = a22_vsram_uv(ssram);

	out(", Vproc "); out_dec(vproc / 1000); out(" mV, Vsram "); out_dec(vsram / 1000);
	out(" mV, PMIC "); out_hex(hw); out("\n");
	if (hw < MT6359P_CHIP_VER || vproc < 550000 || vproc > 1000000 || vsram < 700000 || vsram > 1000000) {
		out("big cores: not what LK usually leaves: touching nothing\n");
		return;
	}
	if (target_mhz <= a22_pll_mhz(before)) {
		out("big cores: already at that speed or more\n");
		return;
	}

	/* 1) voltage in 50 mV steps, SRAM first, staying within 0.1..0.25 V of difference */
	while (vproc < target_uv) {
		uint32_t next = vproc + 50000 < target_uv ? vproc + 50000 : target_uv;
		uint32_t want_sram = next + 100000 > 1000000 ? 1000000 : next + 100000;

		if (want_sram > vsram) {
			if (!a22_set_vosel(MT6359P_VSRAM_PROC1_VOSEL, 500000, want_sram)) {
				out("big cores: VSRAM would not take the write: stopping here\n");
				return;
			}
			vsram = want_sram;
		}
		if (!a22_set_vosel(MT6359P_VGPU11_VOSEL, 400000, next)) {
			out("big cores: Vproc would not take the write: stopping here\n");
			return;
		}
		vproc = next;
	}

	/* 2) clock: mux to 26 MHz, new PLL, wait, mux back to the PLL */
	uint32_t post = target_mhz >= 1500 ? 0u : 1u;
	uint32_t pcw = (uint32_t)(((uint64_t)target_mhz << post) * 16384u / 26u);
	uint32_t mux = rd32(A22_MCUSYS_BASE + A22_CPU_PLLDIV_CFG1);

	wr32(A22_MCUSYS_BASE + A22_CPU_PLLDIV_CFG1, mux & ~(3u << 9));
	__asm__ volatile("dsb sy");
	dbg_delay_us(5);
	wr32(A22_APMIXED_BASE + A22_ARMPLL_BL_CON1, (before & ~((7u << 24) | 0x3fffffu)) | (1u << 31) | (post << 24) | pcw);
	__asm__ volatile("dsb sy");
	dbg_delay_us(30);
	wr32(A22_MCUSYS_BASE + A22_CPU_PLLDIV_CFG1, (mux & ~(3u << 9)) | (1u << 9));
	__asm__ volatile("dsb sy");

	out("big cores: ");
	out_dec(a22_pll_mhz(before));
	out(" -> ");
	out_dec(a22_pll_mhz(rd32(A22_APMIXED_BASE + A22_ARMPLL_BL_CON1)));
	out(" MHz, Vproc ");
	out_dec(vproc / 1000);
	out(" mV, Vsram ");
	out_dec(vsram / 1000);
	out(" mV\n");
}

#endif /* MTKA22CLK_H */
