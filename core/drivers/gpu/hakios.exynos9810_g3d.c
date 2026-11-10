/*
 * The Exynos 9810's side of the Galaxy S9's Mali-G72 (hakios.mali_glue.h, mali_soc=exynos9810, the default): the G3D's
 * supply, clock and power domain, brought up for the Mali driver (drivers/gpu/hakios.mali.c) and put back if its steps
 * end short. Moved out of hakios.mali.c unchanged when the Galaxy Note FE's Exynos 8890 got a glue of its own.
 *
 *   supply        the S2MPS18's BUCK6, vdd_g3d: B6CTRL (0x2A) [7:6] its on mode -- 10 on this phone, the G3D power
 *                 pin's (the device tree's g3d_en): the PMU's power-on of the domain turns it on -- and B6OUT (0x2B)
 *                 its voltage, 300 mV + 6.25 mV a step, 950 mV (0x68) the regulator's most. Reached only through ACPM
 *                 (drivers/core/hakios.exynos_acpm.c). S-BOOT LEAVES IT AT 644 mV WITH THE GPU AT 260 MHz, and on the
 *                 S9+ that is a combination the GPU does not work on: panfrost had DATA_INVALID_FAULTs every few seconds,
 *                 and two kernels whose panfrost reset and read the GPU before the rail was raised never came back (the
 *                 SoC hung). Step 2 raises B6OUT to mali_uv (950 mV by default), selected from the board's ASV table,
 *                 BEFORE THE GPU'S REGISTERS ARE FIRST TOUCHED, which is step 4
 *   power         the domain pd-embedded_g3d of the PMU (0x14060000): its configuration at 0x14064040, [3:0] = 0xF asks
 *                 for it on; its status at 0x14064044, [3:0] = 0xF when it is. A read of the GPU while it is off is a
 *                 bus hang, not an error
 *   clock         PLL_G3D of CMU_G3D (0x17400000, outside the domain: it keeps its settings when the domain is off).
 *                 PLL_CON0 at +0x140: S [2:0], P [13:8], M [25:16], MUX_SEL bit 4 (the G3D's clock from the PLL, not
 *                 the 26 MHz oscillator), MUX_BUSY bit 7, STABLE bit 29, ENABLE bit 31; 26 MHz x M / (P x 2^S). PLL_CON2
 *                 at +0x148 must hold 0x30000003 while the PLL locks (S-Boot's 0x10000001 keeps it from ever reaching
 *                 STABLE); PLL_LOCKTIME at +0x0, P x 150. The PLL is a PLL_1018X, and Samsung's CAL gives that kind
 *                 its limits (pll_spec.c): P 1-63, M 64-1023, S 0-6, 26 MHz / P from 2 to 8 MHz, the VCO (26 MHz x M
 *                 / P) from 600 to 1200 MHz. The rates this driver sets are three of the stock GPU table's six (the
 *                 phone's gpu_dvfs_table: 572, 546, 455, 338, 299, 260), the three the S9+ port runs: 260 MHz (M 160,
 *                 P 4, S 2: S-Boot's own, a VCO of 1040), 455 (M 140, P 4, S 1: 910) and 572 (M 176, P 4, S 1: 1144).
 *                 The S9+ port's own M/P/S for the two (455, 26, 0 and 286, 13, 0) put the VCO at 455 and 572 MHz,
 *                 below the PLL's 600, and 455's reference at 1 MHz, below its 2: the same rates, in the PLL's range
 *                 here. (The kernel's own PLL_G3D table, 860, 320 and 650 MHz, is in the range as well; the APM
 *                 firmware sets the GPU's rates under Android, and its table is not in the source.)
 *                 The G3D's own mux at +0x100 (bit 4: CMU's clock), and its bus dividers: MUX_CLK_G3D_BUSD +0x1000,
 *                 DIV_CLK_G3D_BUSD +0x1800, DIV_CLK_G3D_BUSP +0x1804 (bit 16 busy)
 *
 * THE SEQUENCE, as the mainline Linux port to the S9+ runs it (panfrost from S-Boot's state, the same SoC and almost the
 * same board), and Samsung's CAL before it:
 *   the rail      B6OUT = mali_uv through ACPM, read back, 200 us; before anything else
 *   a rate        PLL_CON2 = 0x30000003; PLL_LOCKTIME = P x 150; PLL_CON0 MUX_SEL cleared and MUX_BUSY waited for (the
 *                 G3D on the oscillator first, as a step of its own: cleared together with ENABLE, the G3D is left with
 *                 no clock at all for a moment); ENABLE and the dividers cleared, then M/P/S, then ENABLE; STABLE (20
 *                 ms); MUX_SEL set and MUX_BUSY waited for again. Nothing, if the PLL already runs at the rate wanted
 *   power on      (only if the domain is off) PLL_CON2 = 0x30000003, PLL_CON0 |= MUX_SEL, +0x100 |= bit 4, the PMU's
 *                 configuration [3:0] = 0xF, its status waited for; then SMC 0x82000410 (1, 0x17410204, 0), the DTZPC
 *                 restore -- without it the first access to the GPU is refused by the bus, an SError -- 0 back for
 *                 success; then the bus dividers: +0x1000 = 0, +0x1800 = 0 (BUSD = the PLL's), +0x1804 = 3 (BUSP = BUSD
 *                 / 4), and the busy bit waited for. A domain the boot left on is used as it is: no SMC, no dividers
 *   power off     the five low-power settings of Samsung's pmucal_lpm_init (masked: PMU +0x4690 [1:0] = 2, +0x4680 bit
 *                 1 = 0, +0x4684 [11:0] = 0x363, +0x15A8 bit 0 = 1, +0x15AC bit 0 = 1) -- without them the S9+'s domain
 *                 stopped half off (STATUS 0x002F000E) and the next access was an SError --; SMC 0x82000410 (0,
 *                 0x17410204, 0), the DTZPC SAVE; the configuration [3:0] = 0 and the status waited for; then +0x100 and
 *                 PLL_CON0 MUX_SEL cleared and PLL_CON2 = 0x10000001. A domain that does not go off is switched on
 *                 again and its protection restored, and left on; one that never came on gets no save (its protection
 *                 controller is inside it), only the configuration [3:0] = 0
 * Under this port Carbon Hypervisor has EL2 and passes an SMC it does not know on to EL3.
 *
 * Sources. The S9+ port: github.com/SheroAbi/galaxy-s9plus-mainline-linux, src/soc/s9p-g3d.c (the rail, the rates, the
 * PLL's switch), src/soc/s9p-g3d-pd.c (the domain on and off, the SMCs, the dividers, the low-power settings),
 * src/soc/s9p-acpm.c (ACPM), src/soc/s9p-clkgate.c, src/mainline-dts/exynos9810-star2lte.dts (the GPU's, the domain's
 * and the clock's nodes) and its docs/04-drivers.md, 06-known-issues.md, 07-validation.md -- GPL-2.0 (the device tree
 * GPL-2.0 OR BSD-3-Clause): read for these facts, not one line copied. Samsung's GPL-2.0 kernel of the Exynos 9810
 * (github.com/ExyHyperBrick/android_kernel_samsung_exynos9810, branch lineage-24.0, forked to Scion-Project: the G3D's
 * CAL and power domain, cal-if.c, the S2MPS18's registers, the Mali's platform code) and Samsung's device tree (the
 * phone's own, and Silicium's starlte.dts), through a fact sheet. Linux panfrost (panfrost_gpu.c, panfrost_regs.h) for
 * what it does at a reset and at power-up.
 *
 * Boot-args:
 *   mali_mhz=MHZ     the GPU's clock: 260 (S-Boot's, the default), 455 or 572, the S9+ port's three rates
 *   mali_pll=0       PLL_G3D left as the boot left it (the step stops if it does not run)
 *   mali_acpm=0      the rail not raised: the GPU is touched at the boot's voltage, which hung the S9+ (a test only)
 *   mali_uv=UV       BUCK6 target in microvolts, 300000..950000 in 6250-uV steps (default 950000).
 *                    Select from this phone's stock ASV table; our SM-G965F's maximum is 750000.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.exynos_acpm.h>
#include <sys/systm.h>

#include "hakios.mali_glue.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern int32_t hakios_exynos_tmu_zone_mc(unsigned zone);       /* hakios.exynos_tmu.c; stubs.c */
extern void loader_log_line(const char *line);

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

static inline uint32_t
rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

/* Until (register & mask) == want, for at most `us` microseconds by the clock (delay() may sleep far longer than asked). */
static bool
wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while ((rd(base, at) & mask) != want) {
		if (mach_absolute_time() >= deadline) {
			return (rd(base, at) & mask) == want;
		}
	}
	return true;
}

/* `us` microseconds, by the clock */
static void
pause_us(uint32_t us)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &deadline);
	deadline += mach_absolute_time();
	while (mach_absolute_time() < deadline) {
	}
}

static vm_offset_t pmu, cmu_g3d;
static bool powered_here;               /* step 2 switched the domain on: off again if the steps end short */

#define STARLTE_SOC_BASE        0x10000000UL    /* where the loader's platform "starlte" says the SoC's registers begin */

#define PMU_PHYS                0x14060000UL    /* the PMU */
#define PMU_SIZE                0x5000
#define PMU_G3D_CONFIGURATION   0x4040          /* 0x14064040: [3:0] = 0xF asks for pd-embedded_g3d on, 0 off */
#define PMU_G3D_STATUS          0x4044          /* 0x14064044: [3:0] = 0xF when it is on */
#define G3D_ON                  0xfu
#define G3D_WAIT_US             100000u         /* (the S9+ port waits 10 ms) */
/* the G3D's low-power settings of Samsung's pmucal_lpm_init, made before the domain is switched off: offset, mask, value */
static const struct { uint32_t at, mask, value; const char *name; } g3d_lpm[] = {
	{ 0x4690, 0x3u, 0x2u, "MEMORY_EMBEDDED_G3D_OPTION" },
	{ 0x4680, 0x2u, 0x0u, "PWR_EMBEDDED_G3D_OPTION" },
	{ 0x4684, 0xfffu, 0x363u, "PWR_EMBEDDED_G3D_DURATION" },
	{ 0x15a8, 0x1u, 0x1u, "SCI_CRPPORTPWRDN" },
	{ 0x15ac, 0x1u, 0x1u, "PWR_EMBEDDED_G3D" },
};

#define CMU_G3D_PHYS            0x17400000UL
#define CMU_G3D_SIZE            0x2000          /* the bus dividers are at +0x1800 */
#define PLL_LOCKTIME            0x000
#define MUX_G3D_USER            0x100           /* PLL_CON0_MUX_CLKCMU_EMBEDDED_G3D_USER: MUX_SEL set on, cleared off */
#define PLL_CON0                0x140           /* PLL_G3D */
#define PLL_CON2                0x148           /* PLL_CON2_LOCK while the PLL locks and the domain is on, _OFF after */
#define MUX_CLK_G3D_BUSD        0x1000
#define DIV_CLK_G3D_BUSD        0x1800
#define DIV_CLK_G3D_BUSP        0x1804
#define CLK_CON_BUSY            (1u << 16)
#define MUX_SEL                 (1u << 4)       /* PLL_CON0: the PLL's output, not the oscillator; +0x100: CMU's clock */
#define MUX_BUSY                (1u << 7)       /* PLL_CON0: the mux still moving */
#define PLL_ENABLE              (1u << 31)
#define PLL_STABLE              (1u << 29)
#define PLL_PMS                 ((0x3ffu << 16) | (0x3fu << 8) | 7u)
#define PLL_CON2_LOCK           0x30000003u
#define PLL_CON2_OFF            0x10000001u
#define PLL_FIN_KHZ             26000ull
#define PLL_LOCK_FACTOR         150u            /* PLL_LOCKTIME = P x 150 */
#define PLL_STABLE_US           20000u
#define MUX_BUSY_US             1000u
/*
 * the rates (mali_mhz): the S9+ port's three, M, P, S on the 26 MHz oscillator, each inside PLL_1018X's limits (26 / P
 * from 2 to 8 MHz, 26 x M / P from 600 to 1200 MHz: Samsung's pll_spec.c)
 */
static const struct g3d_rate { uint32_t mhz, m, p, s; } g3d_rates[] = {
	{ 260, 160, 4, 2 },                     /* S-Boot's own: the VCO at 1040 MHz */
	{ 455, 140, 4, 1 },                     /* 910 */
	{ 572, 176, 4, 1 },                     /* 1144 */
};
static uint32_t mali_mhz = 260;
static bool pll_quiet;                          /* the governor's switches: a line only when one fails */
static uint32_t mali_pll = 1;                   /* mali_pll=0: PLL_G3D as the boot left it */

#define DTZPC_SMC               0x82000410u     /* Samsung's SiP call, SMC_CMD_PREPARE_PD_ONOFF */
#define DTZPC_SAVE              0u              /* before the domain's power goes */
#define DTZPC_RESTORE           1u              /* after it has come */
#define DTZPC_G3D               0x17410204u


/*
 * The supply, through ACPM (drivers/core/hakios.exynos_acpm.c): the S2MPS18's BUCK6 (vdd_g3d). Only B6OUT is ever
 * written, never above 0x68 (950 mV, the regulator's most in the phone's device tree), and read back; B6CTRL is read,
 * for the log. Put back as found when this driver switches the domain off again. Boot-arg mali_acpm=0: none of it.
 */
#define B6CTRL                  0x2a            /* [7:6]: 11 always on, 10 the G3D power pin's (g3d_en), 00 off */
#define B6OUT                   0x2b            /* 300 mV + 6.25 mV a step */
#define B6OUT_950MV             0x68
#define B6OUT_MOST              0x68
#define RAIL_SETTLE_US          200
#define RAIL_TRIES              10              /* 100 ms apart: the APM may not answer at once */
static uint32_t mali_acpm = 1;
static uint32_t mali_uv = 950000;
static uint8_t b6out_target = B6OUT_950MV;
static bool acpm_ok, rail_changed, b6out_known;  /* b6out_known: B6OUT was read before it was written */
static uint8_t b6out_found;

/* PLL_G3D as it is, said; its PLL_CON0 back. */
static uint32_t
pll_say(const char *when)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0);
	uint32_t m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;
	uint32_t khz = p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s)) : 0;

	say("mali: PLL_G3D %s: PLL_CON0 %#x: M %u, P %u, S %u, %u.%03u MHz from 26 MHz; %s, %s; the G3D on %s%s; "
	    "PLL_CON2 %#x\n", when, con0, m, p, s, khz / 1000, khz % 1000, (con0 & PLL_ENABLE) ? "enabled" :
	    "not enabled", (con0 & PLL_STABLE) ? "stable" : "NOT STABLE", (con0 & MUX_SEL) ? "the PLL" : "the oscillator",
	    (con0 & MUX_BUSY) ? " (the mux still moving)" : "", rd(cmu_g3d, PLL_CON2));
	return con0;
}

static bool
pll_is(const struct g3d_rate *r)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0);

	return ((con0 >> 16) & 0x3ff) == r->m && ((con0 >> 8) & 0x3f) == r->p && (con0 & 7) == r->s &&
	       (con0 & (PLL_ENABLE | PLL_STABLE | MUX_SEL)) == (PLL_ENABLE | PLL_STABLE | MUX_SEL);
}

/*
 * PLL_G3D to rate `r`, as the S9+ port switches it (s9p-g3d.c): PLL_CON2 = 0x30000003 (with S-Boot's 0x10000001 the PLL
 * never locks), the lock time; the G3D over to the oscillator and the mux waited for, as a step of its own; the PLL
 * stopped and its dividers cleared, then M/P/S, then ENABLE; STABLE waited for; the G3D back on the PLL and the mux
 * waited for. On a failure the PLL as it was found, on the oscillator if it does not lock again.
 */
static bool
pll_switch(const struct g3d_rate *r)
{
	uint32_t old_con0 = rd(cmu_g3d, PLL_CON0), old_locktime = rd(cmu_g3d, PLL_LOCKTIME), v;
	const char *failed;

	if (!pll_quiet) {
		say("mali: PLL_G3D to %u MHz (M %u, P %u, S %u): PLL_CON2 = %#x, PLL_LOCKTIME = %u, the G3D on the "
		    "oscillator, the PLL relocked, the G3D back on it\n", r->mhz, r->m, r->p, r->s, PLL_CON2_LOCK,
		    r->p * PLL_LOCK_FACTOR);
	}
	wr(cmu_g3d, PLL_CON2, PLL_CON2_LOCK);
	wr(cmu_g3d, PLL_LOCKTIME, r->p * PLL_LOCK_FACTOR);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	if (!wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US)) {
		failed = "the mux did not move to the oscillator";
		goto restore;
	}
	v = rd(cmu_g3d, PLL_CON0) & ~(PLL_ENABLE | PLL_PMS);
	wr(cmu_g3d, PLL_CON0, v);
	v |= r->m << 16 | r->p << 8 | r->s;
	wr(cmu_g3d, PLL_CON0, v);
	wr(cmu_g3d, PLL_CON0, v | PLL_ENABLE);
	if (!wait_for(cmu_g3d, PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_US)) {
		failed = "the PLL did not lock";
		goto restore;
	}
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	if (!wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US) || !pll_is(r)) {
		failed = "the mux did not move back to the PLL";
		goto restore;
	}
	if (!pll_quiet) {
		(void)pll_say("switched");
	}
	return true;

restore:
	(void)pll_say(failed);
	say("mali: %s: PLL_G3D put back as it was found (PLL_CON0 %#x)\n", failed, old_con0);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	if (wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US)) {
		wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~PLL_ENABLE);
		wr(cmu_g3d, PLL_LOCKTIME, old_locktime);
		wr(cmu_g3d, PLL_CON0, old_con0 & ~MUX_SEL);
		if (!(old_con0 & PLL_ENABLE) || wait_for(cmu_g3d, PLL_CON0, PLL_STABLE, PLL_STABLE, PLL_STABLE_US)) {
			wr(cmu_g3d, PLL_CON0, old_con0);
			(void)wait_for(cmu_g3d, PLL_CON0, MUX_BUSY, 0, MUX_BUSY_US);
		}
	}
	(void)pll_say("put back");
	return false;
}

/* The rail's voltage, said; its B6OUT in `out`. False if ACPM did not answer. */
static bool
rail_read(const char *when, uint8_t *out)
{
	uint8_t ctrl = 0, vsel = 0;
	int error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6CTRL, &ctrl);

	if (error == 0) {
		error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &vsel);
	}
	if (error != 0) {
		say("mali: ACPM: BUCK6 (vdd_g3d) %s: NOT READ, error %d\n", when, error);
		return false;
	}
	say("mali: ACPM: BUCK6 (vdd_g3d) %s: B6CTRL %#x ([7:6] = %u: %s), B6OUT %#x = %u mV\n", when, ctrl,
	    (ctrl >> 6) & 3, (ctrl >> 6) == 3 ? "always on" : (ctrl >> 6) == 2 ? "the G3D power pin's" : (ctrl >> 6) == 1 ?
	    "on in standby" : "OFF", vsel, 300 + vsel * 25 / 4);
	*out = vsel;
	return true;
}

/*
 * B6OUT = `vsel`, and read back. The only PMIC register this driver writes, never above 950 mV: nothing else, ever.
 * Tried RAIL_TRIES times, 100 ms apart (the S9+ port found the APM not answering yet early in the boot).
 */
static bool
rail_set(uint8_t vsel)
{
	uint8_t now = 0;

	if (vsel > B6OUT_MOST) {
		return false;
	}
	for (int attempt = 1; attempt <= RAIL_TRIES; attempt++) {
		int error = hakios_acpm_pmic_write(HAKIOS_ACPM_PMIC_PMIC, B6OUT, vsel);

		if (error == 0) {
			error = hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_PMIC, B6OUT, &now);
		}
		if (error == 0 && now == vsel) {
			pause_us(RAIL_SETTLE_US);
			return true;
		}
		say("mali: ACPM: B6OUT = %#x, attempt %d of %d: %s %#x\n", vsel, attempt, RAIL_TRIES,
		    error ? "error" : "it reads", error ? (unsigned)error : now);
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	return false;
}

/* One SMC to EL3 (through Carbon at EL2): x0 back. */
static uint64_t
smc(uint64_t function, uint64_t first, uint64_t second, uint64_t third)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = first;
	register uint64_t x2 __asm__("x2") = second;
	register uint64_t x3 __asm__("x3") = third;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
            "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * The domain off again (see the header's "power off"), the GPU already reset by the Mali driver. A domain that does not go off is switched on again with its protection restored, and
 * left on: it is not one the GPU can be read in, half off. The rail is put back as found once the domain is off.
 */
static void
g3d_off(void)
{
	uint64_t answer;

	if ((rd(pmu, PMU_G3D_STATUS) & G3D_ON) != G3D_ON) {
		/* (never on: its protection controller is in it, and EL3 is not to save it from a block without power) */
		say("mali: the G3D domain is not on (status %#x): no DTZPC save; its configuration [3:0] = 0\n",
		    rd(pmu, PMU_G3D_STATUS));
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
		(void)wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_US);
		goto clocks;
	}
	say("mali: switching the G3D domain off again, as it was found: first Samsung's low-power settings for it\n");
	for (unsigned i = 0; i < sizeof(g3d_lpm) / sizeof(g3d_lpm[0]); i++) {
		uint32_t was = rd(pmu, g3d_lpm[i].at);

		wr(pmu, g3d_lpm[i].at, (was & ~g3d_lpm[i].mask) | g3d_lpm[i].value);
		say("mali: PMU +%#x %s: %#x -> %#x\n", g3d_lpm[i].at, g3d_lpm[i].name, was, rd(pmu, g3d_lpm[i].at));
	}
	answer = smc(DTZPC_SMC, DTZPC_SAVE, DTZPC_G3D, 0);
	say("mali: SMC %#x (%u, %#x): the DTZPC save, returned %#llx\n", DTZPC_SMC, DTZPC_SAVE, DTZPC_G3D, answer);
	if ((uint32_t)answer != 0) {
		say("mali: the protection was not saved: the domain is left ON\n");
		powered_here = false;
		return;
	}
	wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, 0, G3D_WAIT_US)) {
		say("mali: the domain did not go off: status %#x after %u ms; switching it on again\n", rd(pmu, PMU_G3D_STATUS),
		    G3D_WAIT_US / 1000);
		wr(pmu, PMU_G3D_CONFIGURATION, rd(pmu, PMU_G3D_CONFIGURATION) | G3D_ON);
		if (wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
			answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D, 0);
			say("mali: on again; the DTZPC restore returned %#llx; the domain is left ON\n", answer);
		} else {
			say("mali: NOT ON AGAIN EITHER: status %#x; nothing of the G3D is to be touched\n", rd(pmu, PMU_G3D_STATUS));
		}
		powered_here = false;
		return;
	}
clocks:
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) & ~MUX_SEL);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) & ~MUX_SEL);
	wr(cmu_g3d, PLL_CON2, PLL_CON2_OFF);
	say("mali: the G3D domain: configuration %#x, status %#x; CMU_G3D +0x100 %#x, PLL_CON0 %#x, PLL_CON2 %#x\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), rd(pmu, PMU_G3D_STATUS), rd(cmu_g3d, MUX_G3D_USER), rd(cmu_g3d, PLL_CON0),
	    rd(cmu_g3d, PLL_CON2));
	if (rail_changed) {
		say("mali: BUCK6 back as it was found: B6OUT %#x (%u mV)\n", b6out_found, 300 + b6out_found * 25 / 4);
		if (b6out_found <= B6OUT_MOST && rail_set(b6out_found)) {
			rail_changed = false;
		} else {
			say("mali: B6OUT not put back (it reads otherwise, or ACPM did not answer)\n");
		}
	}
	powered_here = false;
}

/* 1: what the boot has left, read only; not the GPU itself. */
static bool
look(void)
{
	uint32_t status;

	say("mali: 1: mapping the PMU (%#lx) and CMU_G3D (%#lx); nothing is written in this step\n", PMU_PHYS, CMU_G3D_PHYS);
	pmu = ml_io_map(PMU_PHYS, PMU_SIZE);
	cmu_g3d = ml_io_map(CMU_G3D_PHYS, CMU_G3D_SIZE);
	if (pmu == 0 || cmu_g3d == 0) {
		say("mali: 1: cannot map them\n");
		return false;
	}
	say("mali: 1: reading the PMU's G3D configuration (%#lx) and status (%#lx)\n", PMU_PHYS + PMU_G3D_CONFIGURATION,
	    PMU_PHYS + PMU_G3D_STATUS);
	status = rd(pmu, PMU_G3D_STATUS);
	say("mali: 1: G3D configuration %#x, status %#x: the domain is %s\n", rd(pmu, PMU_G3D_CONFIGURATION), status,
	    (status & G3D_ON) == G3D_ON ? "on" : (status & G3D_ON) == 0 ? "off" : "NEITHER ON NOR OFF");
	say("mali: 1: reading CMU_G3D (outside the domain: it answers with the domain off)\n");
	(void)pll_say("as found");
	say("mali: 1: CMU_G3D +0x100 %#x (the G3D on %s), MUX_CLK_G3D_BUSD %#x, DIV_CLK_G3D_BUSD %#x, DIV_CLK_G3D_BUSP %#x\n",
	    rd(cmu_g3d, MUX_G3D_USER), (rd(cmu_g3d, MUX_G3D_USER) & MUX_SEL) ? "CMU's clock" : "the oscillator",
	    rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	say("mali: 1: the GPU's own registers are not read before step 2 has raised its supply (S-Boot's 260 MHz at 644 mV "
	    "hung the S9+'s SoC when its GPU was read and reset)\n");
	if (mali_acpm && (acpm_ok = hakios_acpm_init(say))) {
		b6out_known = rail_read("as found", &b6out_found);
	} else if (mali_acpm) {
		say("mali: 1: no ACPM: the rail cannot be raised, and step 2 will stop there\n");
	}
	return true;
}

/* 2: the supply, the clock, power. */
static bool
power(void)
{
	const struct g3d_rate *rate = NULL;
	uint32_t status = rd(pmu, PMU_G3D_STATUS) & G3D_ON;

	if (status != G3D_ON && status != 0) {
		say("mali: 2: the domain is neither on nor off (status %#x): left alone\n", rd(pmu, PMU_G3D_STATUS));
		return false;
	}
	/* the rail first, before the clock moves and before anything of the GPU is touched */
	if (!mali_acpm) {
		say("mali: 2: mali_acpm=0: BUCK6 left as found -- the GPU will be touched at the boot's voltage\n");
	} else if (!acpm_ok) {
		say("mali: 2: no ACPM: BUCK6 cannot be raised; the GPU is not touched at the boot's voltage\n");
		return false;
	} else if (!b6out_known && !(b6out_known = rail_read("as found, read again", &b6out_found))) {
		say("mali: 2: B6OUT cannot be read: not written either (ACPM does not answer); the GPU is not touched at the "
		    "boot's voltage\n");
		return false;
	} else if (b6out_found > b6out_target) {
		say("mali: 2: the boot's BUCK6 is above the requested %u uV: no lowering under a running GPU, and no GPU access\n",
		    mali_uv);
		return false;
	} else if (b6out_found == b6out_target) {
		say("mali: 2: B6OUT is %#x (%u mV) already: not written\n", b6out_found, 300 + b6out_found * 25 / 4);
	} else {
		uint8_t now = 0;

		say("mali: 2: ACPM: B6OUT = %#x (%u uV, was %#x = %u mV), read back, then %u us\n", b6out_target, mali_uv,
		    b6out_found, 300 + b6out_found * 25 / 4, RAIL_SETTLE_US);
		rail_changed = true;
		if (!rail_set(b6out_target)) {
			say("mali: 2: BUCK6 did not reach the selected %u uV: the GPU is not touched\n", mali_uv);
			(void)rail_read("now", &now);
			return false;
		}
		(void)rail_read("now", &now);
	}
	/* the clock: PLL_G3D at the rate asked for */
	for (unsigned i = 0; i < sizeof(g3d_rates) / sizeof(g3d_rates[0]); i++) {
		if (g3d_rates[i].mhz == mali_mhz) {
			rate = &g3d_rates[i];
		}
	}
	if (rate == NULL) {
		say("mali: 2: mali_mhz=%u is not one of 260, 455, 572: 260\n", mali_mhz);
		rate = &g3d_rates[0];
	}
	if (pll_is(rate)) {
		say("mali: 2: PLL_G3D runs at %u MHz already, the G3D on it: not touched\n", rate->mhz);
	} else if (!mali_pll) {
		uint32_t con0 = pll_say("left as found (mali_pll=0)");

		if ((con0 & (PLL_ENABLE | PLL_STABLE)) != (PLL_ENABLE | PLL_STABLE)) {
			say("mali: 2: PLL_G3D does not run, and mali_pll=0 leaves it so: the GPU would have no clock\n");
			return false;
		}
	} else if (!pll_switch(rate)) {
		return false;
	}
	if (status == G3D_ON) {
		say("mali: 2: the domain is on already: no power-on; it is left on whatever happens\n");
		return true;
	}
	/* the power-on: PLL_CON2 and PLL_CON0's MUX_SEL as the switch left them (or the boot, with mali_pll=0) */
	say("mali: 2: the power-on: PLL_CON2 = %#x (was %#x), PLL_CON0 |= MUX_SEL, CMU_G3D +0x100 |= bit 4 (was %#x)\n",
	    PLL_CON2_LOCK, rd(cmu_g3d, PLL_CON2), rd(cmu_g3d, MUX_G3D_USER));
	powered_here = true;            /* from the first write on, the way back is g3d_off() */
	wr(cmu_g3d, PLL_CON2, PLL_CON2_LOCK);
	wr(cmu_g3d, PLL_CON0, rd(cmu_g3d, PLL_CON0) | MUX_SEL);
	wr(cmu_g3d, MUX_G3D_USER, rd(cmu_g3d, MUX_G3D_USER) | MUX_SEL);
	say("mali: 2: writing the PMU's G3D configuration [3:0] = 0xf (was %#x), then waiting up to %u ms for its status\n",
	    rd(pmu, PMU_G3D_CONFIGURATION), G3D_WAIT_US / 1000);
	wr(pmu, PMU_G3D_CONFIGURATION, (rd(pmu, PMU_G3D_CONFIGURATION) & ~G3D_ON) | G3D_ON);
	if (!wait_for(pmu, PMU_G3D_STATUS, G3D_ON, G3D_ON, G3D_WAIT_US)) {
		say("mali: 2: the domain did not come up: status %#x after %u ms\n", rd(pmu, PMU_G3D_STATUS), G3D_WAIT_US / 1000);
		return false;
	}
	say("mali: 2: the domain is on: status %#x\n", rd(pmu, PMU_G3D_STATUS));
	return true;
}

/* 3: after a power-on of ours, the G3D's TrustZone protection controller set up again by EL3, and the bus dividers. */
static bool
after_power(void)
{
	uint64_t answer;

	if (!powered_here) {
		say("mali: 3: the boot left the domain on: its protection and bus dividers stand (the S9+ port restores them only "
		    "after a power-on of its own); nothing done\n");
		return true;
	}
	say("mali: 3: about to make SMC %#x (%u, %#x, 0): the DTZPC restore, after the G3D's power-on; Carbon, at EL2, passes "
	    "it on to EL3\n", DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D);
	answer = smc(DTZPC_SMC, DTZPC_RESTORE, DTZPC_G3D, 0);
	say("mali: 3: SMC %#x returned %#llx\n", DTZPC_SMC, answer);
	if ((uint32_t)answer != 0) {
		say("mali: 3: not 0: %s; the GPU is not read (it would be an SError)\n", (uint32_t)answer == 0xffffffffu ?
		    "the call is not known to whoever answered it (SMCCC's NOT_SUPPORTED)" : "a refusal");
		return false;
	}
	say("mali: 3: the bus dividers: MUX_CLK_G3D_BUSD = 0, DIV_CLK_G3D_BUSD = 0 (the PLL's rate), DIV_CLK_G3D_BUSP = 3 (a "
	    "quarter of it); were %#x, %#x, %#x\n", rd(cmu_g3d, MUX_CLK_G3D_BUSD), rd(cmu_g3d, DIV_CLK_G3D_BUSD),
	    rd(cmu_g3d, DIV_CLK_G3D_BUSP));
	wr(cmu_g3d, MUX_CLK_G3D_BUSD, 0);
	wr(cmu_g3d, DIV_CLK_G3D_BUSD, 0);
	wr(cmu_g3d, DIV_CLK_G3D_BUSP, 3);
	if (!wait_for(cmu_g3d, DIV_CLK_G3D_BUSP, CLK_CON_BUSY, 0, MUX_BUSY_US)) {
		say("mali: 3: DIV_CLK_G3D_BUSP still busy: %#x\n", rd(cmu_g3d, DIV_CLK_G3D_BUSP));
		return false;
	}
	return true;
}

/* PLL_G3D's rate as its registers say, in MHz; 26 when the G3D is on the oscillator */
static uint32_t
pll_mhz(void)
{
	uint32_t con0 = rd(cmu_g3d, PLL_CON0), m = (con0 >> 16) & 0x3ff, p = (con0 >> 8) & 0x3f, s = con0 & 7;

	if (!(con0 & MUX_SEL)) {
		return (uint32_t)(PLL_FIN_KHZ / 1000);
	}
	return p != 0 ? (uint32_t)(PLL_FIN_KHZ * m / ((uint64_t)p << s) / 1000) : 0;
}

/*
 * The steps ended short (the GPU already reset): a domain this file switched on goes off again; a rail it raised under
 * a domain that is off goes back to what it was; a rail under a domain that is on stays (no lower voltage under it).
 */
static void
undo(void)
{
	if (powered_here) {
		g3d_off();
	} else if (rail_changed && (rd(pmu, PMU_G3D_STATUS) & G3D_ON) == 0) {
		say("mali: BUCK6 back as it was found: B6OUT %#x\n", b6out_found);
		if (rail_set(b6out_found)) {
			rail_changed = false;
		}
	} else if (rail_changed) {
		say("mali: the G3D domain is on: BUCK6 left at the selected %u uV under it\n", mali_uv);
	} else if (pmu != 0) {
		say("mali: the G3D domain left as found\n");
	}
}

static uint32_t
rate_mhz(unsigned level)
{
	return g3d_rates[level].mhz;
}

static bool
set_rate(unsigned level)
{
	bool switched;

	pll_quiet = true;
	switched = pll_switch(&g3d_rates[level]);
	pll_quiet = false;
	return switched;
}

/* which of the rates PLL_G3D runs at; none when the clock or the rail is not this file's to move */
static unsigned
rate_now(void)
{
	unsigned n = sizeof(g3d_rates) / sizeof(g3d_rates[0]);

	if (!mali_pll || !mali_acpm || !acpm_ok) {
		return n;
	}
	for (unsigned i = 0; i < n; i++) {
		if (pll_is(&g3d_rates[i])) {
			return i;
		}
	}
	return n;
}

/* the G3D's sensor, zone 2 of hakios.exynos_tmu.c */
static int32_t
g3d_mc(void)
{
	return hakios_exynos_tmu_zone_mc(2);
}

static const struct hakios_mali_glue glue = {
	.name = "exynos9810",
	.gpu_pa = 0x17500000,
	.look = look,
	.power = power,
	.after_power = after_power,
	.undo = undo,
	.nrates = sizeof(g3d_rates) / sizeof(g3d_rates[0]),
	.rate_mhz = rate_mhz,
	.set_rate = set_rate,
	.rate_now = rate_now,
	.mhz_now = pll_mhz,
	.g3d_mc = g3d_mc,
};

const struct hakios_mali_glue *
hakios_mali_glue_exynos9810(void)
{
	DTEntry node;
	void const *prop;
	unsigned int size;

	(void)PE_parse_boot_argn("mali_pll", &mali_pll, sizeof(mali_pll));
	(void)PE_parse_boot_argn("mali_mhz", &mali_mhz, sizeof(mali_mhz));
	(void)PE_parse_boot_argn("mali_acpm", &mali_acpm, sizeof(mali_acpm));
	(void)PE_parse_boot_argn("mali_uv", &mali_uv, sizeof(mali_uv));
	if (mali_uv < 300000 || mali_uv > 950000 || (mali_uv - 300000) % 6250 != 0) {
		say("mali: mali_uv=%u is not a BUCK6 setting (300000..950000, steps of 6250 uV): nothing touched\n", mali_uv);
		return NULL;
	}
	b6out_target = (uint8_t)((mali_uv - 300000) / 6250);
	if (pe_arm_get_soc_base_phys() != STARLTE_SOC_BASE) {
		say("mali: mali_soc=exynos9810 is the Galaxy S9's (the loader's platform starlte, registers from %#lx), and this is "
		    "not it (%#lx): nothing touched\n", STARLTE_SOC_BASE, (unsigned long)pe_arm_get_soc_base_phys());
		return NULL;
	}
	/* the Note FE's Exynos 8890 has its registers from the same address: its platform is told apart by name */
	if (SecureDTLookupEntry(NULL, "/arm-io", &node) == kSuccess &&
	    SecureDTGetProperty(node, "device_type", &prop, &size) == kSuccess && strncmp(prop, "gracerlte", size) == 0) {
		say("mali: mali_soc=exynos9810 on the Galaxy Note FE (an Exynos 8890; mali_soc=exynos8890): nothing touched\n");
		return NULL;
	}
	return &glue;
}
