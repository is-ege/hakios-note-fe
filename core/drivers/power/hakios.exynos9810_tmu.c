/*
 * The Exynos 9810's side of the TMU driver (hakios.exynos_tmu.c, whose header describes it; tmu_soc=exynos9810, the
 * default): the Galaxy S9's two blocks and four zones, the TMU's PCLK gate in CMU_PERIS, tmu_enable's start and tmu_off's
 * PS_HOLD (hakios.exynos_poweroff.c).
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>

#include "hakios.exynos_tmu.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern bool hakios_exynos_power_off(const char *who, int (*say)(const char *, ...));  /* hakios.exynos_poweroff.c */
extern void delay(int usec);

#define CMU_PERIS_PA            0x10020000u     /* cmucal-sfr.c:36 */
#define CMU_PERIS_TMU_GATE      0x2014u         /* :1656 CLK_CON_GAT_GOUT_BLK_PERIS_UID_BUSIF_TMU_IPCLKPORT_PCLK */
#define CMU_PERIS_TMU_QCH       0x3004u         /* :1675 QCH_CON_BUSIF_TMU_QCH */
#define GATE_MANUAL             (1u << 20)      /* :7017 */
#define GATE_CG_VAL             (1u << 21)      /* :7016 */

static inline uint32_t
rd(uintptr_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(uintptr_t base, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
clock_open(int (*say)(const char *, ...), bool ungate)
{
	say("tmu: 1: reading CMU_PERIS's TMU gate (%#x +%#x) and Q-channel (+%#x)\n", CMU_PERIS_PA, CMU_PERIS_TMU_GATE, CMU_PERIS_TMU_QCH);
	vm_offset_t cmu_va = ml_io_map(CMU_PERIS_PA, 0x4000);
	if (cmu_va == 0) {
		say("tmu: could not map CMU_PERIS\n");
		return false;
	}
	uint32_t gate = rd(cmu_va, CMU_PERIS_TMU_GATE);
	bool closed = (gate & GATE_MANUAL) && !(gate & GATE_CG_VAL);

	say("tmu: 1: gate %08x (MANUAL %u, CG_VAL %u)%s, Q-channel %08x (ENABLE %u, CLOCK_REQ %u)\n", gate, gate & GATE_MANUAL ? 1 : 0,
	    gate & GATE_CG_VAL ? 1 : 0, closed ? ": CLOSED" : ": open", rd(cmu_va, CMU_PERIS_TMU_QCH), rd(cmu_va, CMU_PERIS_TMU_QCH) & 1,
	    (rd(cmu_va, CMU_PERIS_TMU_QCH) >> 1) & 1);
	if (closed && ungate) {
		say("tmu: 1: tmu_ungate=1: CG_VAL set\n");
		wr(cmu_va, CMU_PERIS_TMU_GATE, gate | GATE_CG_VAL);
		closed = !(rd(cmu_va, CMU_PERIS_TMU_GATE) & GATE_CG_VAL);
		say("tmu: 1: gate now %08x\n", rd(cmu_va, CMU_PERIS_TMU_GATE));
	}
	return !closed;
}

/* tmu_enable=1: exynos9810_tmu_control()'s configuration and CORE_EN, never TRIP_EN, thresholds or interrupts */
static void
start_block(int (*say)(const char *, ...), const char *name, uintptr_t va, uintptr_t top)
{
	uint32_t ctrl = rd(va, TMU_CONTROL), v, want;

	if (ctrl & TMU_CTRL_CORE_EN) {
		say("tmu: enable: %s already runs (CONTROL %08x): left alone\n", name, ctrl);
		return;
	}
	say("tmu: enable: %s is stopped (CONTROL %08x): configuring it as Android does, without the hardware trip\n", name, ctrl);
	want = ctrl & ~((0x1fu << TMU_CTRL_VREF_SHIFT) | (0xfu << TMU_CTRL_SLOPE_SHIFT) | TMU_CTRL_TRIP_EN);
	want |= TMU_TRIM_FIELD(rd(va, TMU_TRIMINFO(0))) << TMU_CTRL_VREF_SHIFT;
	want |= TMU_TRIM_FIELD4(rd(va, TMU_TRIMINFO(1))) << TMU_CTRL_SLOPE_SHIFT;
	wr(va, TMU_CONTROL, want);
	v = rd(va, TMU_CONTROL1);
	wr(va, TMU_CONTROL1, v & ~TMU_CON1_LPI);
	uint32_t mode = TMU_TRIM_AVG_MODE(rd(va, TMU_TRIMINFO(2)));
	v = rd(va, TMU_AVG_CON) & ~(TMU_AVG_MODE_MASK | TMU_AVG_DEM);
	wr(va, TMU_AVG_CON, mode ? (v | mode | TMU_AVG_DEM) : v);
	wr(va, TMU_COUNTER_VALUE0, (rd(va, TMU_COUNTER_VALUE0) & ~0xffffu) | TMU_SENSE_VALUE);
	wr(va, TMU_COUNTER_VALUE1, (rd(va, TMU_COUNTER_VALUE1) & ~0xffff0000u) | (TMU_SENSE_VALUE << 16));
	v = rd(va, TMU_TRIM0) & ~((0xfu << 20) | (0xfu << 12) | (0xfu << 8));
	v |= TMU_TRIM_FIELD4(rd(top, TMU_TRIMINFO(3))) << 20;
	v |= TMU_TRIM_FIELD4(rd(top, TMU_TRIMINFO(4))) << 12;
	v |= TMU_TRIM_FIELD4(rd(top, TMU_TRIMINFO(5))) << 8;
	wr(va, TMU_TRIM0, v);
	wr(va, TMU_CONTROL, rd(va, TMU_CONTROL) | TMU_CTRL_CORE_EN);
	delay(10 * 1000);
	say("tmu: enable: %s now CONTROL %08x (wanted %08x), CONTROL1 %08x, AVG_CON %08x, COUNTER %08x/%08x, TRIM0 %08x (wanted %08x)\n",
	    name, rd(va, TMU_CONTROL), want | TMU_CTRL_CORE_EN, rd(va, TMU_CONTROL1), rd(va, TMU_AVG_CON),
	    rd(va, TMU_COUNTER_VALUE0), rd(va, TMU_COUNTER_VALUE1), rd(va, TMU_TRIM0), v);
}

static const struct hakios_tmu_glue glue = {
	.name = "exynos9810",
	.nblocks = 2,
	.block_pa = { TMU_TOP_PA, TMU_SUB_PA },
	.block_name = { "TMU_TOP", "TMU_SUB" },
	.zones = tmu_zones,
	.efuse = TMU_EFUSE_VALUE,
	.clock_open = clock_open,
	.start_block = start_block,
	.power_off = hakios_exynos_power_off,
};

const struct hakios_tmu_glue *
hakios_tmu_glue_exynos9810(void)
{
	return &glue;
}
