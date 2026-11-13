/*
 * The Exynos 8890's cores started by the kernel (cpus=). Samsung's EL3 grants PSCI CPU_ON (it records the entry
 * point and answers 0) and leaves the core off; Samsung's kernel powers it from the PMU (exynos-pmu.c, exynos_cpu_up():
 * CPU_CONFIGURATION[3:0] = 0xF at 0x2000 + (cluster * 4 + core) * 0x80). psci_smp.c calls this after each granted
 * CPU_ON (patch 1513). No switch of its own: on this SoC a granted CPU_ON is nothing without it. Seen on the phone in the port's probe and in the loader (PLAN.md, Phases 0 and 2).
 *
 * The power sequencer's options this needs (the CPU part of Samsung's pwrcal_syspwr_init) are not set here: the loader
 * sets them before it powers the kernel's boot core the same way (xnuloader/loader.c, pmu_cpu_power_on), so they are
 * set in every run of this kernel on this SoC.
 *
 * The big cluster's rate (approved by Ege, 8 and 9 November 2026). S-Boot leaves the M1s at 728 MHz, the bottom of
 * Samsung's table (cl1_min_qos_freq). Two ways to move it, both through set_level():
 *   cpufreq_gov=1         the governor (drivers/power/hakios.exynos_cpufreq.c), whose 8890 glue this file is
 *                         (cpufreq_soc=exynos8890): 728 MHz and 1144 to 2288 MHz in Samsung's 104 MHz steps
 *   cpufreq_big=1456000   one fixed rate, when the governor is not asked for
 * A change, in Samsung's order (exynos-mp-cpufreq.c, S5E8890-dfs.c, pwrcal-dfs.c transition()): going up, BUCK2 raised
 * first to the new level's voltage (hakios.s2mps16.c); then, with interrupts off, the cluster moved to the switch clock
 * (CMU_TOP's SCLK_BUS_PLL_MNGS: BUS0_PLL, 1056 MHz, through MNGS's user mux) with the dividers of the switch's level
 * (1040 MHz's), MNGS_PLL relocked, the new level's dividers, and the cluster moved back; CMU_TOP's switch registers then
 * as they were; going down, BUCK2 lowered last. 832 to 1040 MHz are left out: between two rates under 1056 MHz Samsung
 * switches through 528 MHz instead, and without them every change here is the one sequence. The lowest voltage on the
 * switch is 1144 MHz's 725 mV, above 1040 MHz's 700.
 *
 * The values are the ECT's (the stock sboot.bin, at 0x38c00: PLL block MNGS_PLL, dvfs_big's dividers; tools/ect.py) and
 * the voltages this chip's (ASV table 10, big group 9, no SSA correction, + the MARGIN block's 25 mV: what
 * S5E8890-asv.c computes; tools/ectvolt.py). Done only on that chip, from the state S-Boot leaves (PLL, muxes, dividers,
 * BUCK2), else nothing is written. S-Boot's MNGS_EMA_CON, 0x1091B9, is kept: it is what Samsung's kernel writes from 900
 * to 1106 mV (dfsbig_set_ema), and what S-Boot itself runs 675 mV with. A wait that times out stops the cluster where
 * it is, and nothing more is written: every step on the way runs at or under the faster of the two rates, at its
 * voltage.
 *
 * Samsung's limits, the governor's through this glue: 2288000 with three or four M1s online (the tree's
 * cl1_max_support_idx_table; 2600000 for one or two is not used); 1560000 while the GPU runs above 600 MHz
 * (gpu_exynos8890.c's gpu_dvfs_table_default: 650 MHz's cpu max lock), read from G3D_PLL each time; and without the
 * TMU's temperatures (tmu=) no more than the fixed rate's 1456000. Cooling as the tree's tmuctrl@10060000 has it: 1872000
 * from 76 C, 1352000 from 81, 728000 from 86, and from 96 (its hotplug_out_threshold) the lowest level. The tree's MIF
 * minimum for a big rate (cl1_dvfs_table's last column, 1716000 at 2288000) holds as S-Boot leaves MIF: 1794 MHz.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>

#include "hakios.cpufreq_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);

extern bool hakios_s2mps16_buck_volt(int (*say)(const char *, ...), unsigned buck, uint8_t from, uint8_t to);
extern bool hakios_exynos_cpufreq_gov_start(void);     /* drivers/power/hakios.exynos_cpufreq.c; stubs.c */

void hakios_psci_cpu_on_started(int cpu, uint32_t mpidr);
void hakios_exynos8890_cpu_init(void);

#define PMU_CPU_PA              0x105C2000UL
#define CPU_CONFIGURATION(slot) ((slot) * 0x80u)
#define CPU_STATUS(slot)        ((slot) * 0x80u + 0x4u)

/* The rate this core really runs at, in MHz: 32 dependent additions a loop, one a cycle on the M1, timed. */
static uint32_t
measure_mhz(void)
{
	uint64_t loops = 8192, x = 0, began, took, ns;
	boolean_t enabled = ml_set_interrupts_enabled(FALSE);

	began = mach_absolute_time();
	__asm__ volatile ("1:\n.rept 32\nadd %0, %0, #1\n.endr\nsubs %1, %1, #1\nb.ne 1b\n" : "+r" (x), "+r" (loops) : : "cc");
	took = mach_absolute_time() - began;
	(void)ml_set_interrupts_enabled(enabled);
	absolutetime_to_nanoseconds(took, &ns);
	return ns ? (uint32_t)(32ull * 8192 * 1000 / ns) : 0;
}

/*
 * The clocks as S-Boot leaves them, read once for the frequency driver to come (S5E8890-cmusfr.h): each block's PLL
 * (CON0: M [25:16], P [13:8], S [2:0], 26 MHz x M / (P << S)), its muxes at +0x200/+0x204/+0x208 and divider at +0x400;
 * the bus PLLs in CMU_TOP; the rate this core (an M1: the boot core) really runs at. Nothing written.
 */
static void
say_clocks(void)
{
	static const struct { const char *name; uint32_t pa; } blocks[] = {
		{ "MNGS", 0x11800000 }, { "APOLLO", 0x11900000 },
		{ "MIF0", 0x10850000 }, { "MIF1", 0x10950000 }, { "MIF2", 0x10A50000 }, { "MIF3", 0x10B50000 },
	};
	static bool said;
	char line[200];
	vm_offset_t va;

	if (said) {
		return;
	}
	said = true;
	for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
		if ((va = ml_io_map(blocks[i].pa, 0x1000)) == 0) {
			continue;
		}
		uint32_t con0 = *(volatile uint32_t *)(va + 0x100), p = con0 >> 8 & 0x3f;
		snprintf(line, sizeof(line), "exynos8890: %s PLL %08x %08x (%u MHz), muxes %x %x %x, divider %x\n",
		    blocks[i].name, con0, *(volatile uint32_t *)(va + 0x104),
		    p != 0 ? 26 * (con0 >> 16 & 0x3ff) / (p << (con0 & 7)) : 0, *(volatile uint32_t *)(va + 0x200),
		    *(volatile uint32_t *)(va + 0x204), *(volatile uint32_t *)(va + 0x208), *(volatile uint32_t *)(va + 0x400));
		loader_log_line(line);
	}
	if ((va = ml_io_map(0x10570000, 0x1000)) != 0) {
		uint32_t mhz[4];

		for (unsigned i = 0; i < 4; i++) {
			uint32_t con0 = *(volatile uint32_t *)(va + 0x100 + 0x20 * i), p = con0 >> 8 & 0x3f;
			mhz[i] = p != 0 ? 26 * (con0 >> 16 & 0x3ff) / (p << (con0 & 7)) : 0;
		}
		snprintf(line, sizeof(line), "exynos8890: CMU_TOP BUS0-3 PLLs %u %u %u %u MHz\n", mhz[0], mhz[1], mhz[2], mhz[3]);
		loader_log_line(line);
		/* the touch bus's input clock (HSI2C7: ACLK_PERIC1_66), for hakios.s6sy761.c's ipclk_mhz */
		snprintf(line, sizeof(line), "exynos8890: ACLK_PERIC1_66: CMU_TOP mux %x, divider %x\n",
		    *(volatile uint32_t *)(va + 0x2A4), *(volatile uint32_t *)(va + 0x404));
		loader_log_line(line);
	}
	snprintf(line, sizeof(line), "exynos8890: this M1 runs at %u MHz (measured)\n", measure_mhz());
	loader_log_line(line);
}

void
hakios_psci_cpu_on_started(int cpu, uint32_t mpidr)
{
	static vm_offset_t pmu;
	uint32_t cluster = (mpidr >> 8) & 0xff, core = mpidr & 0xff, slot = cluster * 4 + core;
	volatile uint32_t *config;
	uint32_t before, status;
	char line[160];

	if (cluster > 1 || core > 3) {
		snprintf(line, sizeof(line), "exynos8890: cpu %d (MPIDR 0x%x) is no core of the 8890: not powered\n", cpu, mpidr);
		goto out;
	}
	if (pmu == 0 && (pmu = ml_io_map(PMU_CPU_PA, 0x1000)) == 0) {
		snprintf(line, sizeof(line), "exynos8890: cannot map the PMU: cpu %d is not powered\n", cpu);
		goto out;
	}
	config = (volatile uint32_t *)(pmu + CPU_CONFIGURATION(slot));
	before = *config;
	status = *(volatile uint32_t *)(pmu + CPU_STATUS(slot));
	*config |= 0xF;
	__asm__ volatile ("dsb sy" ::: "memory");
	snprintf(line, sizeof(line), "exynos8890: cpu %d (MPIDR 0x%x) powered by the PMU: CONFIGURATION 0x%x -> 0x%x STATUS "
	    "0x%x -> 0x%x\n", cpu, mpidr, before, *config, status, *(volatile uint32_t *)(pmu + CPU_STATUS(slot)));
	say_clocks();
out:
	loader_log_line(line);
	printf("%s", line);
}

/* the big cluster's rate (the header's last paragraph); registers of S5E8890-cmusfr.h */
#define CMU_MNGS_PA             0x11800000UL
#define  MNGS_PLL_LOCK          0x000
#define  MNGS_PLL_CON0          0x100
#define  MUX_MNGS_PLL           0x200
#define  MUX_BUS_PLL_MNGS_USER  0x204
#define  MUX_MNGS               0x208
#define  MUX_MNGS_STAT          0x608
#define CMU_TOP_PA              0x10570000UL
#define  BUS0_PLL_CON0          0x100
#define  G3D_PLL_CON0           0x1E0
#define  MUX_SCLK_BUS_PLL_MNGS  0x380
#define  MUX_SCLK_BUS_PLL_MNGS_STAT 0x680
#define  DIV_SCLK_BUS_PLL_MNGS  0x4F0
#define  ENABLE_SCLK_BUS_PLL_MNGS 0x990
#define SYSREG_MNGS_PA          0x11850000UL
#define  MNGS_EMA_CON           0x314
#define SELECT                  (1u << 12)
#define MUX_GATE                (1u << 21)      /* a mux's output gate */
#define USER_MUX_OSC            (1u << 27)      /* cleared before a user mux takes its source */
#define DIV_BUSY                (1u << 25)
#define PLL_ENABLE              (1u << 31)
#define PLL_LOCKED              (1u << 29)
#define PLL_PMS                 0x03FF3F07u
#define PMS(m, p, s)            ((m) << 16 | (p) << 8 | (s))
#define ASV_INFO_PA             0x101E9000UL
#define SMC_CMD_REG             0xFFFFFF9Bu     /* -101: a word of a secure block, as hakios.exynos8890_g3d.c reads it */
#define EMA_UNDER_900MV         0x001095B9u     /* dfsbig_set_ema() for under 900 mV: both levels here */

/* the ECT's dvfs_big dividers, in its column order: register and field width */
static const struct { uint16_t offset; uint8_t width; } dividers[12] = {
	{ 0x400, 6 }, { 0x404, 4 }, { 0x424, 6 }, { 0x408, 4 }, { 0x41C, 6 }, { 0x428, 4 },     /* MNGS ACLK PCLK ATCLK_CORE PCLK_DBG CNTCLK */
	{ 0x434, 3 }, { 0x430, 3 }, { 0x40C, 6 }, { 0x410, 4 }, { 0x414, 4 }, { 0x418, 4 },     /* MNGS_PLL PROMISE ATCLK_SOC TRACE CAM1 AUD */
};

struct level {
	uint32_t khz, pms;
	uint8_t vsel;                   /* B2CTRL2: this chip's voltage (ASV table 10, group 9) */
	uint8_t div[12];
};
static const struct level levels[] = {
	{ 2288000, PMS(264, 3, 0), 0x72, { 0, 2, 7, 4, 7, 7, 1, 6, 3, 3, 7, 7 } },      /* 1012.5 mV */
	{ 2184000, PMS(252, 3, 0), 0x6D, { 0, 2, 7, 4, 7, 7, 1, 6, 3, 3, 7, 7 } },      /* 981.25 mV */
	{ 2080000, PMS(240, 3, 0), 0x69, { 0, 2, 7, 4, 7, 7, 1, 6, 3, 3, 7, 7 } },      /* 956.25 mV */
	{ 1976000, PMS(228, 3, 0), 0x65, { 0, 2, 7, 4, 7, 7, 1, 5, 3, 3, 7, 7 } },      /* 931.25 mV */
	{ 1872000, PMS(216, 3, 0), 0x61, { 0, 2, 7, 4, 7, 7, 1, 5, 3, 3, 7, 7 } },      /* 906.25 mV */
	{ 1768000, PMS(204, 3, 0), 0x5C, { 0, 2, 7, 4, 7, 7, 1, 5, 3, 3, 7, 7 } },      /* 875 mV */
	{ 1664000, PMS(192, 3, 0), 0x58, { 0, 2, 7, 4, 7, 7, 1, 4, 3, 3, 7, 7 } },      /* 850 mV */
	{ 1560000, PMS(360, 3, 1), 0x53, { 0, 2, 7, 4, 7, 7, 1, 4, 3, 3, 7, 7 } },      /* 818.75 mV */
	{ 1456000, PMS(336, 3, 1), 0x4F, { 0, 2, 7, 4, 7, 7, 1, 4, 3, 3, 7, 7 } },      /* 793.75 mV */
	{ 1352000, PMS(312, 3, 1), 0x4B, { 0, 2, 7, 4, 7, 7, 1, 3, 3, 3, 7, 7 } },      /* 768.75 mV */
	{ 1248000, PMS(288, 3, 1), 0x47, { 0, 2, 7, 4, 7, 7, 1, 3, 3, 3, 7, 7 } },      /* 743.75 mV */
	{ 1144000, PMS(264, 3, 1), 0x44, { 0, 2, 7, 4, 7, 7, 1, 3, 3, 3, 7, 7 } },      /* 725 mV */
	{  728000, PMS(336, 3, 2), 0x3C, { 0, 2, 5, 4, 7, 5, 1, 2, 3, 3, 7, 7 } },      /* 675 mV; S-Boot's */
};
#define NLEVELS                 (sizeof(levels) / sizeof(levels[0]))
#define AT728                   (NLEVELS - 1)
#define AT1456                  8
static const struct level at1040 = { 1040000, 0, 0, { 0, 2, 6, 4, 7, 7, 1, 3, 3, 3, 7, 7 } };    /* the switch's level */

static vm_offset_t mngs, top;
static unsigned level_now = AT728;
static uint8_t vsel_now = 0x3C;         /* BUCK2 as last written (S-Boot's 675 mV) */
static bool ready, stuck_for_good;
static uint32_t changes;

static inline uint32_t rd(vm_offset_t base, uint32_t offset) { return *(volatile uint32_t *)(base + offset); }
static inline void wr(vm_offset_t base, uint32_t offset, uint32_t v) { *(volatile uint32_t *)(base + offset) = v; }

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

/* pwrcal-dfs.c's dfs_trans_div: the dividers that rise (up) or fall (!up) from one level to the other */
static bool
move_dividers(const struct level *from, const struct level *to, bool up)
{
	for (unsigned i = 0; i < 12; i++) {
		uint32_t mask = (1u << dividers[i].width) - 1;

		if (up ? to->div[i] <= from->div[i] : to->div[i] >= from->div[i]) {
			continue;
		}
		wr(mngs, dividers[i].offset, (rd(mngs, dividers[i].offset) & ~mask) | to->div[i]);
		if (!wait_for(mngs, dividers[i].offset, DIV_BUSY, 0)) {
			return false;
		}
	}
	return true;
}

static bool
secure_read(uint32_t address, uint32_t *value)
{
	register uint64_t x0 __asm__("x0") = SMC_CMD_REG;
	register uint64_t x1 __asm__("x1") = 0xC0000000u | (address >> 2);
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;

	__asm__ volatile ("dsb sy\n\tsmc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	*value = (uint32_t)x2;
	return (uint32_t)x0 == 0;
}

static int
say(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/* Why the cluster may not be moved from what S-Boot left, or NULL. */
static const char *
refusal(void)
{
	uint32_t asv0 = 0, asv3 = 0, con0 = rd(mngs, MNGS_PLL_CON0), bus0 = rd(top, BUS0_PLL_CON0);
	vm_offset_t sysreg = ml_io_map(SYSREG_MNGS_PA, 0x1000);

	if (!secure_read(ASV_INFO_PA, &asv0) || !secure_read(ASV_INFO_PA + 12, &asv3)) {
		return "EL3 would not read the ASV fuses";
	}
	/* word 0 [15:0]: the big group 9, its correction, SSA10/11 and SSA0 all 0; word 3: table 10, fused */
	if ((asv0 & 0xFFFF) != 0x0009 || (asv3 & 0xFF) != 0x8A) {
		return "not this chip's ASV (big group 9, table 10): its voltages are not known here";
	}
	if ((con0 & (PLL_ENABLE | PLL_LOCKED)) != (PLL_ENABLE | PLL_LOCKED) || (con0 & PLL_PMS) != levels[AT728].pms) {
		return "MNGS_PLL is not locked at 728 MHz";
	}
	if (!(rd(mngs, MUX_MNGS_PLL) & SELECT) || (rd(mngs, MUX_MNGS) & SELECT) || (rd(mngs, MUX_BUS_PLL_MNGS_USER) & SELECT)) {
		return "the cluster is not on MNGS_PLL";
	}
	for (unsigned i = 0; i < 12; i++) {
		if ((rd(mngs, dividers[i].offset) & ((1u << dividers[i].width) - 1)) != levels[AT728].div[i]) {
			return "the dividers are not the ECT's for 728 MHz";
		}
	}
	if ((bus0 & (PLL_ENABLE | PLL_LOCKED)) != (PLL_ENABLE | PLL_LOCKED) || (bus0 >> 8 & 0x3F) == 0 ||
	    26 * (bus0 >> 16 & 0x3FF) / ((bus0 >> 8 & 0x3F) << (bus0 & 7)) != 1056 ||
	    (rd(top, MUX_SCLK_BUS_PLL_MNGS) & (3u << 12)) != 0) {
		return "the switch clock is not BUS0_PLL at 1056 MHz";
	}
	if (sysreg != 0) {
		say("exynos8890: cpufreq: MNGS_EMA_CON %x, kept (Samsung's kernel writes %x under 900 mV, 1091b9 from there)\n",
		    rd(sysreg, MNGS_EMA_CON), EMA_UNDER_900MV);
	}
	return NULL;
}

/* The cluster from one level's clocks to another's, interrupts off; NULL, or where it stopped. BUCK2 is the caller's. */
static const char *
move(const struct level *from, const struct level *to)
{
	uint32_t saved[4] = { rd(top, MUX_SCLK_BUS_PLL_MNGS), rd(top, DIV_SCLK_BUS_PLL_MNGS), rd(top, ENABLE_SCLK_BUS_PLL_MNGS),
		              rd(mngs, MUX_BUS_PLL_MNGS_USER) };
	boolean_t enabled = ml_set_interrupts_enabled(FALSE);
	const char *stuck = NULL;

	/* the switch clock on: BUS0_PLL / 1, gated through, taken by MNGS's user mux (dfs_set_rate_switch, enable_switch) */
	wr(top, MUX_SCLK_BUS_PLL_MNGS, (saved[0] | MUX_GATE) & ~(3u << 12));
	wr(top, DIV_SCLK_BUS_PLL_MNGS, saved[1] & ~0xFu);
	if (!wait_for(top, DIV_SCLK_BUS_PLL_MNGS, DIV_BUSY, 0) ||
	    !wait_for(top, MUX_SCLK_BUS_PLL_MNGS_STAT, 0xFu << 12, 1u << 12)) {
		stuck = "the switch clock in CMU_TOP";
		goto out;
	}
	wr(top, ENABLE_SCLK_BUS_PLL_MNGS, saved[2] | 1);
	wr(mngs, MUX_BUS_PLL_MNGS_USER, saved[3] & ~USER_MUX_OSC);
	wr(mngs, MUX_BUS_PLL_MNGS_USER, (saved[3] & ~USER_MUX_OSC) | SELECT);
	/* onto the switch */
	if (!move_dividers(from, &at1040, true)) {
		stuck = "the dividers up to the switch's";
		goto out;
	}
	wr(mngs, MUX_MNGS, rd(mngs, MUX_MNGS) | SELECT);
	if (!wait_for(mngs, MUX_MNGS_STAT, 3u << 12, 2u << 12)) {
		stuck = "the cluster's mux to the switch";
		goto out;
	}
	if (!move_dividers(from, &at1040, false)) {
		stuck = "the dividers down to the switch's";
		goto out;
	}
	/* MNGS_PLL relocked (_clk_pll141xx_set_pms: lock time P x 150, bit 26 cleared, bit 5 set) */
	wr(mngs, MNGS_PLL_LOCK, 3 * 150);
	wr(mngs, MNGS_PLL_CON0, ((rd(mngs, MNGS_PLL_CON0) & ~PLL_PMS & ~(1u << 26)) | to->pms | (1u << 5)));
	if (!wait_for(mngs, MNGS_PLL_CON0, PLL_LOCKED, PLL_LOCKED)) {
		stuck = "MNGS_PLL's lock (the cluster stays on the switch)";
		goto out;
	}
	/* off the switch */
	if (!move_dividers(&at1040, to, true)) {
		stuck = "the dividers up to the new level's";
		goto out;
	}
	wr(mngs, MUX_MNGS, rd(mngs, MUX_MNGS) & ~SELECT);
	if (!wait_for(mngs, MUX_MNGS_STAT, 3u << 12, 1u << 12)) {
		stuck = "the cluster's mux back to MNGS_PLL";
		goto out;
	}
	if (!move_dividers(&at1040, to, false)) {
		stuck = "the dividers down to the new level's";
		goto out;
	}
	/* the switch clock as it was (disable_switch) */
	wr(mngs, MUX_BUS_PLL_MNGS_USER, saved[3]);
	wr(top, ENABLE_SCLK_BUS_PLL_MNGS, saved[2]);
	wr(top, DIV_SCLK_BUS_PLL_MNGS, saved[1]);
	(void)wait_for(top, DIV_SCLK_BUS_PLL_MNGS, DIV_BUSY, 0);
	wr(top, MUX_SCLK_BUS_PLL_MNGS, saved[0]);
out:
	(void)ml_set_interrupts_enabled(enabled);
	return stuck;
}

static int
quiet(__unused const char *format, ...)
{
	return 0;
}

/* BUCK2 from what it was last written to; its own log line for the first changes and for every failure */
static bool
buck2(uint8_t vsel)
{
	if (hakios_s2mps16_buck_volt(changes < 4 ? say : quiet, 2, vsel_now, vsel)) {
		vsel_now = vsel;
		return true;
	}
	say("exynos8890: cpufreq: BUCK2 %02x -> %02x not written; it stays\n", vsel_now, vsel);
	return false;
}

/* The cluster to levels[to] (the header's "A change"); the time with interrupts off in *us. */
static bool
set_level(unsigned to, uint32_t *us)
{
	const struct level *from = &levels[level_now], *t = &levels[to];
	uint64_t began, ns;
	const char *stuck;

	*us = 0;
	if (!ready || stuck_for_good || to >= NLEVELS) {
		return false;
	}
	if (to == level_now) {
		return true;
	}
	if (t->vsel > vsel_now && !buck2(t->vsel)) {
		return false;
	}
	began = mach_absolute_time();
	stuck = move(from, t);
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	*us = (uint32_t)(ns / NSEC_PER_USEC);
	if (stuck != NULL) {
		stuck_for_good = true;
		say("exynos8890: cpufreq: %u -> %u kHz: STUCK at %s: MNGS_PLL %x, mux %x, BUCK2 %02x; no more changes\n", from->khz,
		    t->khz, stuck, rd(mngs, MNGS_PLL_CON0), rd(mngs, MUX_MNGS), vsel_now);
		return false;
	}
	level_now = to;
	changes++;
	if (t->vsel < vsel_now) {
		(void)buck2(t->vsel);           /* (left higher if it does not take: more than the level needs) */
	}
	return true;
}

/* What a change needs: the registers mapped, and the cluster as S-Boot leaves it on the chip the voltages are for. */
static bool
init(int (*say_as)(const char *, ...))
{
	const char *no;

	if (ready) {
		return true;
	}
	mngs = ml_io_map(CMU_MNGS_PA, 0x1000);
	top = ml_io_map(CMU_TOP_PA, 0x1000);
	if (mngs == 0 || top == 0) {
		say_as("exynos8890: cpufreq: cannot map CMU_MNGS or CMU_TOP\n");
		return false;
	}
	say_as("exynos8890: cpufreq: the switch as found: CMU_TOP mux %x div %x enable %x, MNGS user mux %x\n",
	    rd(top, MUX_SCLK_BUS_PLL_MNGS), rd(top, DIV_SCLK_BUS_PLL_MNGS), rd(top, ENABLE_SCLK_BUS_PLL_MNGS),
	    rd(mngs, MUX_BUS_PLL_MNGS_USER));
	if ((no = refusal()) != NULL) {
		say_as("exynos8890: cpufreq: %s: nothing written, the M1s stay at 728 MHz\n", no);
		return false;
	}
	ready = true;
	return true;
}

/* the governor's glue (hakios.cpufreq_glue.h); domain 1, the M1s, is the only one with levels */
static int
glue_set(__unused unsigned domain, uint32_t khz, uint32_t answer[4])
{
	unsigned to = 0;
	uint32_t us;
	bool done;

	while (to < NLEVELS && levels[to].khz != khz) {
		to++;
	}
	answer[0] = levels[level_now].khz;
	done = set_level(to, &us);
	answer[1] = levels[level_now].khz;
	answer[2] = us;                 /* with interrupts off */
	answer[3] = vsel_now;
	return done ? 0 : EIO;
}

static uint32_t
glue_table(unsigned domain, uint32_t *khz, uint32_t *uv, uint32_t most, __unused int (*say_as)(const char *, ...))
{
	if (domain != 1 || most < NLEVELS) {
		return 0;
	}
	for (unsigned i = 0; i < NLEVELS; i++) {
		khz[i] = levels[i].khz;
		uv[i] = 300000 + levels[i].vsel * 6250u;
	}
	return NLEVELS;
}

static uint32_t
pll_khz(uint32_t con0)
{
	uint32_t p = con0 >> 8 & 0x3F;

	return p != 0 ? 26000 * (con0 >> 16 & 0x3FF) / (p << (con0 & 7)) : 0;
}

static int
glue_get(__unused unsigned domain, uint32_t *khz)
{
	*khz = (rd(mngs, MUX_MNGS) & SELECT) ? 0 : pll_khz(rd(mngs, MNGS_PLL_CON0));     /* 0 on the switch: not said */
	return 0;
}

/* the header's "Samsung's limits" */
static uint32_t
glue_big_limit(__unused uint32_t online, bool temperatures)
{
	uint32_t limit = temperatures ? 2288000 : 1456000;

	return pll_khz(rd(top, G3D_PLL_CON0)) > 600000 ? MIN(limit, 1560000) : limit;
}

/* the tree's tmuctrl@10060000: freq_clip_max_0-2 at cooling_level_0-2 */
static const struct hakios_cpufreq_clip clips[] = { { 76000, 1872000 }, { 81000, 1352000 }, { 86000, 728000 } };

static const struct hakios_cpufreq_glue glue = {
	.name = "exynos8890",
	.setter = "the CMU",
	.init = init,
	.domain = {
		[1] = { .affinity = 0, .control_mc = 76000, .min = 728000, .max = 2288000, .max_default = 2288000,
			.floor_default = 728000, .touch_khz = 1872000, .frame_khz = 1456000, .clips = clips,
			.n_clips = sizeof(clips) / sizeof(clips[0]) },
	},
	.table = glue_table,
	.set = glue_set,
	.get = glue_get,
	.big_limit = glue_big_limit,
	.big_limit_text = "; 1560000 while the GPU runs above 600 MHz; 1456000 without temperatures",
};

const struct hakios_cpufreq_glue *
hakios_cpufreq_glue_exynos8890(void)
{
	return &glue;
}

void
hakios_exynos8890_cpu_init(void)
{
	uint32_t want = 0, before, us;
	bool done;

	if (hakios_exynos_cpufreq_gov_start()) {
		return;         /* cpufreq_gov=1: the governor moves the rate, through the glue above */
	}
	if (!PE_parse_boot_argn("cpufreq_big", &want, sizeof(want)) || want == 0) {
		return;
	}
	if (want != levels[AT1456].khz) {
		say("exynos8890: cpufreq_big=%u: only 1456000 is a fixed rate here; the M1s stay at 728 MHz\n", want);
		return;
	}
	if (!init(say)) {
		return;
	}
	before = measure_mhz();
	done = set_level(AT1456, &us);
	say("exynos8890: cpufreq_big: %s; measured %u MHz before, %u after (%u us with interrupts off); MNGS_PLL %x, mux %x\n",
	    done ? "the M1s at 1456 MHz, BUCK2 at 793.75 mV" : "NOT DONE", before, measure_mhz(), us, rd(mngs, MNGS_PLL_CON0),
	    rd(mngs, MUX_MNGS));
}
