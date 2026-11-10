/*
 * What the CPU frequency governor (drivers/power/hakios.exynos_cpufreq.c: the load windows, the boosts, the thermal
 * ceilings) needs from the SoC: each cluster's levels and Samsung's limits for them, and how a rate is set. One per SoC,
 * chosen by cpufreq_soc=: the Exynos 9810's is drivers/power/hakios.exynos9810_cpufreq.c (through the APM), the Exynos
 * 8890's devices/gracerlte/soc/drivers/core/hakios.exynos8890_cpu.c (the CMU and the PMIC themselves).
 */
#ifndef _HAKIOS_CPUFREQ_GLUE_H_
#define _HAKIOS_CPUFREQ_GLUE_H_

#include <stdbool.h>
#include <stdint.h>

struct hakios_cpufreq_glue {
	const char      *name;
	const char      *setter;                /* who sets a rate, for the log: "the APM" */
	/* what a rate change needs first; false (it says why): the governor does not start */
	bool            (*init)(int (*say)(const char *, ...));
	struct hakios_cpufreq_domain {          /* [0] the little cluster, [1] the big one; max 0: not governed */
		uint32_t        affinity;       /* MPIDR [15:8] of its cores */
		int32_t         control_mc;     /* where Samsung starts to cool it */
		uint32_t        min, max;       /* Samsung's limits, kHz */
		const uint32_t  *levels;        /* kHz, the fastest first, when `table` gives none */
		uint32_t        n_levels;
		uint32_t        max_default, floor_default;     /* the ceiling and floor without boot-args */
		uint32_t        touch_khz, frame_khz;           /* the boosts' floors without boot-args (0: none) */
		/* Samsung's ceilings by temperature, the coolest first; none: a level a second from control_mc */
		const struct hakios_cpufreq_clip { int32_t mc; uint32_t khz; } *clips;
		uint32_t        n_clips;
	} domain[2];
	/* the SoC's own table of a domain's levels in place of `levels`; NULL, or 0 levels: `levels` stand */
	uint32_t        (*table)(unsigned domain, uint32_t *khz, uint32_t *uv, uint32_t most, int (*say)(const char *, ...));
	/* a rate asked for; 0, or an errno. answer: four words for the log */
	int             (*set)(unsigned domain, uint32_t khz, uint32_t answer[4]);
	/* the rate now; 0 kHz: not said, the rate asked for stands */
	int             (*get)(unsigned domain, uint32_t *khz);
	/* the big cluster's limit under its ceiling: for this many of its cores online, with or without temperatures */
	uint32_t        (*big_limit)(uint32_t online, bool temperatures);
	const char      *big_limit_text;        /* that rule, for the log */
};

/* NULL from drivers/core/hakios.stubs.c when that SoC's file is left out */
const struct hakios_cpufreq_glue *hakios_cpufreq_glue_exynos9810(void);
const struct hakios_cpufreq_glue *hakios_cpufreq_glue_exynos8890(void);

#endif /* _HAKIOS_CPUFREQ_GLUE_H_ */
