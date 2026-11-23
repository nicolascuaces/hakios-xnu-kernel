/*
 * Audio of the Samsung Galaxy A22 5G (SM-A226B, MT6833): the loudspeaker's way.
 *
 *   memory (DL3, the AFE's SRAM) -> AFE -> ADDA -> the codec in the MT6359P PMIC -> LINEOUT L -> SIA8109 -> speaker
 *
 * That is the way Android takes on this phone (tinymix and dmesg while a song played: DL3, ADDA_DL_CH1/CH2 DL3,
 * "LOL Mux" = Playback_L_DAC, "Ext_Speaker_Amp Switch" on; the amplifier's enable line is GPIO 169).
 * Values from the Linux port of this very phone, gitlab.com/pessdres/a22x-mainline (patches 0032-0043: the AFE
 * is the MT8192's, "audsys" at 0x11210000, IRQ 202, power domain AUDIO) and from the MT6359 codec's register map
 * (sound/soc/codecs/mt6359.h, include/linux/mfd/mt6359p/registers.h).
 *
 * Boot-arg a22aud_step=N (each step includes the ones before it):
 *   1  READS only: the AUDIO power domain in the SPM, and on the PMIC the audio supply VAUD18 and the codec's
 *      identity and state. Nothing is written, and nothing of the AFE itself is read (its block hangs the bus
 *      while its domain is off).
 *   2  TURNS ON the AUDIO domain, as mtk-pm-domains.c does it and as a22conn_step=4 did CONNSYS: the clocks the domain
 *      wants (infracfg_ao gates AUDIO and AUDIO_26M_BCLK, topckgen's AUD_INTBUS mux ungated), then SPM's power
 *      sequence, then its SRAM, then the bus protection off. Then READS the AFE's own registers (nothing of them is
 *      written). Off again at the next reboot.
 *   5  iOS's sound (/dev/redwood-audio) to the loudspeaker: no tone, the ring starts silent; amplifier on while
 *      there is sound; iOS's volume = LINEOUT gain; a22aud_gain = the loudest code (default 0x04 = +4 dB).
 *   4  codec LINEOUT L (mt6359.c DAPM order) + SIA8109 one-wire mode 6 on GPIO169: the tone sounds for a22aud_secs (5)
 *      at a22aud_gain (ZCD code, default 0x12 = -10 dB, never below 0x0c), then the amplifier goes off.
 *   3  AFE side: a looping 1 kHz sine in the AFE SRAM through DL3 -> ADDA DL, checks DL3_CUR moves. Codec and amp untouched (silent).
 *
 * Clocks (clk-mt6833 of the port): infracfg_ao 0x10001000: IFRAO3 CLR 0x8c / STA 0x94, AUDIO = bit 25;
 *   IFRAO4 CLR 0xa8 / STA 0xac, AUDIO_26M_BCLK = bit 4; topckgen 0x10000000: CLK_CFG_8 0x090 (CLR 0x098), aud_intbus
 *   gate = bit 7. Bus protection: infracfg_ao +0x718 CLR, +0x724 status, AUDIO = bit 4 (MT8192's TOP_AXI_PROT_EN_2).
 *   SPM +0x358 AUDIO: bit 0 RST_B, 1 isolation, 2 on, 3 on (2nd), 4 clock off, 8 SRAM power-down, 12 its ack.
 * AFE (0x11210000, MT8192's map): +0x00..0x0c AUDIO_TOP_CON0..3, +0x10 AFE_DAC_CON0, +0x84 DL3_CON0, +0x8c DL3_BASE,
 *   +0x94 DL3_CUR, +0x108 ADDA_DL_SRC2_CON0, +0x124 ADDA_UL_DL_CON0, +0x3b4 IRQ_MCU_EN.
 *
 * SPM (0x10006000): +0x358 AUDIO's control word, +0x16c / +0x170 the acknowledgements (bit 22 = AUDIO on).
 * PMIC (through the PMIF at 0x10026000): VAUD18 CON0 0x1c9a (bit 0 enable), MON 0x1c9e;
 *   AUD_TOP_ID 0x2300, AUD_TOP_REV0 0x2302, AUD_TOP_CKPDN_CON0 0x230c, AUDDEC_ANA_CON0 0x2588,
 *   AUDDEC_ANA_CON7 0x2596, AUDDEC_ANA_CON9 0x259a, ZCD_CON1 0x260a.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern boolean_t ml_set_interrupts_enabled(boolean_t enable);

#define A22A_SPM_PHYS           0x10006000u
#define A22A_SPM_AUDIO_CTL      0x358
#define A22A_SPM_PWR_ACK        0x16c
#define A22A_SPM_PWR_ACK_S      0x170
#define A22A_ACK_AUDIO          (1u << 22)

#define A22A_INFRA_PHYS         0x10001000u
#define A22A_TOPCK_PHYS         0x10000000u
#define A22A_AFE_PHYS           0x11210000u

#define A22A_PMIF_PHYS          0x10026000u
#define A22A_PMIF_CMD           0x880
#define A22A_PMIF_RDATA         0x894
#define A22A_PMIF_VLDCLR        0x8a4
#define A22A_PMIF_STATE         0x8a8
#define A22A_PMIF_FSM(v)        (((v) >> 1) & 7u)

#define A22A_VAUD18_CON0        0x1c9a
#define A22A_VAUD18_MON         0x1c9e
#define A22A_AUD_TOP_ID         0x2300
#define A22A_AUD_TOP_REV0       0x2302
#define A22A_AUD_TOP_CKPDN_CON0 0x230c
#define A22A_AUDDEC_ANA_CON0    0x2588
#define A22A_AUDDEC_ANA_CON7    0x2596
#define A22A_AUDDEC_ANA_CON9    0x259a
#define A22A_ZCD_CON1           0x260a

static vm_offset_t a22a_pmif;

static uint32_t
a22a_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static bool
a22a_pmif_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (A22A_PMIF_FSM(a22a_rd(a22a_pmif, A22A_PMIF_STATE)) == state) {
			return true;
		}
	}
	return false;
}

/* a PMIC register read through the PMIF, the same way the other drivers do it; false if the PMIF did not answer */
static bool
a22a_pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (a22a_pmif_wait(0)) {
		*(volatile uint32_t *)(a22a_pmif + A22A_PMIF_CMD) = reg;
		if (a22a_pmif_wait(6)) {
			*value = a22a_rd(a22a_pmif, A22A_PMIF_RDATA) & 0xffff;
			*(volatile uint32_t *)(a22a_pmif + A22A_PMIF_VLDCLR) = 1;
			ok = true;
		}
	} else if (A22A_PMIF_FSM(a22a_rd(a22a_pmif, A22A_PMIF_STATE)) == 6) {
		*(volatile uint32_t *)(a22a_pmif + A22A_PMIF_VLDCLR) = 1;
	}
	redwood_pwrap_unlock();
	return ok;
}

/* what one PMIC register says, or "?" in its place */
static void
a22a_say_reg(const char *name, uint32_t reg, char *out, size_t size)
{
	uint32_t v;

	if (a22a_pmic_read(reg, &v)) {
		snprintf(out, size, "%s %#x", name, v);
	} else {
		snprintf(out, size, "%s ?", name);
	}
}

static void
a22a_step1(void)
{
	vm_offset_t spm = ml_io_map(A22A_SPM_PHYS, 0x1000);
	uint32_t ctl = a22a_rd(spm, A22A_SPM_AUDIO_CTL);
	uint32_t ack = a22a_rd(spm, A22A_SPM_PWR_ACK), ack_s = a22a_rd(spm, A22A_SPM_PWR_ACK_S);
	char a[24], b[24], c[24], d[24];

	printf("a22aud: step 1: SPM AUDIO domain: control %#x, ACK %#x / %#x (AUDIO %s)\n", ctl, ack, ack_s,
	    ((ack & A22A_ACK_AUDIO) && (ack_s & A22A_ACK_AUDIO)) ? "ON" : "off");

	a22a_pmif = ml_io_map(A22A_PMIF_PHYS, 0x1000);
	a22a_say_reg("CON0", A22A_VAUD18_CON0, a, sizeof(a));
	a22a_say_reg("MON", A22A_VAUD18_MON, b, sizeof(b));
	printf("a22aud: step 1: PMIC VAUD18: %s, %s\n", a, b);

	a22a_say_reg("ID", A22A_AUD_TOP_ID, a, sizeof(a));
	a22a_say_reg("REV0", A22A_AUD_TOP_REV0, b, sizeof(b));
	a22a_say_reg("CKPDN_CON0", A22A_AUD_TOP_CKPDN_CON0, c, sizeof(c));
	printf("a22aud: step 1: PMIC audio top: %s, %s, %s\n", a, b, c);

	a22a_say_reg("DEC_CON0", A22A_AUDDEC_ANA_CON0, a, sizeof(a));
	a22a_say_reg("DEC_CON7", A22A_AUDDEC_ANA_CON7, b, sizeof(b));
	a22a_say_reg("DEC_CON9", A22A_AUDDEC_ANA_CON9, c, sizeof(c));
	a22a_say_reg("ZCD_CON1", A22A_ZCD_CON1, d, sizeof(d));
	printf("a22aud: step 1: PMIC codec: %s, %s, %s, %s\n", a, b, c, d);
}

static void
a22a_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
}

/* wait for the bits of `mask` to be all set (want_set) or all clear; the value last read in *value */
static bool
a22a_poll(vm_offset_t base, uint32_t off, uint32_t mask, bool want_set, uint32_t *value)
{
	uint32_t v = 0;

	for (uint32_t spin = 0; spin < 20000; spin++) {
		v = a22a_rd(base, off);
		if ((want_set && (v & mask) == mask) || (!want_set && (v & mask) == 0)) {
			*value = v;
			return true;
		}
		delay(10);
	}
	*value = v;
	return false;
}

static vm_offset_t
a22a_step2(void)
{
	vm_offset_t spm = ml_io_map(A22A_SPM_PHYS, 0x1000), infra = ml_io_map(A22A_INFRA_PHYS, 0x1000);
	vm_offset_t top = ml_io_map(A22A_TOPCK_PHYS, 0x1000);
	uint32_t ack = 0, ack_s = 0, sram = 0, prot = 0, before3, before4, before8;
	bool a1, a2, a3, a4;

	/* the clocks the domain wants */
	before3 = a22a_rd(infra, 0x94);
	before4 = a22a_rd(infra, 0xac);
	before8 = a22a_rd(top, 0x090);
	a22a_wr(infra, 0x8c, 1u << 25);
	a22a_wr(infra, 0xa8, 1u << 4);
	a22a_wr(top, 0x098, 1u << 7);
	delay(100);
	printf("a22aud: step 2: clocks: IFRAO3 %#x -> %#x (AUDIO bit 25 %s), IFRAO4 %#x -> %#x (26M bit 4 %s), CLK_CFG_8 %#x -> %#x (gate bit 7 %s)\n",
	    before3, a22a_rd(infra, 0x94), (a22a_rd(infra, 0x94) & (1u << 25)) ? "STILL GATED" : "on",
	    before4, a22a_rd(infra, 0xac), (a22a_rd(infra, 0xac) & (1u << 4)) ? "STILL GATED" : "on",
	    before8, a22a_rd(top, 0x090), (a22a_rd(top, 0x090) & (1u << 7)) ? "STILL GATED" : "on");

	/* SPM's power sequence, as a22conn_step=4 did it for CONNSYS */
	a22a_wr(spm, 0x0, a22a_rd(spm, 0x0) | 1u | (0x0b16u << 16));
	a22a_wr(spm, A22A_SPM_AUDIO_CTL, a22a_rd(spm, A22A_SPM_AUDIO_CTL) | (1u << 2));
	a1 = a22a_poll(spm, A22A_SPM_PWR_ACK, A22A_ACK_AUDIO, true, &ack);
	a22a_wr(spm, A22A_SPM_AUDIO_CTL, a22a_rd(spm, A22A_SPM_AUDIO_CTL) | (1u << 3));
	a2 = a22a_poll(spm, A22A_SPM_PWR_ACK_S, A22A_ACK_AUDIO, true, &ack_s);
	a22a_wr(spm, A22A_SPM_AUDIO_CTL, a22a_rd(spm, A22A_SPM_AUDIO_CTL) & ~(1u << 4));
	delay(1);
	a22a_wr(spm, A22A_SPM_AUDIO_CTL, a22a_rd(spm, A22A_SPM_AUDIO_CTL) & ~(1u << 1));
	a22a_wr(spm, A22A_SPM_AUDIO_CTL, a22a_rd(spm, A22A_SPM_AUDIO_CTL) | 1u);
	delay(10);
	a22a_wr(spm, A22A_SPM_AUDIO_CTL, a22a_rd(spm, A22A_SPM_AUDIO_CTL) & ~(1u << 8));
	a3 = a22a_poll(spm, A22A_SPM_AUDIO_CTL, 1u << 12, false, &sram);
	printf("a22aud: step 2: SPM AUDIO: control %#x, ACK %#x (%s), ACK_S %#x (%s), SRAM ack %#x (%s)\n",
	    a22a_rd(spm, A22A_SPM_AUDIO_CTL), ack, a1 ? "on" : "NO ACK", ack_s, a2 ? "on" : "NO ACK", sram,
	    a3 ? "powered" : "NO ACK");
	if (!(a1 && a2 && a3)) {
		printf("a22aud: step 2: the AUDIO domain did not come up: bus protection left on, the AFE not read\n");
		return 0;
	}
	a22a_wr(infra, 0x718, 1u << 4);
	a4 = a22a_poll(infra, 0x724, 1u << 4, false, &prot);
	delay(1000);
	printf("a22aud: step 2: bus protect: status %#x (%s)\n", prot, a4 ? "off" : "STILL ON");
	if (!a4) {
		printf("a22aud: step 2: bus protection did not clear: the AFE not read\n");
		return 0;
	}
	{
		vm_offset_t afe = ml_io_map(A22A_AFE_PHYS, 0x2000);

		printf("a22aud: step 2: AFE: TOP_CON0..3 %#x %#x %#x %#x, DAC_CON0 %#x, DL3_CON0 %#x, DL3_BASE %#x, DL3_CUR %#x, "
		    "ADDA_DL_SRC2_CON0 %#x, ADDA_UL_DL_CON0 %#x, IRQ_MCU_EN %#x\n",
		    a22a_rd(afe, 0x0), a22a_rd(afe, 0x4), a22a_rd(afe, 0x8), a22a_rd(afe, 0xc), a22a_rd(afe, 0x10),
		    a22a_rd(afe, 0x84), a22a_rd(afe, 0x8c), a22a_rd(afe, 0x94), a22a_rd(afe, 0x108), a22a_rd(afe, 0x124),
		    a22a_rd(afe, 0x3b4));
		return afe;
	}
}


/* A22A_SRAM: the AFE's own SRAM (Android's sram_size 39936; DL3 ring = its first 32 KB) */
#define A22A_SRAM_PHYS  0x11212000u
#define A22A_RING_BYTES 32640u      /* 8160 frames of S16 stereo = 170 periods of 48 frames (1 kHz at 48 kHz) */

#define AFE_DAC_CON0       0x10
#define AFE_CONN3          0x2c
#define AFE_CONN4          0x30
#define AFE_DL3_CON0       0x84
#define AFE_DL3_BASE_MSB   0x88
#define AFE_DL3_BASE       0x8c
#define AFE_DL3_CUR        0x94
#define AFE_DL3_END        0x9c
#define AFE_ADDA_DL_SRC2_CON0 0x108
#define AFE_ADDA_DL_SRC2_CON1 0x10c
#define AFE_ADDA_UL_DL_CON0   0x124
#define AFE_ADDA_PREDIS_CON0  0x260
#define AFE_ADDA_PREDIS_CON1  0x264
#define AFE_ADDA_DL_SDM_DCCOMP_CON 0xc50
#define AFE_ADDA_DL_SDM_AUTO_RESET_CON 0xc74
#define AFE_AUD_PAD_TOP    0xe40

/* a 1 kHz sine, 48 points, amplitude ~ -20 dBFS (6000/32767) */
static const int16_t a22a_sine48[48] = {
	0, 783, 1553, 2298, 3006, 3667, 4271, 4811, 5278, 5667, 5973, 6190,
	6200, 6190, 5973, 5667, 5278, 4811, 4271, 3667, 3006, 2298, 1553, 783,
	0, -783, -1553, -2298, -3006, -3667, -4271, -4811, -5278, -5667, -5973, -6190,
	-6200, -6190, -5973, -5667, -5278, -4811, -4271, -3667, -3006, -2298, -1553, -783
};

/*
 * Step 3: the AFE's side. A looping sine in the AFE SRAM goes DL3 -> interconnect -> ADDA DL (SRC2, SDM) towards
 * MTKAIF. The codec and the amplifier are NOT touched (step 4), so nothing sounds: this checks the digital part
 * only, by reading DL3_CUR move.
 */
static void
a22a_step3(vm_offset_t afe, bool silent)
{
	vm_offset_t sram = ml_io_map(A22A_SRAM_PHYS, 0x8000);
	volatile int16_t *ring = (volatile int16_t *)sram;
	uint32_t i, v, c0, c1, c2;

	if (!afe || !sram) {
		printf("a22aud: step 3: no AFE/SRAM mapping\n");
		return;
	}
	for (i = 0; i < A22A_RING_BYTES / 4; i++) {
		/* with step 5 the ring starts silent: the sine would beep when the codec comes on */
		ring[2 * i] = silent ? 0 : a22a_sine48[i % 48];
		ring[2 * i + 1] = silent ? 0 : a22a_sine48[i % 48];
	}
	printf("a22aud: step 3: SRAM filled (%u frames), first words %#x %#x\n", A22A_RING_BYTES / 4,
	    a22a_rd(sram, 0), a22a_rd(sram, 4));

	/* ADDA DL: SRC2 at 48 kHz (rate code 8), unmuted, gain 0xf74f; SDM 2nd order; no pre-distortion */
	a22a_wr(afe, AFE_ADDA_PREDIS_CON0, 0);
	a22a_wr(afe, AFE_ADDA_PREDIS_CON1, 0);
	a22a_wr(afe, AFE_ADDA_DL_SRC2_CON1, 0xf74fu << 16);
	a22a_wr(afe, AFE_ADDA_DL_SRC2_CON0, (8u << 28) | (3u << 24) | (1u << 12) | (1u << 11) | (1u << 1));
	a22a_wr(afe, AFE_ADDA_DL_SDM_DCCOMP_CON, 0x1du);
	a22a_wr(afe, AFE_ADDA_DL_SDM_AUTO_RESET_CON, 0x190000u | (1u << 31));
	a22a_wr(afe, AFE_AUD_PAD_TOP, 0x30);

	/* DL3 memory interface: the ring, S16 stereo, 48 kHz (code 10) */
	a22a_wr(afe, AFE_DL3_BASE_MSB, 0);
	a22a_wr(afe, AFE_DL3_BASE, A22A_SRAM_PHYS);
	a22a_wr(afe, AFE_DL3_END, A22A_SRAM_PHYS + A22A_RING_BYTES - 1);
	v = a22a_rd(afe, AFE_DL3_CON0);
	v &= ~((3u << 0) | (1u << 4) | (1u << 8) | (0xfu << 24));
	v |= (10u << 24);
	a22a_wr(afe, AFE_DL3_CON0, v);

	/* interconnect: DL3 CH1 -> ADDA_DL_CH1 (CONN3 bit 23), CH2 -> ADDA_DL_CH2 (CONN4 bit 24) */
	a22a_wr(afe, AFE_CONN3, a22a_rd(afe, AFE_CONN3) | (1u << 23));
	a22a_wr(afe, AFE_CONN4, a22a_rd(afe, AFE_CONN4) | (1u << 24));

	/* run: ADDA first, then the memif, then the AFE */
	a22a_wr(afe, AFE_ADDA_DL_SRC2_CON0, a22a_rd(afe, AFE_ADDA_DL_SRC2_CON0) | 1u);
	a22a_wr(afe, AFE_ADDA_UL_DL_CON0, a22a_rd(afe, AFE_ADDA_UL_DL_CON0) | 1u);
	a22a_wr(afe, AFE_DAC_CON0, a22a_rd(afe, AFE_DAC_CON0) | (1u << 6));
	a22a_wr(afe, AFE_DAC_CON0, a22a_rd(afe, AFE_DAC_CON0) | 1u);
	delay(2000);
	c0 = a22a_rd(afe, AFE_DL3_CUR);
	delay(2000);
	c1 = a22a_rd(afe, AFE_DL3_CUR);
	delay(5000);
	c2 = a22a_rd(afe, AFE_DL3_CUR);
	printf("a22aud: step 3: DAC_CON0 %#x DL3_CON0 %#x BASE %#x END %#x CONN3 %#x CONN4 %#x SRC2_CON0 %#x UL_DL_CON0 %#x\n",
	    a22a_rd(afe, AFE_DAC_CON0), a22a_rd(afe, AFE_DL3_CON0), a22a_rd(afe, AFE_DL3_BASE), a22a_rd(afe, AFE_DL3_END),
	    a22a_rd(afe, AFE_CONN3), a22a_rd(afe, AFE_CONN4), a22a_rd(afe, AFE_ADDA_DL_SRC2_CON0),
	    a22a_rd(afe, AFE_ADDA_UL_DL_CON0));
	printf("a22aud: step 3: DL3_CUR %#x -> %#x -> %#x (%s)\n", c0, c1, c2,
	    (c0 != c1 || c1 != c2) ? "MOVING: the memif reads the ring" : "STUCK");
}


/* ---- step 4: the MT6359P codec (LINEOUT L) and the SIA8109 amplifier ---- */
#define A22A_PMIF_WDATA         0x884
#define A22A_GPIO_PHYS          0x10005000u
#define A22A_GPIO_MODE(p)       (0x300 + ((p) / 8) * 0x10)
#define A22A_GPIO_DIR(p)        (0x000 + ((p) / 32) * 0x10)
#define A22A_GPIO_DOUT(p)       (0x100 + ((p) / 32) * 0x10)
#define A22A_AMP_PIN            169
#define A22A_AMP_MODE           6

static vm_offset_t a22a_gpio;
static uint32_t a22a_wfail;

static bool
a22a_pmic_write(uint32_t reg, uint32_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (a22a_pmif_wait(0)) {
		*(volatile uint32_t *)(a22a_pmif + A22A_PMIF_WDATA) = value & 0xffff;
		*(volatile uint32_t *)(a22a_pmif + A22A_PMIF_CMD) = (1u << 29) | reg;
		ok = a22a_pmif_wait(0);
	}
	redwood_pwrap_unlock();
	if (!ok) {
		a22a_wfail++;
	}
	return ok;
}

static void
a22a_pmic_bits(uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t now;

	if (!a22a_pmic_read(reg, &now)) {
		a22a_wfail++;
		return;
	}
	a22a_pmic_write(reg, (now & ~mask) | (value & mask));
}

static uint32_t
a22a_pmic_get(uint32_t reg)
{
	uint32_t v = 0xdead;

	(void)a22a_pmic_read(reg, &v);
	return v;
}

/* the pin's 4-bit function, set only if it differs; returns what it was */
static uint32_t
a22a_pin_mode(uint32_t pin, uint32_t func)
{
	uint32_t at = A22A_GPIO_MODE(pin), sh = (pin % 8) * 4, was = (a22a_rd(a22a_gpio, at) >> sh) & 0xfu;

	if (was != func) {
		a22a_wr(a22a_gpio, at, (a22a_rd(a22a_gpio, at) & ~(0xfu << sh)) | (func << sh));
	}
	return was;
}

static void
a22a_amp_line(bool high)
{
	uint32_t at = A22A_GPIO_DOUT(A22A_AMP_PIN) + (high ? 4 : 8);

	a22a_wr(a22a_gpio, at, 1u << (A22A_AMP_PIN % 32));
}

/* SIA8109 one-wire (AW8738-compatible): low > 1 ms, then N rising edges 2 us apart, then held high */
static void
a22a_amp_on(uint32_t mode)
{
	uint32_t i;
	boolean_t s;

	(void)a22a_pin_mode(A22A_AMP_PIN, 0);
	a22a_wr(a22a_gpio, A22A_GPIO_DIR(A22A_AMP_PIN) + 4, 1u << (A22A_AMP_PIN % 32));
	a22a_amp_line(false);
	delay(2000);
	s = ml_set_interrupts_enabled(FALSE);
	for (i = 0; i < mode; i++) {
		a22a_amp_line(false);
		delay(2);
		a22a_amp_line(true);
		delay(2);
	}
	ml_set_interrupts_enabled(s);
}

static void
a22a_amp_off(void)
{
	a22a_amp_line(false);
}

/*
 * The codec: what mt6359.c's DAPM does for AIF1 Playback -> DACL -> "LOL Mux" Playback_L_DAC, in its order
 * (supply sequence numbers), then mt_lo_event's PRE_PMU. lo_gain = ZCD_CON1 code (0 = +8 dB ... 0x12 = -10 dB).
 */
static void
a22a_codec_on(uint32_t lo_gain)
{
	/* codec_init_reg: clock source from the DCXO, LO short-circuit protection off, PMIC pad SMT/driving */
	a22a_pmic_bits(0x2536, 1u << 1, 0);                 /* AUDENC_ANA_CON23 CLKSQ_IN_SEL_TEST = 0 */
	a22a_pmic_bits(0x0032, 0x3ff0, 0x3ff0);             /* SMT_CON1 */
	a22a_pmic_bits(0x003c, 0xffff, 0x8888);             /* DRV_CON2..4 4 mA */
	a22a_pmic_bits(0x003e, 0xffff, 0x8888);
	a22a_pmic_bits(0x0040, 0x00ff, 0x88);
	a22a_pmic_write(0x2608, 0);                          /* ZCD_CON0: zero-cross off */
	/* dai startup: PMIC pads to MTKAIF MOSI (clk/data, sync) */
	a22a_pmic_write(0x00dc, 0x0ffe);
	a22a_pmic_write(0x00da, 0x0249);
	a22a_pmic_write(0x00e2, 0x6);
	a22a_pmic_write(0x00e0, 0x1);

	/* DL Power Supply, by sequence */
	a22a_pmic_bits(0x07a8, 1u << 13, 1u << 13);          /* CLK_BUF: DCXO_CW12 XO_AUDIO_EN_M */
	a22a_pmic_bits(0x25a2, 1u << 4, 0);                  /* AUDGLB: AUDDEC_ANA_CON13 PWRDN = 0 */
	a22a_pmic_bits(0x2536, 1u << 0, 1u << 0);            /* CLKSQ Audio */
	a22a_pmic_bits(0x230c, (1u << 6) | (1u << 5) | (1u << 2), 0); /* AUDNCP, ZCD13M, AUDIF clocks */
	a22a_pmic_bits(0x230c, 1u << 1, 0);                  /* AUD_CK (last) */
	delay(250);
	/* DL Digital Clock */
	a22a_pmic_bits(0x2396, (1u << 6) | (1u << 2) | (1u << 0), 0); /* AUDIO_TOP_CON0: DAC, PWR_CLK, RESERVED */
	a22a_pmic_bits(0x2396, 1u << 7, 0);                  /* AFE_CTL (last) */
	delay(250);
	a22a_pmic_bits(0x239e, 1u << 1, 1u << 1);            /* SDM_FIFO_CLK: AFUNC_AUD_CON2 */
	a22a_pmic_bits(0x239e, 0xfffd, 0x0006);              /* SDM: fifo clock, scrambler, power, fifo */
	a22a_pmic_write(0x239a, 0xcba1);
	a22a_pmic_bits(0x239e, 0xfffd, 0x0003);
	a22a_pmic_bits(0x239e, 0xfffd, 0x000b);
	a22a_pmic_write(0x24de, 0xc800);                     /* NCP pre */
	a22a_pmic_bits(0x24de, 1u << 0, 1u << 0);            /* NCP on */
	a22a_pmic_bits(0x2394, 1u << 0, 0);                  /* AFE_TOP_CON0 DL_SINE_ON = 0: normal path (MTKAIF) */
	a22a_pmic_bits(0x2388, 1u << 0, 1u << 0);            /* AFE_ON */
	a22a_pmic_bits(0x238a, 1u << 0, 1u << 0);            /* AFE_DL_SRC */
	/* rest of DL Power Supply */
	a22a_pmic_bits(0x258c, 1u << 8, 1u << 8);            /* ESD_RESIST */
	delay(260);
	a22a_pmic_bits(0x25a4, 1u << 2, 1u << 2);            /* LDO_REMOTE */
	a22a_pmic_bits(0x25a4, 1u << 0, 1u << 0);            /* LDO */
	a22a_pmic_bits(0x25a4, 1u << 4, 1u << 4);            /* NV_REGULATOR */
	delay(100);
	a22a_pmic_bits(0x25a0, 1u << 8, 0);                  /* IBIST: AUDIBIASPWRDN = 0 */

	/* mt_lo_event PRE_PMU, mux Playback_L_DAC */
	a22a_pmic_write(0x2596, 0x0010);
	a22a_pmic_bits(0x259e, 7u << 13, 2u << 13);          /* DRBIAS LO 6 uA */
	a22a_pmic_bits(0x25a0, 3u << 6, 1u << 6);            /* IBIAS ZCD 4 uA */
	a22a_pmic_bits(0x25a0, 3u << 4, 1u << 4);            /* IBIAS LO 5 uA */
	a22a_pmic_write(0x2596, 0x0110);
	a22a_pmic_write(0x2596, 0x0112);
	a22a_pmic_write(0x2596, 0x0113);
	a22a_pmic_write(0x260a, (lo_gain << 7) | lo_gain);   /* ZCD_CON1 LOL/LOR gain */
	a22a_pmic_bits(0x25a2, 1u << 0, 1u << 0);            /* decoder clock: RSTB_DECODER */
	a22a_pmic_write(0x2588, 0x3009);                     /* DACL on, HP mux open */
	a22a_pmic_write(0x259a, 0xf200);
	delay(120);
	a22a_pmic_write(0x2596, 0x0117);                     /* LOL mux = DACL */
}

static void
a22a_step4(vm_offset_t afe, uint32_t lo_gain, uint32_t secs)
{
	uint32_t m[4], i, c0, c1;

	a22a_gpio = ml_io_map(A22A_GPIO_PHYS, 0x1000);
	/* SoC side of MTKAIF MOSI: GPIO152 clk, 153 sync, 154/155 data, function 1 */
	for (i = 0; i < 4; i++) {
		m[i] = a22a_pin_mode(152 + i, 1);
	}
	printf("a22aud: step 4: MOSI pins 152..155 were mode %u %u %u %u, now %#x\n", m[0], m[1], m[2], m[3],
	    a22a_rd(a22a_gpio, A22A_GPIO_MODE(152)));

	a22a_wfail = 0;
	a22a_codec_on(lo_gain);
	printf("a22aud: step 4: codec: %u PMIF failures; CKPDN %#x TOP_CON0 %#x AFUNC2 %#x NCP %#x UL_DL %#x SRC2_L %#x\n",
	    a22a_wfail, a22a_pmic_get(0x230c), a22a_pmic_get(0x2396), a22a_pmic_get(0x239e), a22a_pmic_get(0x24de),
	    a22a_pmic_get(0x2388), a22a_pmic_get(0x238a));
	printf("a22aud: step 4: codec: DEC_CON0 %#x CON7 %#x CON9 %#x CON13 %#x CON14 %#x ZCD_CON1 %#x CW12 %#x\n",
	    a22a_pmic_get(0x2588), a22a_pmic_get(0x2596), a22a_pmic_get(0x259a), a22a_pmic_get(0x25a2),
	    a22a_pmic_get(0x25a4), a22a_pmic_get(0x260a), a22a_pmic_get(0x07a8));

	a22a_amp_on(A22A_AMP_MODE);
	c0 = a22a_rd(afe, 0x94);
	printf("a22aud: step 4: amplifier on (GPIO169 mode %u, DOUT %#x): 1 kHz tone for %u s, LO gain code %#x\n",
	    A22A_AMP_MODE, a22a_rd(a22a_gpio, A22A_GPIO_DOUT(A22A_AMP_PIN)), secs, lo_gain);
	for (i = 0; i < secs * 10; i++) {
		delay(100000);
	}
	c1 = a22a_rd(afe, 0x94);
	a22a_amp_off();
	a22a_pmic_write(0x2596, 0x0113);                     /* LOL mux open */
	printf("a22aud: step 4: tone done, amplifier off; DL3_CUR %#x -> %#x\n", c0, c1);
}


/*
 * ---- step 5: what iOS plays (/dev/redwood-audio) to the loudspeaker ----
 * The DL3 ring (8192 frames, S16 stereo, 48 kHz) plays on forever. The sink writes iOS's frames a little ahead of
 * the hardware's read pointer; a thread clears what the hardware has already read (so a gap is silence, not an
 * echo), turns the amplifier on when sound comes and off after 3 s of silence, and follows iOS's volume with the
 * codec's LINEOUT gain (1 dB a step; ZCD code = a22aud_gain + iOS's dB under the loudest; 0x1f and above = muted).
 */
#define A22A_RING5_FRAMES   8192u
#define A22A_LEAD_FRAMES    1920u       /* 40 ms ahead of the hardware */
#define A22A_MAX_AHEAD      7936u       /* up to 165 ms queued: the writers start with a burst ahead of real time */

static vm_offset_t a22a5_afe;
static volatile uint32_t *a22a5_ring;   /* one uint32_t = one frame */
static uint32_t a22a5_w;                /* next frame to write (mod ring) */
static volatile uint64_t a22a5_last_sound;
static uint32_t a22a5_base_gain, a22a5_lost;

static uint32_t
a22a5_hw(void)
{
	return ((a22a_rd(a22a5_afe, 0x94) - A22A_SRAM_PHYS) / 4) % A22A_RING5_FRAMES;
}

static uint64_t
a22a5_ms_since(uint64_t then)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - then, &ns);
	return ns / 1000000;
}

static uint32_t
a22a5_sink(const int16_t *frames, uint32_t count)
{
	uint32_t hw = a22a5_hw(), ahead = (a22a5_w + A22A_RING5_FRAMES - hw) % A22A_RING5_FRAMES, room, i;
	const uint32_t *f = (const uint32_t *)(const void *)frames;
	bool loud = false;

	static uint64_t last_write;
	bool fresh = last_write == 0 || a22a5_ms_since(last_write) > 200;

	last_write = mach_absolute_time();
	if (fresh || ahead < 64 || ahead > A22A_MAX_AHEAD) {
		/* behind the hardware (underrun) or never started: start again just ahead of it */
		a22a5_w = (hw + A22A_LEAD_FRAMES) % A22A_RING5_FRAMES;
		ahead = A22A_LEAD_FRAMES;
	}
	room = ahead < A22A_MAX_AHEAD ? A22A_MAX_AHEAD - ahead : 0;
	if (count > room) {
		a22a5_lost += count - room;
		count = room;
	}
	for (i = 0; i < count; i++) {
		int16_t l = frames[2 * i], r = frames[2 * i + 1];

		if (l > 8 || l < -8 || r > 8 || r < -8) {
			loud = true;
		}
		a22a5_ring[a22a5_w] = f[i];
		a22a5_w = (a22a5_w + 1) % A22A_RING5_FRAMES;
	}
	if (loud) {
		a22a5_last_sound = mach_absolute_time();
	}
	return count;
}


static void
a22a5_set_gain(uint32_t below, bool muted)
{
	uint32_t code = a22a5_base_gain + (below + 4) / 8;

	if (muted || code > 0x1e) {
		code = 0x1f;
	}
	a22a_pmic_write(0x260a, (code << 7) | code);
	printf("a22aud: step 5: volume %s%u dB under the loudest -> LINEOUT code %#x\n", muted ? "muted, " : "", below / 8, code);
}

static void
a22a5_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t prev = a22a5_hw(), gen = 0, below = 0, starts = 0;
	uint64_t played = 0, t0 = mach_absolute_time(), tstart = t0;
	uint32_t quiet = 90;   /* a22aud_quiet: seconds after boot with the amplifier kept off (the boot sound) */

	(void)PE_parse_boot_argn("a22aud_quiet", &quiet, sizeof(quiet));
	bool amp = false, muted = false;

	for (;;) {
		uint32_t hw = a22a5_hw(), g, b;
		bool m;

		/* clear what the hardware has read since the last look */
		while (prev != hw) {
			a22a5_ring[prev] = 0;
			prev = (prev + 1) % A22A_RING5_FRAMES;
			played++;
		}
		if (a22a5_ms_since(t0) >= 10000) {
			/* how fast the hardware really reads, by the system's clock: iOS writes 48000 a second by that clock */
			printf("a22aud: step 5: DL3 reads %u frames a second (by mach time)\n", (uint32_t)(played * 1000 / a22a5_ms_since(t0)));
			played = 0;
			t0 = mach_absolute_time();
		}
		if (redwood_audio_volume(&b, &m, &g) && g != gen) {
			gen = g;
			below = b;
			muted = m;
			a22a5_set_gain(below, muted);
		}
		if (!amp && !muted && a22a5_last_sound != 0 && a22a5_ms_since(a22a5_last_sound) < 100 &&
		    a22a5_ms_since(tstart) >= (uint64_t)quiet * 1000) {
			a22a_amp_on(A22A_AMP_MODE);
			amp = true;
			starts++;
			printf("a22aud: step 5: iOS plays: amplifier on (%u)\n", starts);
		} else if (amp && (muted || a22a5_ms_since(a22a5_last_sound) > 3000)) {
			a22a_amp_off();
			amp = false;
			printf("a22aud: step 5: 3 s of silence: amplifier off (%u frames dropped so far)\n", a22a5_lost);
		}
		delay_for_interval(5, NSEC_PER_MSEC);
	}
}

static void
a22a_step5(vm_offset_t afe, uint32_t base_gain)
{
	thread_t t;
	uint32_t i;

	a22a5_afe = afe;
	a22a5_base_gain = base_gain;
	a22a_gpio = ml_io_map(A22A_GPIO_PHYS, 0x1000);   /* the amplifier's pin (step 4 maps it there; not run here) */
	a22a5_ring = (volatile uint32_t *)ml_io_map(A22A_SRAM_PHYS, 0x8000);
	for (i = 0; i < A22A_RING5_FRAMES; i++) {
		a22a5_ring[i] = 0;
	}
	/* the ring is now the whole 32 KB */
	a22a_wr(afe, AFE_DL3_END, A22A_SRAM_PHYS + A22A_RING5_FRAMES * 4 - 1);
	if (kernel_thread_start(a22a5_thread, NULL, &t) != KERN_SUCCESS) {
		printf("a22aud: step 5: no thread\n");
		return;
	}
	thread_deallocate(t);
	a22a5_set_gain(0, false);
	redwood_audio_set_sink("the loudspeaker (A22: DL3 -> MT6359P LINEOUT L -> SIA8109, redwood_a22audio.c)", a22a5_sink);
	printf("a22aud: step 5: WHAT IOS PLAYS GOES TO THE LOUDSPEAKER, loudest = LINEOUT code %#x\n", base_gain);
}

void
redwood_a22audio_init(void)
{
	uint32_t step = 0;

	if (!PE_parse_boot_argn("a22aud_step", &step, sizeof(step)) || step == 0) {
		return;
	}
	if (step >= 1) {
		a22a_step1();
	}
	if (step >= 2) {
		vm_offset_t afe = a22a_step2();

		if (step >= 3 && afe) {
			a22a_step3(afe, step >= 5);
			if (step == 4) {
				uint32_t gain = 0x12, secs = 5;

				(void)PE_parse_boot_argn("a22aud_gain", &gain, sizeof(gain));
				(void)PE_parse_boot_argn("a22aud_secs", &secs, sizeof(secs));
				if (gain < 0x0c) {
					gain = 0x0c;   /* the test tone: never louder than -4 dB */
				}
				if (gain > 0x1f) {
					gain = 0x1f;
				}
				if (secs > 20) {
					secs = 20;
				}
				a22a_step4(afe, gain, secs);
			} else if (step >= 5) {
				uint32_t gain = 0x04;   /* +4 dB at iOS's loudest (Android plays music at code 0 = +8 dB) */

				(void)PE_parse_boot_argn("a22aud_gain", &gain, sizeof(gain));
				if (gain > 0x1e) {
					gain = 0x1e;
				}
				a22a_codec_on(0x1f);
				a22a_step5(afe, gain);
			}
		}
	}
}
