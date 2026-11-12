/*
 * The Exynos 8890's side of the TMU driver (drivers/power/hakios.exynos_tmu.c; tmu_soc=exynos8890): the Note FE's four
 * blocks, one sensor each, and the TMU's bus clock. Read only: no block is started and the phone is not powered off from
 * here (tmu_enable and tmu_off are not offered).
 *
 * The blocks, from the phone's own device tree (tmuctrl@10060000 and on, "samsung,exynos8890-tmu") in the order of
 * Samsung's exynos8890_default_tmu_data (drivers/thermal/samsung/exynos_tmu_data.c:773-881, LineageOS
 * android_kernel_samsung_universal8890 lineage-18.1): MNGS, APOLLO, GPU, ISP. Each is the 9810's block with one sensor: the
 * code in CURRENT_TEMP [8:0] (0x40), TRIMINFO [8:0] the code at 25 C and [17:9] at 85 C, bit 23 two-point, efuse_value 50;
 * so the shared header's sensor 0 reads it. Samsung takes the GPU's and the ISP's reading only while STATUS bit 12 (valid)
 * is set (exynos_tmu_read, exynos_tmu.c:609-620).
 *
 * Where Android starts to cool, from the tree: the big cores clipped to 1872000 kHz at 76 C, 1352000 at 81, 728000 at 86,
 * taken away at 96; the little ones from 76; the GPU from 76 (600 MHz; 546 at 86, 419 at 91, 260 at 96); the ISP from 96.
 * The hardware trip is 115 C (trigger_levels_7), the ISP's last level 110.
 *
 * The clock. Samsung's driver takes none; the blocks' APB clock is CMU_PERIS's PCLK_SFR_APBIF_TMU, bit 12 of
 * CG_CTRL_VAL_ACLK_PERIS (0x10040000 +0x800; pwrcal/S5E8890/S5E8890-cmu.c:1763, S5E8890-cmusfr.h:1597). The blocks are
 * read only while that bit is set, as S-Boot leaves it.
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>

#include "hakios.exynos_tmu.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define CMU_PERIS_PA            0x10040000u
#define CG_VAL_ACLK_PERIS       0x0800u
#define CG_MAN_ACLK_PERIS       0x1800u
#define CG_STAT_ACLK_PERIS_0    0x1C00u
#define QSTATE_APBIF_TMU        0x241Cu
#define QSTATE_TMU              0x2474u
#define PCLK_APBIF_TMU          (1u << 12)
#define STATUS_VALID            (1u << 12)

static const struct tmu_zone zones[TMU_NZONES] = {
	{ "big", 0, 0x01, 76000, 115000, 0 },
	{ "little", 1, 0x01, 76000, 115000, 0 },
	{ "g3d", 2, 0x01, 76000, 115000, STATUS_VALID },
	{ "isp", 3, 0x01, 96000, 110000, STATUS_VALID },
};

static bool
clock_open(int (*say)(const char *, ...), __unused bool ungate)
{
	volatile uint32_t *cmu = (volatile uint32_t *)ml_io_map(CMU_PERIS_PA, 0x3000);

	if (cmu == NULL) {
		say("tmu: could not map CMU_PERIS\n");
		return false;
	}
	uint32_t val = cmu[CG_VAL_ACLK_PERIS / 4];

	say("tmu: 1: CMU_PERIS %#x: CG_CTRL_VAL_ACLK_PERIS %08x (PCLK_SFR_APBIF_TMU, bit 12: %u), MAN %08x, STAT %08x %08x, "
	    "QSTATE APBIF_TMU %08x, TMU %08x\n", CMU_PERIS_PA, val, val & PCLK_APBIF_TMU ? 1 : 0, cmu[CG_MAN_ACLK_PERIS / 4],
	    cmu[CG_STAT_ACLK_PERIS_0 / 4], cmu[CG_STAT_ACLK_PERIS_0 / 4 + 1], cmu[QSTATE_APBIF_TMU / 4], cmu[QSTATE_TMU / 4]);
	return (val & PCLK_APBIF_TMU) != 0;
}

static const struct hakios_tmu_glue glue = {
	.name = "exynos8890",
	.nblocks = 4,
	.block_pa = { 0x10060000u, 0x10064000u, 0x10068000u, 0x1006C000u },
	.block_name = { "TMU_MNGS", "TMU_APOLLO", "TMU_GPU", "TMU_ISP" },
	.zones = zones,
	.efuse = 50,
	.clock_open = clock_open,
};

const struct hakios_tmu_glue *
hakios_tmu_glue_exynos8890(void)
{
	return &glue;
}
