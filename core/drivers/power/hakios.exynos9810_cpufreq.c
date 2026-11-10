/*
 * The Exynos 9810's side of the CPU frequency governor (hakios.exynos_cpufreq.c, whose header describes all of it;
 * cpufreq_soc=exynos9810, the default): the Galaxy S9's two clusters, their levels and Samsung's limits, and a rate set
 * by a DVFS request to the APM (domain 2 cpucl0, 3 cpucl1).
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/param.h>

#include "hakios.cpufreq_glue.h"

#define ACPM_DOMAIN(d)  (2 + (d))       /* cpucl0, cpucl1 */
#define NO_TEMP_BIG_MAX 1794000         /* without temperatures, the M3s no faster than this */

static const uint32_t lit_dt[] = {
	1950000, 1794000, 1690000, 1456000, 1248000, 1053000, 949000, 832000, 715000, 598000, 455000,
};
static const uint32_t big_dt[] = {
	2886000, 2860000, 2704000, 2652000, 2496000, 2314000, 2106000, 2002000, 1924000, 1794000, 1690000, 1586000, 1469000,
	1261000, 1170000, 1066000, 962000, 858000, 741000, 650000, 598000,
};

static uint32_t
table(unsigned domain, uint32_t *khz, uint32_t *uv, uint32_t most, int (*say)(const char *, ...))
{
	return hakios_acpm_dvfs_table(ACPM_DOMAIN(domain), khz, uv, most, say);
}

/*
 * One DVFS request, as hakios_acpm_dvfs_set() sends it (domain, kHz, FREQ_REQ 0, 0), with the APM's answer kept: its four
 * words come back in place of the request's.
 */
static int
set(unsigned domain, uint32_t khz, uint32_t answer[4])
{
	answer[0] = ACPM_DOMAIN(domain);
	answer[1] = khz;
	answer[2] = 0;
	answer[3] = 0;
	return hakios_acpm_request(HAKIOS_ACPM_CH_DVFS, answer);
}

/* FREQ_GET; the S9's APM answers 0 for both CPU domains */
static int
get(unsigned domain, uint32_t *khz)
{
	return hakios_acpm_dvfs_get(ACPM_DOMAIN(domain), khz);
}

/* the M3s' limit for this many online (exynos-hotplug_governor.c get_hpgov_maxfreq with the device tree's values) */
static uint32_t
big_limit(uint32_t online, bool temperatures)
{
	uint32_t limit = online <= 2 ? 2314000 : 1794000;

	return temperatures ? limit : MIN(limit, NO_TEMP_BIG_MAX);
}

static const struct hakios_cpufreq_glue glue = {
	.name = "exynos9810",
	.setter = "the APM",
	.init = hakios_acpm_init,
	.domain = {
		{ .affinity = 0, .control_mc = 76000, .min = 455000, .max = 1950000, .levels = lit_dt,
		  .n_levels = sizeof(lit_dt) / sizeof(lit_dt[0]), .max_default = 1794000, .floor_default = 1053000,
		  .touch_khz = 1690000 },
		{ .affinity = 1, .control_mc = 83000, .min = 650000, .max = 2314000, .levels = big_dt,
		  .n_levels = sizeof(big_dt) / sizeof(big_dt[0]), .max_default = 1794000, .floor_default = 650000,
		  .touch_khz = 1469000, .frame_khz = 1261000 },
	},
	.table = table,
	.set = set,
	.get = get,
	.big_limit = big_limit,
	.big_limit_text = "; and by M3s online: 1-2 2314000, 3-4 1794000; 1794000 without temperatures",
};

const struct hakios_cpufreq_glue *
hakios_cpufreq_glue_exynos9810(void)
{
	return &glue;
}
