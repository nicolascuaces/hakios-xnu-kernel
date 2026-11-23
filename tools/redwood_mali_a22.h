/*
 * The GPU of the Samsung Galaxy A22 5G (MT6833): a Mali-G57 MC2 (Valhall, job manager), brought up
 * from the kernel. Included by bsd/dev/redwood_mali.c and used with boot-arg mali=2 instead of the
 * Infinix's mali_up(); everything after the power-up (the GPU's registers, the first two jobs, the
 * page tables) is redwood_mali.c's own, the job-manager interface being the same on Bifrost and on
 * this Valhall.
 *
 * Where each value comes from: the Linux mainline port of this very phone, a22x-mainline
 * (gitlab.com/pessdres/a22x-mainline, patches 0013, 0024, 0025 and 0045), which runs this GPU with
 * panfrost; its addresses and order are those of the Samsung SM-A226B kernel.
 *
 *   1  the rails, from the MT6359P through the PMIF: VSRAM_MD (here the GPU's SRAM) to 0.85 V, then
 *      VPROC1 (the GPU) to 0.78125 V, each enabled. Not lower: the vendor's 0.625 V and up need a
 *      calibration (EEMG) that is not done, and hung the GPU every second boot under Linux.
 *   2  the clock: mfg_ref_sel to mainpll_d6 (364 MHz) and mfg_pll_sel to it, not to MFGPLL
 *   3  the four power domains in the SPM, MFG0..MFG3, each with its SRAM; after MFG1 the bus
 *      protection released, MT8192's masks, and last the MFG1 AXI "way" in infra_pdn opened
 *   4  BG3D, the block's clock gate (mfgcfg 0x13fbf000)
 *   5  the GPU at 0x13000000, then redwood_mali.c's steps 4 to 6
 *
 * Programs are not given the GPU (RedwoodMaliStart) unless boot-arg mali_start=1: the Mesa in the
 * image is built for the Infinix's Bifrost (v7) alone.
 */

#define A22_TOPCKGEN_PHYS   0x10000000
#define A22_INFRACFG_PHYS   0x10001000
#define A22_SPM_PHYS        0x10006000
#define A22_PMIF_PHYS       0x10026000
#define A22_INFRA_PDN_PHYS  0x10215000
#define A22_MFGCFG_PHYS     0x13fbf000
#define A22_MALI_PHYS       0x13000000

#define A22_PMIF_CMD        0x880
#define A22_PMIF_WDATA      0x884
#define A22_PMIF_RDATA      0x894
#define A22_PMIF_VLDCLR     0x8a4
#define A22_PMIF_STATE      0x8a8
#define A22_PMIF_FSM(v)     (((v) >> 1) & 7u)

#define MT6359_VPROC1_CON0      0x1708          /* bit 0: enable */
#define MT6359_VPROC1_DBG0      0x1726          /* what it is at, 7 bits */
#define MT6359_VPROC1_DBG1      0x1728          /* bit 0: on */
#define MT6359_VPROC1_ELR0      0x172e          /* where it is set: 0.4 V + 6.25 mV a step */
#define MT6359P_VSRAM_MD_ELR    0x1b4a          /* 0.5 V + 6.25 mV a step */
#define MT6359P_VSRAM_MD_CON0   0x1f2e          /* bit 0: enable */
#define MT6359P_VSRAM_MD_MON    0x1f32          /* bit 0: on */
#define MT6359P_VSRAM_MD_VOSEL1 0x1f36          /* what it is at, bits 14:8 */

#define A22_VGPU_UV         781250
#define A22_VSRAM_GPU_UV    850000

#define A22_PWR_STATUS      0x16c
#define A22_PWR_STATUS_2ND  0x170

static vm_offset_t a22_pmif, a22_infra_pdn;
static uint32_t mali_mode;                      /* boot-arg mali: 1 the Infinix, 2 this phone */

static bool
a22_pmif_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (A22_PMIF_FSM(rd(a22_pmif, A22_PMIF_STATE)) == state) {
			return true;
		}
	}
	return false;
}

static bool
a22_pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (a22_pmif_wait(0)) {
		wr(a22_pmif, A22_PMIF_CMD, reg);
		if (a22_pmif_wait(6)) {
			*value = rd(a22_pmif, A22_PMIF_RDATA) & 0xffff;
			wr(a22_pmif, A22_PMIF_VLDCLR, 1);
			ok = true;
		}
	} else if (A22_PMIF_FSM(rd(a22_pmif, A22_PMIF_STATE)) == 6) {
		wr(a22_pmif, A22_PMIF_VLDCLR, 1);
	}
	redwood_pwrap_unlock();
	return ok;
}

static bool
a22_pmic_write(uint32_t reg, uint32_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (a22_pmif_wait(0)) {
		wr(a22_pmif, A22_PMIF_WDATA, value & 0xffff);
		wr(a22_pmif, A22_PMIF_CMD, (1u << 29) | reg);
		ok = a22_pmif_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* the 7-bit setting at `reg` to `uv`, the rest of the register kept; the enable bit at `en` set */
static bool
a22_rail(const char *name, uint32_t vosel_reg, uint32_t base_uv, uint32_t uv, uint32_t en_reg)
{
	uint32_t v = 0, en = 0, sel = (uv - base_uv + 6249) / 6250;

	if (!a22_pmic_read(vosel_reg, &v) || !a22_pmic_read(en_reg, &en)) {
		printf("mali: %s: the PMIC does not answer\n", name);
		return false;
	}
	printf("mali: %s was at %u mV (%s), to %u mV\n", name, (base_uv + 6250 * (v & 0x7f)) / 1000,
	    (en & 1) ? "on" : "off", (base_uv + 6250 * sel) / 1000);
	if (!a22_pmic_write(vosel_reg, (v & ~0x7fu) | sel) || !a22_pmic_write(en_reg, en | 1)) {
		printf("mali: %s: could not be written\n", name);
		return false;
	}
	delay(300);                                     /* the ramp: 240 us at most, Linux says */
	return a22_pmic_read(vosel_reg, &v) && (v & 0x7f) == sel;
}

static const struct {
	const char *name;
	uint32_t con, bit;
} a22_domains[4] = {
	{ "MFG0", 0x308, 1u << 2 },
	{ "MFG1", 0x30c, 1u << 3 },
	{ "MFG2", 0x310, 1u << 4 },
	{ "MFG3", 0x314, 1u << 5 },
};

static bool
a22_domain_on(unsigned i)
{
	uint32_t con = a22_domains[i].con, bit = a22_domains[i].bit;

	wr(spm, con, rd(spm, con) | PWR_ON);
	wr(spm, con, rd(spm, con) | PWR_ON_2ND);
	if (!wait_for(spm, A22_PWR_STATUS, bit, bit, 20) || !wait_for(spm, A22_PWR_STATUS_2ND, bit, bit, 20)) {
		return false;
	}
	wr(spm, con, rd(spm, con) & ~PWR_CLK_DIS);
	wr(spm, con, rd(spm, con) & ~PWR_ISO);
	wr(spm, con, rd(spm, con) | PWR_RST_B);
	wr(spm, con, rd(spm, con) & ~PWR_SRAM_PDN);
	return wait_for(spm, con, PWR_SRAM_ACK, 0, 20);
}

/* one bus protection released: its mask to the CLR register, and the status reading it clear */
static bool
a22_bus_release(uint32_t clr, uint32_t sta, uint32_t mask)
{
	wr(infracfg, clr, mask);
	return wait_for(infracfg, sta, mask, 0, 20);
}

/*
 * Boot-arg mali_mhz=N (390..955): the GPU from MFGPLL at N MHz instead of mainpll_d6 (364 MHz).
 * MFGPLL (apmixedsys 0x1000c000: CON0 0x268 bit 0 enable, CON1 0x26c, CON3 0x274 power and
 * isolation) is programmed while the GPU still runs from mfg_ref_sel, and only then mfg_pll_sel
 * (CLK_CFG_4 bit 18) moves to it. CON1: bit 31 asks for the change, bits 26..24 the post divider
 * (2^n), bits 21..0 the PCW (26 MHz * PCW / 2^14). 955 MHz is the most this chip's MFGPLL takes
 * (a22x-mainline patch 0027); the 0.78125 V of step 1 is that patch's voltage for 955 MHz.
 */
#define A22_APMIXED_PHYS    0x1000c000
#define A22_MFGPLL_CON0     0x268
#define A22_MFGPLL_CON1     0x26c
#define A22_MFGPLL_CON3     0x274

static uint32_t a22_mfgpll_mhz(uint32_t con1)
{
	uint32_t post = (con1 >> 24) & 7u;
	return (uint32_t)(((uint64_t)(con1 & 0x3fffffu) * 26u / 16384u) >> post);
}

static void
a22_mfgpll(void)
{
	vm_offset_t apmixed, top;
	uint32_t mhz = 0, post, pcw, before0, before1;

	(void)PE_parse_boot_argn("mali_mhz", &mhz, sizeof(mhz));
	apmixed = ml_io_map(A22_APMIXED_PHYS, 0x1000);
	top = ml_io_map(A22_TOPCKGEN_PHYS, 0x1000);
	before0 = rd(apmixed, A22_MFGPLL_CON0);
	before1 = rd(apmixed, A22_MFGPLL_CON1);
	printf("mali: MFGPLL was %s at %u MHz (CON0 %#x CON1 %#x CON3 %#x)\n", (before0 & 1) ? "on" : "off",
	    a22_mfgpll_mhz(before1), before0, before1, rd(apmixed, A22_MFGPLL_CON3));
	if (mhz == 0) {
		printf("mali: the GPU at 364 MHz (mainpll_d6); boot-arg mali_mhz=N for MFGPLL\n");
		return;
	}
	if (mhz < 390 || mhz > 955) {
		printf("mali: mali_mhz=%u outside 390..955, left at 364 MHz\n", mhz);
		return;
	}
	post = mhz >= 750 ? 1u : 2u;                    /* the VCO between 1500 and 3800 MHz */
	pcw = (uint32_t)(((uint64_t)mhz << post) * 16384u / 26u);
	if (!(rd(apmixed, A22_MFGPLL_CON3) & 1u)) {     /* power, then isolation off */
		wr(apmixed, A22_MFGPLL_CON3, rd(apmixed, A22_MFGPLL_CON3) | 1u);
		delay(30);
		wr(apmixed, A22_MFGPLL_CON3, rd(apmixed, A22_MFGPLL_CON3) & ~2u);
		delay(1);
	}
	wr(apmixed, A22_MFGPLL_CON1, (rd(apmixed, A22_MFGPLL_CON1) & ~((7u << 24) | 0x3fffffu)) | (1u << 31) | (post << 24) | pcw);
	wr(apmixed, A22_MFGPLL_CON0, rd(apmixed, A22_MFGPLL_CON0) | 1u);
	delay(40);
	wr(top, 0x54, 1u << 18);                        /* CLK_CFG_4 SET: mfg_pll_sel = MFGPLL */
	delay(10);
	printf("mali: the GPU now from MFGPLL at %u MHz (CON0 %#x CON1 %#x, CLK_CFG_4 %#x)\n",
	    a22_mfgpll_mhz(rd(apmixed, A22_MFGPLL_CON1)), rd(apmixed, A22_MFGPLL_CON0), rd(apmixed, A22_MFGPLL_CON1),
	    rd(top, 0x50));
}

static void
mali_up_a22(void)
{
	uint32_t shaders, tiler, l2;

	topckgen = ml_io_map(A22_TOPCKGEN_PHYS, 0x1000);
	infracfg = ml_io_map(A22_INFRACFG_PHYS, 0x1000);
	spm = ml_io_map(A22_SPM_PHYS, 0x1000);
	a22_pmif = ml_io_map(A22_PMIF_PHYS, 0x1000);
	a22_infra_pdn = ml_io_map(A22_INFRA_PDN_PHYS, 0x1000);

	/* 1: the rails, SRAM first */
	if (!a22_rail("VSRAM_GPU (VSRAM_MD)", MT6359P_VSRAM_MD_ELR, 500000, A22_VSRAM_GPU_UV, MT6359P_VSRAM_MD_CON0) ||
	    !a22_rail("VGPU (VPROC1)", MT6359_VPROC1_ELR0, 400000, A22_VGPU_UV, MT6359_VPROC1_CON0)) {
		printf("mali: the GPU's rails are not as asked; left off\n");
		return;
	}

	/* 2: the clock (CLK_CFG_4: mfg_ref_sel bits 17:16, its gate bit 23, mfg_pll_sel bit 18) */
	printf("mali: CLK_CFG_4 %#x, SPM status %#x / %#x, before\n", rd(topckgen, 0x50), rd(spm, A22_PWR_STATUS),
	    rd(spm, A22_PWR_STATUS_2ND));
	wr(topckgen, 0x58, (3u << 16) | (1u << 18) | (1u << 23));      /* CLR */
	wr(topckgen, 0x54, 2u << 16);                                   /* SET: mainpll_d6 */
	wr(topckgen, 0x04, 1u << 18);                                   /* CLK_CFG_UPDATE: mfg_ref_sel */
	a22_mfgpll();
	reached = 1;

	/* 3: power. The SPM takes writes only with its project code in POWERON_CONFIG_EN */
	if (!(rd(spm, 0) & 1)) {
		wr(spm, 0, (0xb16u << 16) | 1u);
	}
	for (unsigned i = 0; i < 4; i++) {
		if (!a22_domain_on(i)) {
			printf("mali: power domain %s did not come up (status %#x / %#x, its register %#x)\n",
			    a22_domains[i].name, rd(spm, A22_PWR_STATUS), rd(spm, A22_PWR_STATUS_2ND), rd(spm, a22_domains[i].con));
			return;
		}
		if (i == 1) {
			/* in the reverse of the order Linux closes them; the way last */
			if (!a22_bus_release(0x718, 0x724, 1u << 7) ||                     /* EN_2, MFG1 2nd */
			    !a22_bus_release(0x2a4, 0x228, (1u << 21) | (1u << 22)) ||     /* EN */
			    !a22_bus_release(0x718, 0x724, (1u << 5) | (1u << 6)) ||       /* EN_2 */
			    !a22_bus_release(0x2ac, 0x258, 1u << 21)) {                    /* EN_1 */
				printf("mali: the bus protection did not let go (%#x %#x %#x)\n", rd(infracfg, 0x228),
				    rd(infracfg, 0x258), rd(infracfg, 0x724));
				return;
			}
			wr(a22_infra_pdn, 0x4c, rd(a22_infra_pdn, 0x4c) | (1u << 11));    /* the MFG1 way open */
			if (!wait_for(a22_infra_pdn, 0x4c, 1u << 11, 1u << 11, 20)) {
				printf("mali: the MFG1 way did not open (%#x)\n", rd(a22_infra_pdn, 0x4c));
				return;
			}
		}
	}
	reached = 2;
	printf("mali: MFG0..3 on (status %#x / %#x)\n", rd(spm, A22_PWR_STATUS), rd(spm, A22_PWR_STATUS_2ND));

	/* 4: inside the block */
	mfgcfg = ml_io_map(A22_MFGCFG_PHYS, 0x1000);
	mali = ml_io_map(A22_MALI_PHYS, 0x4000);
	wr(mfgcfg, 0x008, 1u);                  /* MFG_CG_CLR: BG3D runs */
	reached = 3;

	/* 5: the GPU itself, as redwood_mali.c does it */
	gpu_id = rd(mali, GPU_ID);
	shaders = shader_mask = rd(mali, SHADER_PRESENT);
	tiler = rd(mali, TILER_PRESENT);
	l2 = rd(mali, L2_PRESENT);
	printf("mali: GPU_ID %#x (arch %u, product %#x, r%up%u), MMU_FEATURES %#x, shaders %#x, tiler %#x, L2 %#x\n",
	    gpu_id, gpu_id >> 28, gpu_id >> 16, (gpu_id >> 12) & 0xf, (gpu_id >> 4) & 0xff, rd(mali, 0x014), shaders, tiler, l2);
	if (gpu_id == 0 || gpu_id == 0xffffffffu) {
		printf("mali: no GPU answers at %#x\n", A22_MALI_PHYS);
		return;
	}
	wr(mali, GPU_INT_MASK, 0);
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, GPU_CMD, 1);                   /* soft reset */
	if (!wait_for(mali, GPU_INT_RAWSTAT, IRQ_RESET_COMPLETED, IRQ_RESET_COMPLETED, 100)) {
		printf("mali: GPU_ID %#x, but it did not finish its reset\n", gpu_id);
		return;
	}
	wr(mali, GPU_INT_CLEAR, 0xffffffffu);
	wr(mali, L2_PWRON, l2);
	wr(mali, TILER_PWRON, tiler);
	wr(mali, SHADER_PWRON, shaders);
	if (!wait_for(mali, L2_READY, l2, l2, 50) || !wait_for(mali, TILER_READY, tiler, tiler, 50) ||
	    !wait_for(mali, SHADER_READY, shaders, shaders, 50)) {
		printf("mali: GPU_ID %#x, but its cores did not all power up (L2 %#x, tiler %#x, shaders %#x ready)\n", gpu_id,
		    rd(mali, L2_READY), rd(mali, TILER_READY), rd(mali, SHADER_READY));
		return;
	}
	reached = 4;
	printf("mali: the Mali-G57 is powered: shader cores %#x, tiler %#x, L2 %#x\n", shaders, tiler, l2);

	switch (first_jobs()) {
	case 2:
		reached = 6;
		break;
	case 1:
		reached = 5;
		break;
	default:
		break;
	}
}
