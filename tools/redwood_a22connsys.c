/*
 * CONNSYS (Wi-Fi/Bluetooth/GPS) of the Samsung Galaxy A22 5G's MT6833.
 * Boot-arg a22conn_step=1: SPM's power state of CONNSYS, and only if both its domains say "on", the
 * CONNSYS id registers; a22conn_step=2 also reads its supplies on the PMIC MT6359P (1 and 2 write nothing).
 * a22conn_step=3 TURNS ON the two supplies Android turns on for this phone's radio die (the MT6631, the
 * "primary A-die" of mt6833.c), in its legacy (not RC) mode: VCN18 (fixed 1.8 V), then after 300 us VCN33_1
 * set to 3.3 V (VOSEL 0xd: vcn33_voltages[] of mt6359-regulator.c; LK leaves 0xf = 3.5 V) and on. Its
 * SW_EN only: the low-power and hardware-op bits are left as they are. Off again at the next reboot.
 *
 * Every value is from MediaTek's own driver for this SoC as Samsung ships it (GPL SM-A226B
 * A226BXXSBDYA1), as carried by gitlab.com/pessdres/a22x-mainline conn/:
 *   common_main/platform/mt6833.c, include/mt6833.h, and the stock device tree's consys@18002000
 *   ("mediatek,mt6833-consys": mcu 0x18002000, spm 0x10006000, conn_hif_on 0x18007000,
 *   mcu_top_misc_off 0x180b1000, mcu_cfg_on 0x180a3000, top_misc_on 0x180c1000, ...).
 *   SPM +0x304 TOP1_PWR_CTRL (bit 0 reset released, 1 isolation, 2 on, 3 on (2nd), 4 clock, 8 SRAM pd)
 *       +0x16c PWR_CONN_ACK   bit 1 CONNSYS on, bit 30 TOP2 on;  +0x170 the same, 2nd half
 *       +0x178 CON_TOP_OFF_CHECK bit 1
 *   MCU_TOP_MISC_OFF +0x10 CONSYS_IP_VER (0x100a0000 on this chip), +0x1c CONF_ID
 *   MCU +0x00 hardware version, +0x04 ROM version
 *   PMIC MT6359P: VCN13 0x1c1c, VCN18 0x1c2e, VCN33_1 0x1be2, VCN33_2 0x1c08 (CON0, bit 0 enable)
 * The CONNSYS blocks are read only when SPM says both domains are on: a read of a powered-off block of
 * this kind can hang the bus. LK does not power CONNSYS on, so straight from LK it is expected off.
 */
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <kern/thread.h>
#include <kern/kalloc.h>
#include <kern/clock.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority, thread_t *new_thread);
extern void delay_for_interval(uint32_t interval, uint32_t scale_factor);
extern kern_return_t thread_terminate(thread_t thread);

#define A22C_SPM_PHYS           0x10006000u
#define A22C_TOP1_PWR_CTRL      0x304
#define A22C_PWR_ACK            0x16c
#define A22C_PWR_ACK_S          0x170
#define A22C_TOP_OFF_CHECK      0x178
#define A22C_ACK_ON             (1u << 1)
#define A22C_ACK_TOP2           (1u << 30)
#define A22C_MCU_PHYS           0x18002000u
#define A22C_MISC_OFF_PHYS      0x180b1000u
#define A22C_IP_VER_ID          0x100a0000u

#define A22C_PMIF_PHYS          0x10026000u
#define A22C_PMIF_CMD           0x880
#define A22C_PMIF_WDATA         0x884
#define A22C_PMIF_RDATA         0x894
#define A22C_PMIF_VLDCLR        0x8a4
#define A22C_PMIF_STATE         0x8a8
#define A22C_PMIF_FSM(v)        (((v) >> 1) & 7u)

static vm_offset_t a22c_pmif;

static uint32_t
a22c_rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static bool
a22c_pmif_wait(uint32_t state)
{
	for (uint32_t spin = 0; spin < 200000; spin++) {
		if (A22C_PMIF_FSM(a22c_rd(a22c_pmif, A22C_PMIF_STATE)) == state) {
			return true;
		}
	}
	return false;
}

static bool
a22c_pmic_read(uint32_t reg, uint32_t *value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (a22c_pmif_wait(0)) {
		*(volatile uint32_t *)(a22c_pmif + A22C_PMIF_CMD) = reg;
		if (a22c_pmif_wait(6)) {
			*value = a22c_rd(a22c_pmif, A22C_PMIF_RDATA) & 0xffff;
			*(volatile uint32_t *)(a22c_pmif + A22C_PMIF_VLDCLR) = 1;
			ok = true;
		}
	} else if (A22C_PMIF_FSM(a22c_rd(a22c_pmif, A22C_PMIF_STATE)) == 6) {
		*(volatile uint32_t *)(a22c_pmif + A22C_PMIF_VLDCLR) = 1;
	}
	redwood_pwrap_unlock();
	return ok;
}

static bool
a22c_pmic_write(uint32_t reg, uint32_t value)
{
	bool ok = false;

	redwood_pwrap_lock();
	if (a22c_pmif_wait(0)) {
		*(volatile uint32_t *)(a22c_pmif + A22C_PMIF_WDATA) = value & 0xffff;
		*(volatile uint32_t *)(a22c_pmif + A22C_PMIF_CMD) = (1u << 29) | reg;
		ok = a22c_pmif_wait(0);
	}
	redwood_pwrap_unlock();
	return ok;
}

/* bits under `mask` of a PMIC register set to `value`; reads it back into *after */
static bool
a22c_pmic_update(uint32_t reg, uint32_t mask, uint32_t value, uint32_t *after)
{
	uint32_t now;

	if (!a22c_pmic_read(reg, &now) || !a22c_pmic_write(reg, (now & ~mask) | (value & mask))) {
		return false;
	}
	return a22c_pmic_read(reg, after);
}

static void
a22c_supplies_on(void)
{
	uint32_t rc = 0, v18 = 0, vosel = 0, v33 = 0;
	vm_offset_t srclken = ml_io_map(0x1000f800u, 0x1000);
	bool ok;

	rc = a22c_rd(srclken, 0x4);
	printf("a22conn: step 3: SRCLKEN_RC CENTRAL_CFG1 %#x: RC mode %s\n", rc, (rc & 1) ? "ON (Android would use its RC path)" :
	    "off (legacy, as written here)");
	ok = a22c_pmic_update(0x1c2e, 1u, 1u, &v18);                         /* VCN18 SW_EN */
	delay(300);
	ok = ok && a22c_pmic_update(0x1f98, 0xfu << 8, 0xdu << 8, &vosel);    /* VCN33_1 3.3 V */
	ok = ok && a22c_pmic_update(0x1be2, 1u, 1u, &v33);                    /* VCN33_1 SW_EN_0 */
	delay(300);
	/* VCN28 of mt6833.c = VCN33_2 at 2.8 V (VOSEL 9: vcn33_voltages[] of mt6359-regulator.c), the "efuse PALDO" Android
	 * turns on before CONNSYS is powered and off after the RF calibration (mtk_wcn_soc_sw_init: "turn off VCN28 after
	 * reading efuse"). Run ios27: without it the firmware ASSERTed during the calibration. Left on here. */
	{
		uint32_t v28 = 0, v28sel = 0;
		bool ok28 = a22c_pmic_update(0x1f9c, 0xfu << 8, 0x9u << 8, &v28sel) && a22c_pmic_update(0x1c08, 1u, 1u, &v28);

		delay(300);
		printf("a22conn: step 3: %s: VCN33_2 (VCN28) ANA_CON0 %#x (VOSEL %#x%s), CON0 %#x (%s)\n",
		    ok28 ? "written" : "THE WRAPPER DID NOT ANSWER", v28sel, (v28sel >> 8) & 0xf,
		    ((v28sel >> 8) & 0xf) == 9 ? " = 2.8 V" : " NOT 2.8 V", v28, (v28 & 1) ? "on" : "OFF");
	}
	printf("a22conn: step 3: %s: VCN18 CON0 %#x (%s), VCN33_1 ANA_CON0 %#x (VOSEL %#x%s), CON0 %#x (%s)\n",
	    ok ? "written" : "THE WRAPPER DID NOT ANSWER", v18, (v18 & 1) ? "on" : "OFF", vosel, (vosel >> 8) & 0xf,
	    ((vosel >> 8) & 0xf) == 0xd ? " = 3.3 V" : " NOT 3.3 V", v33, (v33 & 1) ? "on" : "OFF");
}

/*
 * Step 4: CONNSYS power-on, the raw (no power-domain API) path of consys_hw_power_ctrl() in mt6833.c,
 * in the order of mtk_wcn_consys_hw_pwr_on(): hold the CONNSYS CPU in reset (AP_RGU +0x18 bit 12, key 0x88),
 * SPM register clock on (SPM +0x0 bit 0, key 0x0b16), TOP1 on / on2 with their acks, clock gate off,
 * isolation off, RGU bit 9 cleared, SPM reset released, TOP_OFF_CHECK, then the INFRA bus protections
 * (infracfg 0x10001000: 0x2ac <- bit 10 [status 0x258], 0x2a4 <- bits 14, 18, 13 [status 0x228]),
 * and finally the chip id (MCU_TOP_MISC_OFF +0x10 must read 0x100a0000). The CONNSYS CPU stays in reset:
 * no firmware runs. Everything here is undone by the next reboot.
 */
static bool
a22c_poll(vm_offset_t base, uint32_t off, uint32_t bit, uint32_t want, uint32_t *last)
{
	for (int i = 0; i < 10; i++) {
		*last = a22c_rd(base, off);
		if (((*last & bit) != 0) == (want != 0)) {
			return true;
		}
		delay(500);
	}
	return false;
}

static void
a22c_wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static void
a22c_power_on(vm_offset_t spm)
{
	vm_offset_t rgu = ml_io_map(0x10007000u, 0x1000), infra = ml_io_map(0x10001000u, 0x1000);
	uint32_t v = 0, ack = 0, ack_s = 0, off = 0, s1 = 0, s2 = 0;
	bool a1, a2, a3, p1, p2;

	/* the D-die's pins to the A-die (MT6631) in their CONN function, before anything else, as consys_set_if_pinmux of
	 * mt6833.c: GPIO_MODE 0x10005000+0x480 (8 pins) all mode 1, +0x490 its first pin mode 1, and their driving
	 * (IOCFG 0x11ea0000 +0x0, +0x10) to 2 mA. Without it the A-die is never heard: run ios24, "A-die" answered ff */
	{
		vm_offset_t gpio = ml_io_map(0x10005000u, 0x1000), iocfg = ml_io_map(0x11ea0000u, 0x1000);
		uint32_t m1 = a22c_rd(gpio, 0x480), m2 = a22c_rd(gpio, 0x490), d1 = a22c_rd(iocfg, 0x0), d2 = a22c_rd(iocfg, 0x10);

		a22c_wr(gpio, 0x480, (m1 & 0x88888888u) | 0x11111111u);
		a22c_wr(gpio, 0x490, (m2 & 0xfffffff8u) | 0x1u);
		a22c_wr(iocfg, 0x0, (d1 & 0xc0007fffu) | 0x9048000u);
		a22c_wr(iocfg, 0x10, d2 & 0xfffff000u);
		printf("a22conn: step 4: A-die pins: GPIO_MODE 0x480 %#x -> %#x, 0x490 %#x -> %#x, driving %#x -> %#x, %#x -> %#x\n",
		    m1, a22c_rd(gpio, 0x480), m2, a22c_rd(gpio, 0x490), d1, a22c_rd(iocfg, 0x0), d2, a22c_rd(iocfg, 0x10));
		delay(150);
	}
	a22c_wr(rgu, 0x18, (a22c_rd(rgu, 0x18) | (1u << 12)) | (0x88u << 24));      /* CPU held in reset */
	a22c_wr(spm, 0x0, a22c_rd(spm, 0x0) | 1u | (0x0b16u << 16));                  /* SPM CR clock */
	v = a22c_rd(spm, A22C_TOP1_PWR_CTRL);
	printf("a22conn: step 4: start: RGU SW_RST %#x, SPM PWRON_CONFG_EN %#x, TOP1 %#x\n",
	    a22c_rd(rgu, 0x18), a22c_rd(spm, 0x0), v);
	a22c_wr(spm, A22C_TOP1_PWR_CTRL, a22c_rd(spm, A22C_TOP1_PWR_CTRL) | (1u << 2));
	a1 = a22c_poll(spm, A22C_PWR_ACK, A22C_ACK_ON, 1, &ack);
	a22c_wr(spm, A22C_TOP1_PWR_CTRL, a22c_rd(spm, A22C_TOP1_PWR_CTRL) | (1u << 3));
	a2 = a22c_poll(spm, A22C_PWR_ACK_S, A22C_ACK_ON, 1, &ack_s);
	a22c_wr(spm, A22C_TOP1_PWR_CTRL, a22c_rd(spm, A22C_TOP1_PWR_CTRL) & ~(1u << 4));
	delay(1);
	a22c_wr(spm, A22C_TOP1_PWR_CTRL, a22c_rd(spm, A22C_TOP1_PWR_CTRL) & ~(1u << 1));
	a22c_wr(rgu, 0x18, (a22c_rd(rgu, 0x18) & ~(1u << 9)) | (0x88u << 24));
	a22c_wr(spm, A22C_TOP1_PWR_CTRL, a22c_rd(spm, A22C_TOP1_PWR_CTRL) | 1u);
	a3 = a22c_poll(spm, A22C_TOP_OFF_CHECK, 1u << 1, 1, &off);
	delay(500);
	printf("a22conn: step 4: TOP1 %#x, ACK %#x (%s), ACK_S %#x (%s), TOP_OFF_CHECK %#x (%s)\n",
	    a22c_rd(spm, A22C_TOP1_PWR_CTRL), ack, a1 ? "on" : "NO ACK", ack_s, a2 ? "on" : "NO ACK",
	    off, a3 ? "on" : "NO ACK");
	if (!(a1 && a2 && a3)) {
		printf("a22conn: step 4: CONNSYS did not come up: bus protections left on, its registers not read\n");
		return;
	}
	a22c_wr(infra, 0x2ac, 1u << 10);
	p1 = a22c_poll(infra, 0x258, 1u << 10, 0, &s1);
	a22c_wr(infra, 0x2a4, 1u << 14);
	p2 = a22c_poll(infra, 0x228, 1u << 14, 0, &s2);
	a22c_wr(infra, 0x2a4, 1u << 18);
	p2 = a22c_poll(infra, 0x228, 1u << 18, 0, &s2) && p2;
	a22c_wr(infra, 0x2a4, 1u << 13);
	p2 = a22c_poll(infra, 0x228, 1u << 13, 0, &s2) && p2;
	delay(5000);
	printf("a22conn: step 4: bus protect: AHB status %#x (%s), AXI status %#x (%s)\n",
	    s1, p1 ? "off" : "STILL ON", s2, p2 ? "off" : "STILL ON");
	if (!(p1 && p2)) {
		printf("a22conn: step 4: bus protection did not clear: CONNSYS registers not read\n");
		return;
	}
	{
		vm_offset_t mcu = ml_io_map(A22C_MCU_PHYS, 0x1000), misc = ml_io_map(A22C_MISC_OFF_PHYS, 0x1000);
		uint32_t ip = 0;

		for (int i = 0; i < 10 && ip != A22C_IP_VER_ID; i++) {
			ip = a22c_rd(misc, 0x10);
			if (ip != A22C_IP_VER_ID) {
				delay(20000);
			}
		}
		printf("a22conn: step 4: IP_VER %#x (%s), CONF_ID %#x, HW %#x, FW/ROM %#x, MCU+0x600 %#x\n", ip,
		    ip == A22C_IP_VER_ID ? "the MT6833's: CONNSYS ON" : "NOT 0x100a0000", a22c_rd(misc, 0x1c),
		    a22c_rd(mcu, 0x0), a22c_rd(mcu, 0x4), a22c_rd(mcu, 0x600));
	}
}

/*
 * Step 5: the firmware into CONNSYS's memory and its CPU let go, as the Infinix's connsys_firmware_thread
 * (redwood_connsys.c, MT6768, which works) but with mt6833.c's values:
 *   - the memory: 8 MB at A22C_EMI_PHYS (boot-arg a22conn_emi, in MB; Android's consys-reserve-memory is
 *     placed anywhere in 0x40000000-0xc0000000, 16 MB aligned; 0x60000000 is clear of every reserved-memory
 *     node of this phone's device tree and of LK's load area), zeroed first as mtk_wmt_probe does;
 *   - INFRA+0x380 = its address >> 16 in bits 0-19 (CONSYS_REG_WRITE_OFFSET_RANGE(.., 0, 16, 20): 64 KB steps,
 *     not the MT6768's 16 MB), INFRA+0x388 the same for 0x10003000;
 *   - the firmware's log rings at EMI+0x58000 (connsys_fw_log_parameter), build times at EMI+0x88300/310/320;
 *   - each soc2_2_ram_{mcu,bt,wifi}_1(a)_1_hdr.bin: a 48-byte header (date 16, platform 4, hw version 2,
 *     sw version 2, address 4 at +0x18 as CONNSYS sees it, 0xf0000000 = start of EMI), body copied there.
 *     Of the "1" and "1a" files, the one whose hw version is CONNSYS's own (MCU+0x0) is taken; no patch_mcu.
 *   - then, in mtk_wcn_consys_hw_reg_ctrl's order: access-EMI hw mode (TOP_MISC_ON+0x168 bit 0), EMI entry
 *     addresses (MCU+0x504 0xf0170000, +0x508 0xf02a0000), XO control in RC mode (0x180c1200: +0x44/4c/54/5c
 *     0x02080706, ack bit 16 of +0x48/50/58/60 cleared, +0x40 OSC_RC_EN bits set and legacy bit 8 cleared),
 *     primary A-die (0x180c1130 bit 8), AHB timeout (0x18002440 = 0x90000002), CPU reset let go, and
 *     MCU+0x600 polled for 0x1d1e (consys_polling_goto_idle).
 * In a kernel thread: the files (/fw/connsys, put in the system image by userland/build.sh) are there only
 * once the root is mounted.
 */
#define A22C_EMI_SIZE           0x800000u
static uint32_t a22c_emi_phys = 0x60000000u;

/* a file of the RAM disk's fw/ (bsd/dev/redwood_image.c), as the other drivers' firmware: needs no root, and
 * what it returns is not freed (it lies in the RAM disk, or is kept for the session) */
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

static const uint8_t *
a22c_file(const char *name, uint32_t *size)
{
	uint64_t length = 0;
	const uint8_t *data = redwood_image_file("fw", name, &length, NULL);

	if (data == NULL || length < 48 || length > A22C_EMI_SIZE) {
		return NULL;
	}
	*size = (uint32_t)length;
	return data;
}

/* bytes into EMI through a 16 KB aligned window that is given back after (the kernel's I/O map is small) */
static bool
a22c_emi_write(uint32_t offset, const uint8_t *bytes, uint32_t length)
{
	uint32_t start = offset & ~0x3fffu, span = (offset + length + 0x3fffu) & ~0x3fffu;
	vm_offset_t window = ml_io_map_unmappable(a22c_emi_phys + start, span - start, 0x7 /* VM_WIMG_IO */);

	if (window == 0) {
		return false;
	}
	for (uint32_t at = 0; at < length; at += 4) {
		uint32_t word = 0;

		memcpy(&word, bytes + at, length - at >= 4 ? 4 : length - at);
		*(volatile uint32_t *)(window + (offset - start) + at) = word;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	ml_io_unmap(window, span - start);
	return true;
}

static bool
a22c_emi_clear(void)
{
	const uint32_t chunk = 512u * 1024u;

	for (uint32_t at = 0; at < A22C_EMI_SIZE; at += chunk) {
		vm_offset_t window = ml_io_map_unmappable(a22c_emi_phys + at, chunk, 0x7 /* VM_WIMG_IO */);

		if (window == 0) {
			printf("a22conn: step 5: EMI+%#x could not be mapped\n", at);
			return false;
		}
		for (uint32_t i = 0; i < chunk / 8; i++) {
			((volatile uint64_t *)window)[i] = 0;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		ml_io_unmap(window, chunk);
	}
	return true;
}

static void
a22c_set(vm_offset_t base, uint32_t off, uint32_t clear, uint32_t set)
{
	a22c_wr(base, off, (a22c_rd(base, off) & ~clear) | set);
}

/*
 * Step 6: BTIF (btif@1100c000, the same block and clock as the MT6768's: infracfg_ao IFRAO2 bit 31, CLR +0x84 --
 * clk-mt6833.c of a22x-mainline) by PIO, set up as hal_btif_hw_init does, and the first thing WMT asks CONNSYS
 * (wmt_core_hw_check), in STP's mandatory mode: a read of 0x80000008. Frame 80 40 14 00 + WMT_SET_REG_CMD (op 2,
 * 20 bytes) + CRC 00 00; CONNSYS woken first by BTIF_WAK (+0x64 low ~150 us, high). The Infinix's answered
 * 80 40 10 00 | 02 08 0c 00 00 00 00 01 08 00 00 80 <value> | 00 00. Up to 5 tries, each woken again.
 * Then CONNSYS asked for its own EMI+0 (0xf0000000): the answer should end with ram_mcu's first 4 bytes.
 */
static vm_offset_t a22c_btif;
static uint32_t a22c_step;
void redwood_a22wifi_net_early(void);
void redwood_a22wifi_start(uint32_t last);


static void
a22c_btif_init(void)
{
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000);

	a22c_wr(infra, 0x84, 1u << 31);                 /* ifrao_btif ungated */
	delay(10);
	a22c_btif = ml_io_map(0x1100c000u, 0x1000);
	a22c_wr(a22c_btif, 0x0c, 0);                    /* FAKELCR: normal mode */
	a22c_set(a22c_btif, 0x6c, 0, 1u);               /* HANDSHAKE: the new one */
	a22c_wr(a22c_btif, 0x60, 0x18);                 /* TRI_LVL */
	a22c_wr(a22c_btif, 0x08, 1u << 1);              /* clear RX */
	a22c_wr(a22c_btif, 0x08, 0);
	a22c_wr(a22c_btif, 0x08, 1u << 2);              /* clear TX */
	a22c_wr(a22c_btif, 0x08, 0);
	a22c_wr(a22c_btif, 0x4c, 1u << 2);              /* no DMA, auto-reset */
	a22c_wr(a22c_btif, 0x04, 0);                    /* no interrupts: polled */
	printf("a22conn: step 6: BTIF on (PIO): clock status %#x, LSR %#x\n", a22c_rd(infra, 0x90), a22c_rd(a22c_btif, 0x14));
}

static bool
a22c_btif_write(const uint8_t *bytes, unsigned length)
{
	unsigned at = 0, t;

	while (at < length) {
		for (t = 0; t < 50000 && !(a22c_rd(a22c_btif, 0x14) & (1u << 6)); t++) {
			delay(10);
		}
		if (t >= 50000) {
			return false;
		}
		for (unsigned n = 0; n < 16 && at < length; n++, at++) {
			*(volatile uint8_t *)(a22c_btif + 0x0) = bytes[at];
		}
	}
	return true;
}

static unsigned
a22c_btif_read(uint8_t *into, unsigned room, unsigned ms)
{
	unsigned got = 0;

	for (unsigned t = 0; t < ms * 10 && got < room; t++) {
		while ((a22c_rd(a22c_btif, 0x14) & 1) && got < room) {
			into[got++] = (uint8_t)a22c_rd(a22c_btif, 0x0);
		}
		if (got >= 4 && got >= 4u + (into[2] | ((into[1] & 0xfu) << 8)) + 2u) {
			break;                                  /* a whole mandatory-mode frame */
		}
		delay(100);
	}
	return got;
}

static bool
a22c_wmt(const char *what, const uint8_t *cmd, unsigned length, unsigned tries)
{
	uint8_t frame[64], answer[64];
	char line[220];

	frame[0] = 0x80;
	frame[1] = (uint8_t)(0x40 | ((length >> 8) & 0xf));
	frame[2] = (uint8_t)length;
	frame[3] = 0;
	memcpy(frame + 4, cmd, length);
	frame[4 + length] = 0;
	frame[5 + length] = 0;
	for (unsigned i = 0; i < tries; i++) {
		unsigned got, used;

		a22c_set(a22c_btif, 0x64, 1u, 0);               /* BTIF_WAK low ... */
		delay(150);
		a22c_set(a22c_btif, 0x64, 0, 1u);               /* ... and high */
		delay_for_interval(20, NSEC_PER_MSEC);
		if (!a22c_btif_write(frame, length + 6)) {
			printf("a22conn: step 6: %s: BTIF write timeout (LSR %#x)\n", what, a22c_rd(a22c_btif, 0x14));
			return false;
		}
		got = a22c_btif_read(answer, sizeof(answer), 1000);
		used = (unsigned)snprintf(line, sizeof(line), "a22conn: step 6: %s, try %u: ", what, i + 1);
		if (got == 0) {
			snprintf(line + used, sizeof(line) - used, "no answer (LSR %#x)", a22c_rd(a22c_btif, 0x14));
		}
		for (unsigned b = 0; b < got && used < sizeof(line) - 4; b++) {
			used += (unsigned)snprintf(line + used, sizeof(line) - used, "%02x%s", answer[b],
			    b == 3 || b + 3 == got ? " | " : " ");
		}
		printf("%s\n", line);
		if (got >= 8 && answer[0] == 0x80 && answer[4] == 0x02 && answer[5] == cmd[1]) {
			return true;
		}
	}
	return false;
}

/*
 * Step 7: STP into its full mode and WMT's init script, as mtk_wcn_soc_sw_init (core/wmt_ic_soc.c) runs it for
 * this chip: mt6833.c has pda_dl_patch_flag 1 and OPT_DISABLE_ROM_PATCH_DWN in its options, so no patch and no
 * WMT reset after one -- straight to _wmt_soc_mode_ctrl (STP defaults asked, set, the host switched, asked again
 * in full mode), then the script the Infinix's driver runs (redwood_connsys.c connsys_sw_init, which works on the
 * MT6768) with this chip's ID 0x6833: A-die query, LTE coex (OPT_WIFI_LTE_COEX_TABLE_3), BT TSSI, RF calibration,
 * coex adjust (new flag: 0x1e-0x20), FM strap, UTC time. Full-mode frame: 0x80 | seq << 3 | ack, task << 4 | len
 * high bits, len low byte, the sum of the three; payload; CRC-16 (0xA001 reflected) low byte first. Every frame
 * CONNSYS sends with a payload is acked with a header-only frame; ack = the last seq taken (7 before any).
 */
static bool a22c_full;
static uint8_t a22c_seq, a22c_ack = 7, a22c_expect;

static uint16_t
a22c_crc16(const uint8_t *b, unsigned n)
{
	uint16_t crc = 0;

	for (unsigned i = 0; i < n; i++) {
		crc ^= b[i];
		for (int k = 0; k < 8; k++) {
			crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static int
a22c_byte(unsigned ms)
{
	for (unsigned t = 0; t <= ms * 10; t++) {
		if (a22c_rd(a22c_btif, 0x14) & 1) {
			return (int)(a22c_rd(a22c_btif, 0x0) & 0xff);
		}
		delay(100);
	}
	return -1;
}

static bool
a22c_stp_send(const uint8_t *payload, unsigned length)
{
	uint8_t frame[4 + 64 + 2];
	uint16_t crc = a22c_full ? a22c_crc16(payload, length) : 0;

	if (length > 64) {
		return false;
	}
	frame[0] = (uint8_t)(0x80 | (a22c_full ? ((a22c_seq << 3) | a22c_ack) : 0));
	frame[1] = (uint8_t)((4u << 4) | ((length >> 8) & 0xf));
	frame[2] = (uint8_t)length;
	frame[3] = a22c_full ? (uint8_t)(frame[0] + frame[1] + frame[2]) : 0;
	memcpy(frame + 4, payload, length);
	frame[4 + length] = (uint8_t)crc;
	frame[5 + length] = (uint8_t)(crc >> 8);
	if (a22c_full) {
		a22c_seq = (a22c_seq + 1) & 7;
	}
	return a22c_btif_write(frame, length + 6);
}

/* the next WMT frame with a payload; acks, other channels and repeats said in "heard" and skipped */
static int
a22c_stp_receive(uint8_t *payload, unsigned room, char *heard, size_t heard_room, unsigned first_ms)
{
	size_t used = strlen(heard);

	for (;;) {
		int b0 = a22c_byte(first_ms), b1, b2, b3;
		unsigned length;

		if (b0 < 0) {
			return -1;
		}
		if (b0 == 0x55 || !(b0 & 0x80) || (b0 & 0x40)) {
			used += (size_t)snprintf(heard + used, heard_room - used, " ?%02x", b0);
			continue;
		}
		if ((b1 = a22c_byte(50)) < 0 || (b2 = a22c_byte(50)) < 0 || (b3 = a22c_byte(50)) < 0) {
			return -1;
		}
		length = ((unsigned)(b1 & 0xf) << 8) | (unsigned)b2;
		if (length == 0) {
			used += (size_t)snprintf(heard + used, heard_room - used, " [ack %u]", b0 & 7);
			continue;
		}
		for (unsigned i = 0; i < length + 2; i++) {
			int b = a22c_byte(50);

			if (b < 0) {
				return -1;
			}
			if (i < length && i < room) {
				payload[i] = (uint8_t)b;
			}
		}
		if (a22c_full) {
			uint8_t seq = (uint8_t)((b0 >> 3) & 7), ack[4];
			bool again = seq != a22c_expect;

			if (!again) {
				a22c_ack = seq;
				a22c_expect = (uint8_t)((seq + 1) & 7);
			}
			ack[0] = (uint8_t)(0x80 | a22c_ack);
			ack[1] = 0;
			ack[2] = 0;
			ack[3] = ack[0];
			(void)a22c_btif_write(ack, 4);
			if (again) {
				/* said in full (header, length, first payload bytes); a WMT event of this kind is taken as it is
				 * and its sequence adopted -- run ios25: the RF calibration's answer came as "seq 0" after seq 4 */
				used += (size_t)snprintf(heard + used, heard_room - used, " [seq %u again: %02x %02x %02x %02x, %u bytes: %02x %02x %02x %02x %02x %02x]",
				    seq, b0, b1, b2, b3, length, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5]);
				if (((b1 >> 4) & 7) != 4) {
					/* another channel: its text said whole (run ios27: channel 5, "<ASSERT..." during the RF calibration) */
					char text[260];
					unsigned n = 0;

					for (unsigned i = 0; i < length && i < room && n < sizeof(text) - 1; i++) {
						text[n++] = (payload[i] >= 0x20 && payload[i] < 0x7f) ? (char)payload[i] : '.';
					}
					text[n] = 0;
					printf("a22conn: step 7: CONNSYS channel %u says: %s\n", (b1 >> 4) & 7, text);
				}
				if (((b1 >> 4) & 7) == 4 && payload[0] == 0x02 && length >= 4 &&
				    (unsigned)(payload[2] | (payload[3] << 8)) + 4 == length) {
					a22c_ack = seq;
					a22c_expect = (uint8_t)((seq + 1) & 7);
					ack[0] = (uint8_t)(0x80 | a22c_ack);
					ack[3] = ack[0];
					(void)a22c_btif_write(ack, 4);
					used += (size_t)snprintf(heard + used, heard_room - used, " [taken: a whole WMT event]");
					return (int)(length < room ? length : room);
				}
				continue;
			}
		}
		if (((b1 >> 4) & 7) != 4) {
			char text[260];
			unsigned n = 0;

			for (unsigned i = 0; i < length && i < room && n < sizeof(text) - 1; i++) {
				text[n++] = (payload[i] >= 0x20 && payload[i] < 0x7f) ? (char)payload[i] : '.';
			}
			text[n] = 0;
			printf("a22conn: step 7: CONNSYS channel %u says: %s\n", (b1 >> 4) & 7, text);
			used += (size_t)snprintf(heard + used, heard_room - used, " [type %u, %u bytes]", (b1 >> 4) & 7, length);
			continue;
		}
		return (int)(length < room ? length : room);
	}
}

static bool
a22c_cmd(const char *what, const uint8_t *cmd, unsigned length, const uint8_t *expected, unsigned expected_length,
    unsigned wait_ms)
{
	char line[600];
	uint8_t answer[600];
	int got;
	size_t used;
	bool ok;

	used = (size_t)snprintf(line, sizeof(line), "a22conn: step 7: %s:", what);
	/* in the mandatory mode (no sequence numbers) a lost command is sent again, up to 3 times, each after a new
	 * wakeup -- CONNSYS does not always take the first one (ios26; the Infinix's runs 317-318 too) */
	for (unsigned t = 0; t < (a22c_full ? 1u : 3u); t++) {
		if (t > 0) {
			used = strlen(line);
			snprintf(line + used, sizeof(line) - used, " [no answer, again]");
		}
		a22c_set(a22c_btif, 0x64, 1u, 0);       /* BTIF_WAK pulse */
		delay(150);
		a22c_set(a22c_btif, 0x64, 0, 1u);
		delay_for_interval(20, NSEC_PER_MSEC);
		if (!a22c_stp_send(cmd, length)) {
			printf("%s BTIF write timeout\n", line);
			return false;
		}
		got = a22c_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms ? wait_ms : 1000);
		if (got >= 0) {
			break;
		}
	}
	/* power-saving events (02 03 02 00 00 xx) are not answers, nor another opcode's event */
	for (unsigned more = 0; more < 6 && got >= 5 && answer[0] == 0x02 && answer[1] != expected[1]; more++) {
		used = strlen(line);
		snprintf(line + used, sizeof(line) - used, " [event %02x]", answer[1]);
		got = a22c_stp_receive(answer, sizeof(answer), line, sizeof(line), wait_ms ? wait_ms : 1000);
	}
	used = strlen(line);
	if (got < 0) {
		snprintf(line + used, sizeof(line) - used, " no answer (LSR %#x)", a22c_rd(a22c_btif, 0x14));
	}
	for (int i = 0; i < got && i < 24 && used < sizeof(line) - 24; i++) {
		used += (size_t)snprintf(line + used, sizeof(line) - used, " %02x", answer[i]);
	}
	ok = got >= 0 && (unsigned)got >= expected_length && memcmp(answer, expected, expected_length) == 0;
	if (got >= 0) {
		snprintf(line + used, sizeof(line) - used, ok ? " (as expected)" : " (NOT as expected)");
	}
	printf("%s\n", line);
	return ok;
}

static void
a22c_wmt_init(void)
{
	static const uint8_t query[] = { 0x01, 0x04, 0x01, 0x00, 0x04 };
	static const uint8_t query_said[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04 };
	static const uint8_t set[] = { 0x01, 0x04, 0x05, 0x00, 0x03, 0xDB, 0x0E, 0x68, 0x01 };
	static const uint8_t set_done[] = { 0x02, 0x04, 0x02, 0x00, 0x00, 0x03 };
	static const uint8_t query_full[] = { 0x02, 0x04, 0x06, 0x00, 0x00, 0x04, 0xDB, 0x0E, 0x68, 0x01 };
	static const uint8_t chip_id[] = { 0x01, 0x02, 0x05, 0x00, 0x11, 0x33, 0x68, 0x00, 0x00 };
	static const uint8_t a_die[] = { 0x01, 0x02, 0x01, 0x00, 0x12 };
	static const uint8_t a_die_said[] = { 0x02, 0x02, 0x05, 0x00, 0x00 };
	static const uint8_t lte_coex[] = { 0x01, 0x10, 0x02, 0x00, 0x15, 0x01 };
	/* WMT_SOC.cfg of this phone: bt_tssi_from_wifi=3, bt_tssi_target=3719 (0x0e87); wifi_ant_swap_mode=1,
	 * wifi_main_ant_polarity=0, wifi_ant_swap_ant_sel_gpio=3 -> WMT_WIFI_ANT_SWAP_CMD before the calibration */
	static const uint8_t tssi[] = { 0x01, 0x02, 0x04, 0x00, 0x10, 0x03, 0x87, 0x0e };
	static const uint8_t ant_swap[] = { 0x01, 0x14, 0x04, 0x00, 0x07, 0x02, 0x00, 0x03 };
	static const uint8_t ant_swap_done[] = { 0x02, 0x14, 0x02, 0x00, 0x00, 0x07 };
	static const uint8_t calibration[] = { 0x01, 0x14, 0x01, 0x00, 0x01 };
	static const uint8_t calibrated[] = { 0x02, 0x14 };
	static const uint8_t adjust_bt[] = { 0x01, 0x10, 0x04, 0x00, 0x1e, 0x00, 0x00, 0x00 };
	static const uint8_t adjust_opp[] = { 0x01, 0x10, 0x04, 0x00, 0x1f, 0x00, 0x00, 0x00 };
	static const uint8_t adjust_ble[] = { 0x01, 0x10, 0x04, 0x00, 0x20, 0x00, 0x00, 0x00 };
	static const uint8_t fm_strap[] = { 0x01, 0x05, 0x02, 0x00, 0x02, 0x02 };
	static const uint8_t fm_strap_said[] = { 0x02, 0x05, 0x02, 0x00, 0x00, 0x02 };
	static const uint8_t wmt_done[] = { 0x02, 0x02, 0x01, 0x00, 0x00 };
	static const uint8_t coex_done[] = { 0x02, 0x10, 0x01, 0x00, 0x00 };
	static const uint8_t utc_said[] = { 0x02, 0xf0, 0x02, 0x00, 0x02, 0x00 };
	static const struct {
		const char *what;
		const uint8_t *c, *e;
		uint8_t cl, el;
		bool must;
		unsigned wait;
	} script[] = {
#define S(w, c, e, m, t) { w, c, e, sizeof(c), sizeof(e), m, t }
		S("set chip ID 0x6833", chip_id, wmt_done, false, 0),
		S("A-die", a_die, a_die_said, true, 0),
		S("LTE coex (table 3)", lte_coex, coex_done, false, 0),
		S("BT TSSI from Wi-Fi", tssi, wmt_done, false, 0),
		S("Wi-Fi antenna swap", ant_swap, ant_swap_done, false, 0),
		S("RF calibration", calibration, calibrated, true, 3000),
		S("coex: BT control", adjust_bt, coex_done, false, 0),
		S("coex: opp time ratio", adjust_opp, coex_done, false, 0),
		S("coex: BLE scan time ratio", adjust_ble, coex_done, false, 0),
		S("FM strap (COMM)", fm_strap, fm_strap_said, true, 0),
#undef S
	};
	uint8_t utc[] = { 0x01, 0xf0, 0x09, 0x00, 0x02, 0, 0, 0, 0, 0, 0, 0, 0 };
	clock_sec_t secs;
	clock_usec_t usecs;
	unsigned answered = 0;

	if (!a22c_cmd("STP defaults (mandatory mode)", query, sizeof(query), query_said, sizeof(query_said), 3000) ||
	    !a22c_cmd("STP set full mode", set, sizeof(set), set_done, sizeof(set_done), 3000)) {
		printf("a22conn: step 7: stopped before full mode\n");
		return;
	}
	a22c_full = true;
	a22c_seq = 0;
	a22c_ack = 7;
	a22c_expect = 0;
	delay(10000);
	if (!a22c_cmd("full mode, STP asked again", query, sizeof(query), query_full, sizeof(query_full), 0)) {
		printf("a22conn: step 7: full mode not confirmed\n");
		return;
	}
	printf("a22conn: step 7: STP IN FULL MODE\n");
	for (unsigned i = 0; i < sizeof(script) / sizeof(script[0]); i++) {
		bool ok;

		if (script[i].c == lte_coex) {
			delay(5000);
		}
		if (script[i].c == tssi) {
			uint32_t want = 1;

			/* boot-arg a22conn_tssi=0: not sent (Android sends it only when WMT_SOC.cfg has bt_tssi_from_wifi; the
			 * values here are the Infinix's) */
			if (PE_parse_boot_argn("a22conn_tssi", &want, sizeof(want)) && want == 0) {
				printf("a22conn: step 7: BT TSSI from Wi-Fi: not sent (a22conn_tssi=0)\n");
				continue;
			}
		}
		ok = a22c_cmd(script[i].what, script[i].c, script[i].cl, script[i].e, script[i].el, script[i].wait);
		answered += ok;
		if (!ok && script[i].must) {
			printf("a22conn: step 7: stopped at \"%s\", as Android would\n", script[i].what);
			return;
		}
	}
	clock_get_calendar_microtime(&secs, &usecs);
	for (unsigned i = 0; i < 4; i++) {
		utc[5 + i] = (uint8_t)((uint32_t)secs >> (8 * i));
		utc[9 + i] = (uint8_t)((uint32_t)usecs >> (8 * i));
	}
	answered += a22c_cmd("UTC time", utc, sizeof(utc), utc_said, sizeof(utc_said), 0);
	printf("a22conn: step 7: %u of %u answered: WMT IS UP (Wi-Fi itself is gen4m's: next step)\n", answered,
	    (unsigned)(sizeof(script) / sizeof(script[0])) + 1);
	if (a22c_step >= 8) {
		redwood_a22wifi_start(a22c_step + 4);
	}
}

static void
a22c_wmt_first(void)
{
	static const uint8_t chip_id[] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01, 0x08, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};
	static const uint8_t emi_read[] = {
		0x01, 0x08, 0x10, 0x00, 0x02, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0xf0, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
	};

	a22c_btif_init();
	if (a22c_wmt("WMT read 0x80000008 (chip id)", chip_id, sizeof(chip_id), 5)) {
		delay_for_interval(10, NSEC_PER_MSEC);
		(void)a22c_wmt("WMT read 0xf0000000 (its EMI+0: ram_mcu's first word)", emi_read, sizeof(emi_read), 2);
		printf("a22conn: step 6: THE COMMAND CHANNEL WORKS\n");
		if (a22c_step >= 7) {
			delay_for_interval(10, NSEC_PER_MSEC);
			a22c_wmt_init();
		}
	} else {
		printf("a22conn: step 6: CONNSYS did not answer\n");
	}
}


static void
a22c_firmware_thread(__unused void *parameter, __unused wait_result_t result)
{
	static const char *const kinds[] = { "mcu", "bt", "wifi" };
	static const uint32_t times[] = { 0x88320u, 0x88300u, 0x88310u };
	vm_offset_t infra = ml_io_map(0x10001000u, 0x1000), rgu = ml_io_map(0x10007000u, 0x1000);
	vm_offset_t mcu = ml_io_map(A22C_MCU_PHYS, 0x1000), hif = ml_io_map(0x18007000u, 0x1000);
	vm_offset_t misc_on = ml_io_map(0x180c1000u, 0x1000);
	uint32_t hw = a22c_rd(mcu, 0x0) & 0xffff, loaded = 0, idle = 0;

	for (unsigned s = 0; s < 240 && rootvnode == NULLVP; s++) {
		delay_for_interval(1, NSEC_PER_SEC);
	}
	delay_for_interval(3, NSEC_PER_SEC);
	if (!a22c_emi_clear()) {
		goto out;
	}
	{
		static const uint32_t rings[10] = {
			0x40, 16 * 1024, 0x40 + 16 * 1024 + 64, 64 * 1024,
			0x40 + 16 * 1024 + 64 + 64 * 1024 + 64, 64 * 1024,
			0x40 + 16 * 1024 + 64 + 2 * (64 * 1024 + 64), 32 * 1024,
			0x46494d45u, 0x474f4c57u,                               /* "EMIFWLOG" at +56 */
		};
		uint32_t header[16] = { 0 };

		memcpy(header, rings, 32);
		header[14] = rings[8];
		header[15] = rings[9];
		(void)a22c_emi_write(0x58000u, (const uint8_t *)header, sizeof(header));
	}
	printf("a22conn: step 5: EMI %#x (8 MB) cleared, log rings at EMI+0x58000; CONNSYS hw version %#x\n",
	    a22c_emi_phys, hw);
	for (unsigned k = 0; k < 3; k++) {
		/* the "_1_" files (2024) by default: with the "_1a_" ones (2021) Bluetooth's firmware ASSERTs in the RF
		 * calibration (ios27-29: swid 0x6721, lr 0x45590), with these all 11 of WMT's init answer (ios30).
		 * Boot-arg a22conn_fw=2: the "_1a_" ones first */
		static const char *const flavours_a[] = { "1a", "1" }, *const flavours_1[] = { "1", "1a" };
		uint32_t fw1 = 1;
		const char *const *flavours = (PE_parse_boot_argn("a22conn_fw", &fw1, sizeof(fw1)) && fw1 == 2) ? flavours_a : flavours_1;
		const uint8_t *file = NULL;
		uint32_t size = 0, address = 0, offset;
		uint16_t file_hw = 0;
		char name[64];

		for (unsigned f = 0; f < 2 && file == NULL; f++) {
			snprintf(name, sizeof(name), "soc2_2_ram_%s_%s_1_hdr.bin", kinds[k], flavours[f]);
			file = a22c_file(name, &size);
			if (file == NULL) {
				printf("a22conn: step 5: fw/%s: not in the RAM disk\n", name);
				continue;
			}
			memcpy(&file_hw, file + 20, 2);
			memcpy(&address, file + 0x18, 4);
			printf("a22conn: step 5: %s: %u bytes, %.14s, hw field %#x, address field %#x\n", name, size,
			    (const char *)file, file_hw, address);
		}
		if (file == NULL) {
			printf("a22conn: step 5: no %s firmware in the RAM disk\n", kinds[k]);
			goto out;
		}
		/* as the Infinix's driver: the offset in EMI is bits 8-23 of the address field (its low byte is not part of it) */
		offset = address & 0x00ffff00u;
		if (offset + size - 48 > A22C_EMI_SIZE || !a22c_emi_write(offset, file + 48, size - 48)) {
			printf("a22conn: step 5: %s: %u bytes at EMI+%#x do not fit\n", kinds[k], size - 48, offset);
			goto out;
		}
		{
			uint8_t text[16] = { 0 };

			memcpy(text, file, 15);
			(void)a22c_emi_write(times[k], text, 16);
		}
		printf("a22conn: step 5: ram_%s in: %u bytes at EMI+%#x\n", kinds[k], size - 48, offset);
		loaded++;
	}
	a22c_set(infra, 0x380, 0xfffffu, (a22c_emi_phys >> 16) & 0xfffffu);
	a22c_set(infra, 0x388, 0xfffffu, (0x10003000u >> 16) & 0xfffffu);
	a22c_set(misc_on, 0x168, 0, 1u);                                        /* access EMI hw mode */
	a22c_wr(mcu, 0x504, 0xf0170000u);                                       /* BT entry */
	a22c_wr(mcu, 0x508, 0xf02a0000u);                                       /* Wi-Fi entry */
	for (uint32_t o = 0x244; o <= 0x25c; o += 8) {                          /* XO: GPS, BT, WF, TOP */
		a22c_wr(misc_on, o, 0x02080706u);
		a22c_set(misc_on, o + 4, 1u << 16, 0);
	}
	a22c_set(misc_on, 0x240, 1u << 8, 0xf0f0u);
	a22c_set(misc_on, 0x130, 0, 1u << 8);                                   /* primary A-die (MT6631) */
	a22c_wr(mcu, 0x440, 0x90000002u);                                       /* AHB timeout */
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("a22conn: step 5: %u files in; remap %#x/%#x, EMI hw mode %#x, entries %#x/%#x, XO RC %#x, A-die %#x\n",
	    loaded, a22c_rd(infra, 0x380), a22c_rd(infra, 0x388), a22c_rd(misc_on, 0x168), a22c_rd(mcu, 0x504),
	    a22c_rd(mcu, 0x508), a22c_rd(misc_on, 0x240), a22c_rd(misc_on, 0x130));
	a22c_wr(rgu, 0x18, (a22c_rd(rgu, 0x18) & ~(1u << 12)) | (0x88u << 24));       /* the CPU let go */
	for (unsigned n = 0; n < 50 && idle != 0x1d1e; n++) {
		delay_for_interval(20, NSEC_PER_MSEC);
		idle = a22c_rd(mcu, 0x600);
	}
	printf("a22conn: step 5: MCU+0x600 %#x (%s), CPUPCR %#x, %#x, MCU+0x16c %#x\n", idle,
	    idle == 0x1d1e ? "CONNSYS IDLE: ITS ROM TOOK THE FIRMWARE" : "not 0x1d1e", a22c_rd(hif, 0x104),
	    (delay_for_interval(10, NSEC_PER_MSEC), a22c_rd(hif, 0x104)), a22c_rd(mcu, 0x16c));
	if (a22c_step >= 6 && idle == 0x1d1e) {
		delay_for_interval(20, NSEC_PER_MSEC);          /* Android's msleep(20) after the reset bit */
		a22c_wmt_first();
	}
out:
	thread_terminate(current_thread());
}

void
redwood_a22connsys_init(void)
{
	uint32_t step = 0, ctrl, ack, ack_s, off;
	vm_offset_t spm;

	if (!PE_parse_boot_argn("a22conn_step", &step, sizeof(step)) || step == 0) {
		return;
	}
	if (step >= 12) {
		redwood_a22wifi_net_early();
	}
	spm = ml_io_map(A22C_SPM_PHYS, 0x1000);
	ctrl = a22c_rd(spm, A22C_TOP1_PWR_CTRL);
	ack = a22c_rd(spm, A22C_PWR_ACK);
	ack_s = a22c_rd(spm, A22C_PWR_ACK_S);
	off = a22c_rd(spm, A22C_TOP_OFF_CHECK);
	printf("a22conn: step 1: TOP1_PWR_CTRL %#x (reset released %u, iso %u, on %u, on2 %u, clk %u, sram_pd %u), "
	    "ACK %#x / %#x (CONN %u/%u, TOP2 %u/%u), TOP_OFF_CHECK %#x\n", ctrl, ctrl & 1, (ctrl >> 1) & 1,
	    (ctrl >> 2) & 1, (ctrl >> 3) & 1, (ctrl >> 4) & 1, (ctrl >> 8) & 1, ack, ack_s,
	    !!(ack & A22C_ACK_ON), !!(ack_s & A22C_ACK_ON), !!(ack & A22C_ACK_TOP2), !!(ack_s & A22C_ACK_TOP2), off);
	if ((ack & A22C_ACK_ON) && (ack_s & A22C_ACK_ON) && (ack & A22C_ACK_TOP2) && (ack_s & A22C_ACK_TOP2)) {
		vm_offset_t mcu = ml_io_map(A22C_MCU_PHYS, 0x1000), misc = ml_io_map(A22C_MISC_OFF_PHYS, 0x1000);
		uint32_t ip = a22c_rd(misc, 0x10);

		printf("a22conn: step 1: CONNSYS is on: IP_VER %#x (%s), CONF_ID %#x, HW %#x, ROM %#x\n", ip,
		    ip == A22C_IP_VER_ID ? "the MT6833's" : "NOT the expected 0x100a0000", a22c_rd(misc, 0x1c),
		    a22c_rd(mcu, 0x0), a22c_rd(mcu, 0x4));
	} else {
		printf("a22conn: step 1: CONNSYS is off (as LK leaves it): its own registers not read\n");
	}
	if (step < 2) {
		return;
	}
	{
		static const struct { const char *name; uint32_t con0, ana; } ldo[] = {
			{ "VCN13", 0x1c1c, 0x202e },
			{ "VCN18", 0x1c2e, 0x2010 },
			{ "VCN33_1 (BT/Wi-Fi)", 0x1be2, 0x1f98 },
			{ "VCN33_2 (Wi-Fi)", 0x1c08, 0x1f9c },
		};

		a22c_pmif = ml_io_map(A22C_PMIF_PHYS, 0x1000);
		for (unsigned i = 0; i < sizeof(ldo) / sizeof(ldo[0]); i++) {
			uint32_t con0 = 0xdead, ana = 0xdead;
			bool ok = a22c_pmic_read(ldo[i].con0, &con0) && a22c_pmic_read(ldo[i].ana, &ana);

			printf("a22conn: step 2: %s: %s, CON0 %#x = %#x, ANA_CON0 %#x = %#x\n", ldo[i].name,
			    ok ? ((con0 & 1) ? "ON" : "off") : "NOT READ", ldo[i].con0, con0, ldo[i].ana, ana);
		}
	}
	if (step >= 3) {
		a22c_supplies_on();
	}
	if (step >= 4) {
		delay(150);
		a22c_power_on(spm);
	}
	if (step >= 5 && (a22c_rd(spm, A22C_PWR_ACK) & A22C_ACK_ON)) {
		uint32_t mb = 0;
		thread_t thread;

		if (PE_parse_boot_argn("a22conn_emi", &mb, sizeof(mb)) && mb >= 0x400 && mb < 0xc00 && (mb & 0xf) == 0) {
			a22c_emi_phys = mb << 20;
		}
		a22c_step = step;
		if (kernel_thread_start_priority(a22c_firmware_thread, NULL, 95, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}
