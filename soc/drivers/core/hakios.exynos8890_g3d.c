/*
 * The Exynos 8890's side of the Galaxy Note FE's Mali-T880 MP12 (hakios.mali_glue.h, mali_soc=exynos8890): the G3D
 * block's power domain, clock and supplies for the Mali driver (drivers/gpu/hakios.mali.c). S-Boot leaves the block on
 * and clocked (PLAN.md, Phase 6: run 20261108T155235): step 2 goes on only if the block is as S-Boot leaves it, at a
 * rate and voltage of Samsung's table. With mali_mhz=546 or 650 (approved by Ege, 8 November 2026) step 2 then raises
 * the supplies once to 650 MHz's (this chip's: below) and moves G3D_PLL to the rate, and the rates 260, 546 and 650 are
 * offered to mali_gov, which switches only the clock. Powering the domain on is not done here (not approved).
 *
 *   power     the PMU's pd-g3d (the tree's pd-g3d@105c4060): configuration 0x105C4060 [3:0] = 0xF asks for it on,
 *             status 0x105C4064 [3:0] = 0xF when it is. The domain's power pin also switches BUCK6 (vdd_g3d) on
 *             (hakios.s2mps16.c). GPU_DVS_CTRL (0x105C6100) bit 1 asks for DVS, which moves BUCK6 to B6CTRL2's
 *             voltage; GPU_DVS_STATUS bit 0 reads 0 while DVS is on, and Samsung's kernel counts the GPU off then
 *             (s2m_get_dvs_is_on, gpu_is_power_on)
 *   clock     G3D_PLL, a PLL141xx in CMU_TOP (always on): CON0 ENABLE bit 31, LOCKED 29, M [25:16], P [13:8], S [2:0];
 *             26 MHz x M / (P << S). It reaches the GPU through CMU_G3D's MUX_G3D_PLL_USER and MUX_G3D (bit 12 the
 *             select). CMU_G3D is inside the domain (Samsung's g3d_post sets its muxes again after every power-on), so
 *             it is read only with the domain on
 *   rate      Samsung's order (gpu_dvfs_api.c GPU_SET_CLK_VOL up: BUCK11, BUCK6, EMA, clock; S5E8890-dfs.c's
 *             dfsg3d table, pwrcal-dfs.c transition()): the G3D moved to the switch clock (CMU_TOP's SCLK_BUS_PLL_G3D:
 *             SCLK_BUS0_PLL, 528 MHz, through CMU_G3D's user mux), G3D_PLL relocked, the dividers to the new level's
 *             (the ECT's: only SCLK_HPM_G3D moves, 1 under 546 MHz, 2 from it), back; CMU_TOP's switch registers then
 *             as they were. The EMA stays: Samsung changes it only above 750 mV. Done only on this chip's ASV (G3D
 *             group 10, table 10, no SSA or g3dm floor), from S-Boot's voltages (B6CTRL3 0x30, B11CTRL2 0x48)
 *   voltage   Samsung's table (the ECT in the stock sboot.bin, at 0x38c00: ASV block, dvfs_g3d) gives 260 MHz 600 to
 *             643 mV, by the chip's ASV group and the table's version. Both are fused at 0x101E9000 (struct
 *             asv_tbl_info of S5E8890-asv.c), a secure block: read by Samsung's SMC_CMD_REG (-101, x1 = 0xC0000000 |
 *             address >> 2; 0 back in x0, the word in x2), as its kernel does at every boot (exynos_smc_readsfr).
 *             The g3d fields: word 1 [3:0] group, [7:4] its signed correction; word 3 [6:0] the table's version,
 *             [7] fused (3 if not). This chip, table 10 group 10: 260 600 mV, 546 668.75, 650 706.25; BUCK11
 *             (dvs_g3dm) 750 mV to 455 MHz, 800 from 546
 *   GPU       mali@14AC0000 (0x5000 bytes); IRQs SPI 225 JOB, 226 MMU, 224 GPU. A read of it, or of CMU_G3D, while the
 *             domain is off or unclocked hangs the bus instead of faulting
 *
 * Sources: the phone's device tree; Samsung's kernel for the 8890 (LineageOS android_kernel_samsung_universal8890:
 * pwrcal/S5E8890 S5E8890-cmusfr.h, -pmusfr.h, -pmu.c, -cmu.c, -pll.c, -asv.c; arch/arm64/kernel/exynos-smc.S;
 * soc/samsung/ect_parser.c; gpu/arm/t8xx/r28p0/platform/exynos/gpu_exynos8890.c). GPL-2.0: read for facts, no code
 * taken.
 */
#include <stdarg.h>
#include <stdint.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "hakios.mali_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern bool hakios_s2mps16_g3d_supply(int (*say)(const char *, ...), uint8_t *b6ctrl3);
extern bool hakios_s2mps16_buck_volt(int (*say)(const char *, ...), unsigned buck, uint8_t from, uint8_t to);

#define PMU_PA                  0x105C0000UL
#define PMU_SIZE                0x7000
#define  G3D_CONFIGURATION      0x4060
#define  G3D_STATUS             0x4064
#define  G3D_OPTION             0x4068
#define  G3D_DURATION0          0x4070
#define  MEMORY_G3D_CONFIG      0x4A60
#define  MEMORY_G3D_STATUS      0x4A64
#define  GPU_DVS_CTRL           0x6100
#define  GPU_DVS_STATUS         0x6104
#define  DVS_OFF                1u              /* GPU_DVS_STATUS bit 0 */
#define  G3D_ON                 0xFu

#define CMU_TOP_PA              0x10570000UL
#define  G3D_PLL_LOCK           0x00E0
#define  G3D_PLL_CON0           0x01E0
#define  G3D_PLL_CON1           0x01E4
#define  MUX_G3D_PLL            0x021C
#define  MUX_ACLK_CCORE_G3D_800 0x0248
#define  MUX_SCLK_BUS_PLL_G3D   0x038C
#define  DIV_ACLK_CCORE_G3D_800 0x03A8
#define  DIV_SCLK_BUS_PLL_G3D   0x04FC
#define  PLL_ENABLE             (1u << 31)
#define  PLL_LOCKED             (1u << 29)
#define  PLL_PMS                ((0x3FFu << 16) | (0x3Fu << 8) | 7u)
#define  PLL_260MHZ             ((160u << 16) | (4u << 8) | 2u)     /* M 160, P 4, S 2: S-Boot's, the table's lowest */
#define  BUS0_PLL_CON0          0x0100
#define  MUX_SCLK_BUS0_PLL      0x0220          /* 1: BUS0_PLL / 2 */
#define  MUX_SCLK_BUS_PLL_G3D_STAT 0x068C
#define  ENABLE_SCLK_BUS_PLL_G3D 0x099C

#define CMU_CCORE_PA            0x105B0000UL
#define CMU_CCORE_SIZE          0x3000
#define  MUX_ACLK_CCORE_G3D_800_USER 0x0208
#define  QCH_LH_G3DIRAM         0x2008
#define  QCH_LH_G3D0            0x2014
#define  QCH_LH_G3D1            0x2018
#define  QCH_LH_G3DP            0x202C

#define CMU_G3D_PA              0x14AA0000UL
#define  MUX_G3D_PLL_USER       0x0200
#define  MUX_BUS_PLL_USER_G3D   0x0204
#define  MUX_G3D                0x020C
#define  MUX_G3D_STAT           0x060C
#define  DIV_ACLK_G3D           0x0400
#define  DIV_PCLK_G3D           0x0404
#define  DIV_SCLK_HPM_G3D       0x0408
#define  ENABLE_ACLK_G3D        0x0800
#define  ENABLE_ACLK_G3D_BUS    0x0804
#define  ENABLE_PCLK_G3D        0x0900
#define  MUX_SELECT             (1u << 12)

#define ASV_INFO_PA             0x101E9000UL
#define ASV_INFO_WORDS          5
#define SMC_CMD_REG             0xFFFFFF9Bu     /* -101 */
#define SMC_REG_SFR_READ        0xC0000000u

/* B6CTRL3 for 260 MHz: 600 to 643.75 mV, the range of Samsung's table at that rate */
#define B6CTRL3_LEAST           0x30
#define B6CTRL3_MOST            0x37
#define B6CTRL3_650MHZ          0x41            /* 706.25 mV */
#define B11CTRL2_BOOT           0x48            /* 750 mV */
#define B11CTRL2_650MHZ         0x50            /* 800 mV */
#define USER_MUX_OSC            (1u << 27)
#define MUX_GATE                (1u << 21)
#define DIV_BUSY                (1u << 25)

/* the rates offered (mali_mhz, mali_gov), lowest first: G3D_PLL's M, P, S (PLL1418X: 26 / P 2-8 MHz, VCO 600-1200 MHz) */
static const struct g3d_rate { uint32_t mhz, pms, hpm; } rates[] = {
	{ 260, (160u << 16) | (4u << 8) | 2u, 1 },
	{ 546, (168u << 16) | (4u << 8) | 1u, 2 },
	{ 650, (100u << 16) | (4u << 8) | 0u, 2 },
};
#define SWITCH_HPM              1               /* the ECT's 455 MHz level: the switch's (528 MHz) */
static uint32_t mali_mhz = 260;
static bool asv_known, raised;                  /* this chip's ASV; the supplies at 650 MHz's */

static vm_offset_t pmu, top, ccore, g3d;
static uint8_t b6ctrl3;
static bool supply_read;

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

static inline uint32_t rd(vm_offset_t base, uint32_t offset) { return *(volatile uint32_t *)(base + offset); }

static uint32_t
pll_khz(uint32_t con0)
{
	uint32_t m = (con0 >> 16) & 0x3FF, p = (con0 >> 8) & 0x3F, s = con0 & 0x7;

	return (con0 & PLL_ENABLE) && p ? (uint32_t)(26000ull * m / ((uint64_t)p << s)) : 0;
}

/* One word of a secure block, read by EL3 (SMC_CMD_REG); false if it refused. */
static bool
secure_read(uint32_t address, uint32_t *value)
{
	register uint64_t x0 __asm__("x0") = SMC_CMD_REG;
	register uint64_t x1 __asm__("x1") = SMC_REG_SFR_READ | (address >> 2);
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	*value = (uint32_t)x2;
	return (uint32_t)x0 == 0;
}

/* the chip's ASV fuses, for the voltage Samsung's table gives its G3D (the header's "voltage") */
static void
say_asv(void)
{
	uint32_t w[ASV_INFO_WORDS] = { 0 };

	for (unsigned i = 0; i < ASV_INFO_WORDS; i++) {
		if (!secure_read((uint32_t)(ASV_INFO_PA + 4 * i), &w[i])) {
			say("mali: 1: the ASV fuses: EL3 refused to read %#lx\n", ASV_INFO_PA + 4 * i);
			return;
		}
	}
	say("mali: 1: the ASV fuses (%#lx): %08x %08x %08x %08x %08x; the G3D's group %u, correction %d, ssa0 %u, ssa10 %u, "
	    "ssa11 %u; the table's version %u%s\n", ASV_INFO_PA, w[0], w[1], w[2], w[3], w[4], w[1] & 0xF,
	    (int)((w[1] >> 4) & 0xF) - (((w[1] >> 4) & 0x8) ? 16 : 0), (w[1] >> 12) & 0xF, (w[1] >> 8) & 3, (w[1] >> 10) & 3,
	    (w[3] & 0x80) ? w[3] & 0x7F : 3, (w[3] & 0x80) ? "" : " (not fused: 3)");
	/* group 10, no correction or SSA; table 10, fused; g3dm_vthr1/2 0 (word 4 [11:8]) */
	asv_known = (w[1] & 0xFFFF) == 0x000A && (w[3] & 0xFF) == 0x8A && (w[4] & 0xF00) == 0;
}

/* 1: what S-Boot left, read only; CMU_G3D only with the domain on. */
static bool
look(void)
{
	uint32_t status, con0, user, mux;

	pmu = ml_io_map(PMU_PA, PMU_SIZE);
	top = ml_io_map(CMU_TOP_PA, 0x1000);
	ccore = ml_io_map(CMU_CCORE_PA, CMU_CCORE_SIZE);
	if (pmu == 0 || top == 0 || ccore == 0) {
		say("mali: 1: cannot map the PMU, CMU_TOP or CMU_CCORE\n");
		return false;
	}
	status = rd(pmu, G3D_STATUS);
	say("mali: 1: PMU: G3D configuration %#x status %#x (the domain %s), option %#x duration0 %#x; memory %#x/%#x; "
	    "GPU_DVS_CTRL %#x STATUS %#x (DVS %s)\n", rd(pmu, G3D_CONFIGURATION), status,
	    (status & G3D_ON) == G3D_ON ? "on" : (status & G3D_ON) == 0 ? "off" : "NEITHER ON NOR OFF", rd(pmu, G3D_OPTION),
	    rd(pmu, G3D_DURATION0), rd(pmu, MEMORY_G3D_CONFIG), rd(pmu, MEMORY_G3D_STATUS), rd(pmu, GPU_DVS_CTRL),
	    rd(pmu, GPU_DVS_STATUS), (rd(pmu, GPU_DVS_STATUS) & DVS_OFF) ? "off" : "ON");
	con0 = rd(top, G3D_PLL_CON0);
	say("mali: 1: CMU_TOP: G3D_PLL CON0 %#x (%s, %slocked, M %u P %u S %u: %u kHz) CON1 %#x LOCK %#x; MUX_G3D_PLL %#x; "
	    "BUS_PLL_G3D mux %#x div %#x; ACLK_CCORE_G3D_800 mux %#x div %#x\n", con0, (con0 & PLL_ENABLE) ? "on" : "off",
	    (con0 & PLL_LOCKED) ? "" : "not ", (con0 >> 16) & 0x3FF, (con0 >> 8) & 0x3F, con0 & 0x7, pll_khz(con0),
	    rd(top, G3D_PLL_CON1), rd(top, G3D_PLL_LOCK), rd(top, MUX_G3D_PLL), rd(top, MUX_SCLK_BUS_PLL_G3D),
	    rd(top, DIV_SCLK_BUS_PLL_G3D), rd(top, MUX_ACLK_CCORE_G3D_800), rd(top, DIV_ACLK_CCORE_G3D_800));
	say("mali: 1: CMU_CCORE: ACLK_CCORE_G3D_800_USER %#x; Q-channels LH_G3D0 %#x LH_G3D1 %#x LH_G3DP %#x LH_G3DIRAM "
	    "%#x\n", rd(ccore, MUX_ACLK_CCORE_G3D_800_USER), rd(ccore, QCH_LH_G3D0), rd(ccore, QCH_LH_G3D1),
	    rd(ccore, QCH_LH_G3DP), rd(ccore, QCH_LH_G3DIRAM));
	supply_read = hakios_s2mps16_g3d_supply(say, &b6ctrl3);
	say_asv();
	if ((status & G3D_ON) != G3D_ON) {
		say("mali: 1: the domain is not on: CMU_G3D is not read\n");
		return true;
	}
	if ((g3d = ml_io_map(CMU_G3D_PA, 0x1000)) == 0) {
		say("mali: 1: cannot map CMU_G3D\n");
		return false;
	}
	user = rd(g3d, MUX_G3D_PLL_USER);
	mux = rd(g3d, MUX_G3D);
	say("mali: 1: CMU_G3D: MUX_G3D_PLL_USER %#x MUX_BUS_PLL_USER %#x MUX_G3D %#x (the GPU on %s); DIV_ACLK %#x DIV_PCLK "
	    "%#x; ENABLE_ACLK %#x ACLK_BUS %#x PCLK %#x\n", user, rd(g3d, MUX_BUS_PLL_USER_G3D), mux,
	    (mux & MUX_SELECT) ? "the bus PLL" : (user & MUX_SELECT) ? "G3D_PLL" : "the oscillator", rd(g3d, DIV_ACLK_G3D),
	    rd(g3d, DIV_PCLK_G3D), rd(g3d, ENABLE_ACLK_G3D), rd(g3d, ENABLE_ACLK_G3D_BUS), rd(g3d, ENABLE_PCLK_G3D));
	return true;
}

static inline void wr(vm_offset_t base, uint32_t offset, uint32_t v) { *(volatile uint32_t *)(base + offset) = v; }
static uint32_t mhz_now(void);

static bool
wait_for(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want)
{
	for (unsigned i = 0; i < 100000; i++) {
		if ((rd(base, offset) & mask) == want) {
			return true;
		}
	}
	return false;
}

static bool
set_hpm(uint32_t hpm)
{
	wr(g3d, DIV_SCLK_HPM_G3D, (rd(g3d, DIV_SCLK_HPM_G3D) & ~3u) | hpm);
	return wait_for(g3d, DIV_SCLK_HPM_G3D, DIV_BUSY, 0);
}

/* the rate's level, or nrates when the supplies are not raised, G3D_PLL is at none of the rates or the G3D not on it */
static unsigned
rate_now(void)
{
	unsigned n = sizeof(rates) / sizeof(rates[0]);
	uint32_t con0 = rd(top, G3D_PLL_CON0);

	if (!raised || (rd(g3d, MUX_G3D) & MUX_SELECT) || (con0 & (PLL_ENABLE | PLL_LOCKED)) != (PLL_ENABLE | PLL_LOCKED)) {
		return n;
	}
	for (unsigned i = 0; i < n; i++) {
		if ((con0 & PLL_PMS) == rates[i].pms) {
			return i;
		}
	}
	return n;
}

/*
 * G3D_PLL to a rate through the switch clock (the header's "rate"), with interrupts off; NULL, or where it stopped. A
 * step that times out leaves the G3D where it is: on the switch (528 MHz) or on G3D_PLL, the supplies at 650 MHz's.
 */
static const char *
switch_rate(const struct g3d_rate *to)
{
	unsigned from_level = rate_now();
	uint32_t from_hpm = rd(g3d, DIV_SCLK_HPM_G3D) & 3u, saved[4];
	const char *stuck = NULL;
	boolean_t enabled;

	if (from_level == sizeof(rates) / sizeof(rates[0])) {
		return "G3D_PLL at none of the rates";
	}
	if (rates[from_level].mhz == to->mhz) {
		return NULL;
	}
	saved[0] = rd(top, MUX_SCLK_BUS_PLL_G3D);
	saved[1] = rd(top, DIV_SCLK_BUS_PLL_G3D);
	saved[2] = rd(top, ENABLE_SCLK_BUS_PLL_G3D);
	saved[3] = rd(g3d, MUX_BUS_PLL_USER_G3D);
	enabled = ml_set_interrupts_enabled(FALSE);
	/* the switch clock on: SCLK_BUS0_PLL / 1, gated through, taken by CMU_G3D's user mux */
	wr(top, MUX_SCLK_BUS_PLL_G3D, (saved[0] | MUX_GATE) & ~(7u << 12));
	wr(top, DIV_SCLK_BUS_PLL_G3D, saved[1] & ~0xFu);
	if (!wait_for(top, DIV_SCLK_BUS_PLL_G3D, DIV_BUSY, 0) ||
	    !wait_for(top, MUX_SCLK_BUS_PLL_G3D_STAT, 0xFFu << 12, 1u << 12)) {
		stuck = "the switch clock in CMU_TOP";
		goto out;
	}
	wr(top, ENABLE_SCLK_BUS_PLL_G3D, saved[2] | 1);
	wr(g3d, MUX_BUS_PLL_USER_G3D, saved[3] & ~USER_MUX_OSC);
	wr(g3d, MUX_BUS_PLL_USER_G3D, (saved[3] & ~USER_MUX_OSC) | MUX_SELECT);
	/* onto the switch */
	if (from_hpm < SWITCH_HPM && !set_hpm(SWITCH_HPM)) {
		stuck = "SCLK_HPM up to the switch's";
		goto out;
	}
	wr(g3d, MUX_G3D, rd(g3d, MUX_G3D) | MUX_SELECT);
	if (!wait_for(g3d, MUX_G3D_STAT, 3u << 12, 2u << 12)) {
		stuck = "MUX_G3D to the switch";
		goto out;
	}
	if (from_hpm > SWITCH_HPM && !set_hpm(SWITCH_HPM)) {
		stuck = "SCLK_HPM down to the switch's";
		goto out;
	}
	/* G3D_PLL relocked (_clk_pll141xx_set_pms: lock time P x 150, bit 26 cleared, bit 5 set) */
	wr(top, G3D_PLL_LOCK, 4 * 150);
	wr(top, G3D_PLL_CON0, (rd(top, G3D_PLL_CON0) & ~PLL_PMS & ~(1u << 26)) | to->pms | (1u << 5));
	if (!wait_for(top, G3D_PLL_CON0, PLL_LOCKED, PLL_LOCKED)) {
		stuck = "G3D_PLL's lock (the G3D stays on the switch)";
		goto out;
	}
	/* off the switch */
	if (to->hpm > SWITCH_HPM && !set_hpm(to->hpm)) {
		stuck = "SCLK_HPM up to the rate's";
		goto out;
	}
	wr(g3d, MUX_G3D, rd(g3d, MUX_G3D) & ~MUX_SELECT);
	if (!wait_for(g3d, MUX_G3D_STAT, 3u << 12, 1u << 12)) {
		stuck = "MUX_G3D back to G3D_PLL";
		goto out;
	}
	if (to->hpm < SWITCH_HPM && !set_hpm(to->hpm)) {
		stuck = "SCLK_HPM down to the rate's";
		goto out;
	}
	/* the switch clock as it was */
	wr(g3d, MUX_BUS_PLL_USER_G3D, saved[3]);
	wr(top, ENABLE_SCLK_BUS_PLL_G3D, saved[2]);
	wr(top, DIV_SCLK_BUS_PLL_G3D, saved[1]);
	(void)wait_for(top, DIV_SCLK_BUS_PLL_G3D, DIV_BUSY, 0);
	wr(top, MUX_SCLK_BUS_PLL_G3D, saved[0]);
out:
	(void)ml_set_interrupts_enabled(enabled);
	return stuck;
}

/* mali_mhz above 260: the supplies once to 650 MHz's in Samsung's order (BUCK11, then BUCK6), then the clock */
static bool
raise(void)
{
	const struct g3d_rate *to = NULL;
	uint32_t bus0 = rd(top, BUS0_PLL_CON0);
	const char *stuck;

	for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
		if (rates[i].mhz == mali_mhz) {
			to = &rates[i];
		}
	}
	if (to == NULL) {
		say("mali: 2: mali_mhz=%u is not one of 260, 546, 650: 260\n", mali_mhz);
		return true;
	}
	if (!asv_known) {
		say("mali: 2: mali_mhz=%u: not this chip's ASV (G3D group 10, table 10), whose voltages are all that is known "
		    "here: 260\n", mali_mhz);
		return true;
	}
	if (b6ctrl3 != B6CTRL3_LEAST || pll_khz(bus0) != 1056000 || !(rd(top, MUX_SCLK_BUS0_PLL) & MUX_SELECT)) {
		say("mali: 2: mali_mhz=%u: BUCK6 not at S-Boot's 600 mV, or the switch clock not BUS0_PLL / 2 (528 MHz): 260\n",
		    mali_mhz);
		return true;
	}
	if (!hakios_s2mps16_buck_volt(say, 11, B11CTRL2_BOOT, B11CTRL2_650MHZ)) {
		say("mali: 2: BUCK11 not raised: 260\n");
		return true;
	}
	if (!hakios_s2mps16_buck_volt(say, 6, B6CTRL3_LEAST, B6CTRL3_650MHZ)) {
		say("mali: 2: BUCK6 not raised: 260 (BUCK11 left at 800 mV, more than 260 MHz needs)\n");
		return true;
	}
	raised = true;
	b6ctrl3 = B6CTRL3_650MHZ;
	stuck = switch_rate(to);
	say("mali: 2: G3D_PLL %s%s: CON0 %#x, MUX_G3D %#x, SCLK_HPM %#x; %u MHz\n", stuck ? "STUCK at " : "moved",
	    stuck ? stuck : "", rd(top, G3D_PLL_CON0), rd(g3d, MUX_G3D), rd(g3d, DIV_SCLK_HPM_G3D), mhz_now());
	return stuck == NULL;
}

/*
 * 2: the block as S-Boot leaves it, then mali_mhz's rate (raise()). The GPU may be read if the block is as S-Boot leaves it: the domain on, DVS off, the GPU on
 * G3D_PLL locked at 260 MHz, and BUCK6 in the table's range for that rate.
 */
static bool
power(void)
{
	uint32_t con0 = rd(top, G3D_PLL_CON0);

	if ((rd(pmu, G3D_STATUS) & G3D_ON) != G3D_ON || g3d == 0) {
		say("mali: 2: the G3D domain is off, and powering it is not this glue's (not approved): the GPU is not touched\n");
		return false;
	}
	if (!(rd(pmu, GPU_DVS_STATUS) & DVS_OFF)) {
		say("mali: 2: DVS is on (BUCK6 at B6CTRL2's voltage): the GPU is not touched\n");
		return false;
	}
	if ((rd(g3d, MUX_G3D) & MUX_SELECT) || !(rd(g3d, MUX_G3D_PLL_USER) & MUX_SELECT) ||
	    (con0 & (PLL_ENABLE | PLL_LOCKED)) != (PLL_ENABLE | PLL_LOCKED) || (con0 & PLL_PMS) != PLL_260MHZ) {
		say("mali: 2: the GPU is not on G3D_PLL locked at 260 MHz (CON0 %#x, MUX_G3D %#x, MUX_G3D_PLL_USER %#x): not "
		    "touched\n", con0, rd(g3d, MUX_G3D), rd(g3d, MUX_G3D_PLL_USER));
		return false;
	}
	if (!supply_read || b6ctrl3 < B6CTRL3_LEAST || b6ctrl3 > B6CTRL3_MOST) {
		say("mali: 2: BUCK6 %s outside Samsung's 600-643 mV for 260 MHz: the GPU is not touched\n",
		    supply_read ? "is" : "could not be read, so it is taken as");
		return false;
	}
	say("mali: 2: the domain on, DVS off, the GPU on G3D_PLL at 260 MHz, BUCK6 at %u mV, as S-Boot left them\n",
	    300 + b6ctrl3 * 25 / 4);
	return mali_mhz == 260 || raise();
}

static bool
after_power(void)
{
	say("mali: 3: nothing: the block's power is as S-Boot left it\n");
	return true;
}

/* the steps ended short: mali_mhz's clock and supplies back to S-Boot's, Samsung's order down (clock, BUCK6, BUCK11) */
static void
undo(void)
{
	const char *stuck;

	if (!raised) {
		say("mali: the G3D block left as S-Boot left it (this glue wrote nothing)\n");
		return;
	}
	if ((stuck = switch_rate(&rates[0])) != NULL) {
		say("mali: G3D_PLL back to 260 MHz: stuck at %s; the supplies stay at 650 MHz's\n", stuck);
		return;
	}
	if (hakios_s2mps16_buck_volt(say, 6, B6CTRL3_650MHZ, B6CTRL3_LEAST)) {
		(void)hakios_s2mps16_buck_volt(say, 11, B11CTRL2_650MHZ, B11CTRL2_BOOT);
	}
	raised = false;
	say("mali: the G3D back at S-Boot's 260 MHz\n");
}

static uint32_t
mhz_now(void)
{
	return top != 0 ? pll_khz(rd(top, G3D_PLL_CON0)) / 1000 : 0;
}

static uint32_t
rate_mhz(unsigned level)
{
	return rates[level].mhz;
}

/* for mali_gov, between two jobs: the clock only, the supplies already at 650 MHz's */
static bool
set_rate(unsigned level)
{
	const char *stuck = raised ? switch_rate(&rates[level]) : "the supplies not raised";

	if (stuck != NULL) {
		say("mali: G3D_PLL to %u MHz: stuck at %s\n", rates[level].mhz, stuck);
	}
	return stuck == NULL;
}

/* the G3D zone as the TMU driver last read it (tmu=; hakios.exynos8890_tmu.c), for mali_gov's caps */
extern int32_t hakios_exynos_tmu_zone_mc(unsigned zone);

static int32_t
g3d_mc(void)
{
	return hakios_exynos_tmu_zone_mc(2);
}

static const struct hakios_mali_glue glue = {
	.name = "exynos8890",
	.gpu_pa = 0x14AC0000,
	.look = look,
	.power = power,
	.after_power = after_power,
	.undo = undo,
	.nrates = sizeof(rates) / sizeof(rates[0]),
	.rate_mhz = rate_mhz,
	.set_rate = set_rate,
	.rate_now = rate_now,
	.g3d_mc = g3d_mc,
	.mhz_now = mhz_now,
};

/* only on the Note FE: the loader's platform, /arm-io's device_type (the S9's SoC registers start at the same address) */
const struct hakios_mali_glue *
hakios_mali_glue_exynos8890(void)
{
	DTEntry node;
	void const *prop;
	unsigned int size;

	if (SecureDTLookupEntry(NULL, "/arm-io", &node) != kSuccess ||
	    SecureDTGetProperty(node, "device_type", &prop, &size) != kSuccess || strncmp(prop, "gracerlte", size) != 0) {
		say("mali: mali_soc=exynos8890 is the Galaxy Note FE's (the loader's platform gracerlte), and this is not it: "
		    "nothing touched\n");
		return NULL;
	}
	(void)PE_parse_boot_argn("mali_mhz", &mali_mhz, sizeof(mali_mhz));
	return &glue;
}
