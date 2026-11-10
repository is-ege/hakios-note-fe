/*
 * What the Mali driver (drivers/gpu/hakios.mali.c: the GPU's own registers, its page tables, its jobs, HakiosMali)
 * needs from the SoC its GPU sits in: where the GPU is, and how its supply, clock and power domain are brought up and
 * put back. One per SoC, chosen by mali_soc=: the Exynos 9810's is drivers/gpu/hakios.exynos9810_g3d.c, the Exynos
 * 8890's devices/gracerlte/soc/drivers/core/hakios.exynos8890_g3d.c.
 */
#ifndef _HAKIOS_MALI_GLUE_H_
#define _HAKIOS_MALI_GLUE_H_

#include <stdbool.h>
#include <stdint.h>

struct hakios_mali_glue {
	const char      *name;
	uint64_t        gpu_pa;         /* the GPU's registers, 0x5000 bytes */
	/* step 1: what the boot left, read only (not the GPU itself); false: the steps stop */
	bool            (*look)(void);
	/* step 2: the supply, the clock and the domain, until the GPU can be read; false: it cannot */
	bool            (*power)(void);
	/* step 3: what a power-on of step 2's own needs after it; false: the GPU is not to be read */
	bool            (*after_power)(void);
	/* the steps ended short (the GPU already reset if it answered): what steps 2 and 3 changed, put back */
	void            (*undo)(void);
	/* the clock's rates, lowest first, for mali_gov; nrates 0: the boot's clock only, and the rest may be NULL */
	unsigned        nrates;
	uint32_t        (*rate_mhz)(unsigned level);
	bool            (*set_rate)(unsigned level);    /* says only a failure */
	unsigned        (*rate_now)(void);              /* the level running; nrates when none, or switching is off */
	uint32_t        (*mhz_now)(void);               /* the GPU's clock as the registers say */
	int32_t         (*g3d_mc)(void);                /* the G3D's temperature, millidegrees; NULL: no sensor read */
};

/* NULL from drivers/core/hakios.stubs.c when that SoC's file is left out */
const struct hakios_mali_glue *hakios_mali_glue_exynos9810(void);
const struct hakios_mali_glue *hakios_mali_glue_exynos8890(void);

#endif /* _HAKIOS_MALI_GLUE_H_ */
