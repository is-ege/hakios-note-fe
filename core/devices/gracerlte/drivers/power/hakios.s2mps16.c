/*
 * The Galaxy Note FE's main PMIC, a Samsung S2MPS16 at 0x66 on HSI2C 0x10550000: the touch screen's two supplies,
 * which S-Boot leaves off (the device tree marks neither boot-on), for hakios.s6sy761.c's gracerlte board. Called
 * by that driver before it first asks the chip.
 *
 * LDO32 is tsp_io (1.85 V), LDO33 tsp_avdd (3.3 V); their control registers 0x5C and 0x5D: bits 7:6 the on mode
 * (3: on), 5:0 the voltage (LDO32 0.7 V + 25 mV a step, LDO33 1.8 V + 25 mV a step: 0x2E and 0x3C are the tree's
 * voltages). The registers are read always; with s2mps16_tsp=1 the on bits of those two registers, and nothing else
 * of the PMIC, are set (approved by Ege, 8 November 2026) -- only if the voltage field already holds the tree's
 * voltage, each read back -- in Samsung's order (sec_ts_power: tsp_io, then tsp_avdd), then the 65 ms its driver
 * waits before the first transfer (sec_ts_power's 5 ms, probe's 60 ms).
 *
 * The GPU's supplies are only read (hakios_s2mps16_g3d_supply, for hakios.exynos8890_g3d.c). BUCK6 is vdd_g3d,
 * switched by the G3D domain's power pin (B6CTRL1 7:6 = 10, as Samsung's kernel sets for the tree's g3d_en). Its voltage
 * is in B6CTRL3, or in B6CTRL2 while the PMU's GPU DVS pin is high. BUCK11 is vdd_extra, Samsung's g3d_m supply. Both are
 * 300 mV + 6.25 mV a step. Read beside them, for the frequency driver to come: BUCK1-4 (vdd_mif, vdd_mngs, vdd_apollo,
 * vdd_int), CTRL1 0x1A + 2n its on mode, CTRL2 0x1B + 2n its voltage, the same steps.
 *
 * Three bucks are also written, each from the value its caller expects, read back, then the ramp's time at the tree's
 * 12 mV/us (regulator-ramp-delay) and more (hakios_s2mps16_buck_volt; approved by Ege, 8 November 2026): BUCK2 vdd_mngs
 * (B2CTRL2) for the big cluster's rate (hakios.exynos8890_cpu.c), BUCK6 vdd_g3d (B6CTRL3, its voltage while DVS is off)
 * and BUCK11 vdd_extra (B11CTRL2) for the GPU's (hakios.exynos8890_g3d.c).
 *
 * The bus is shared: with the APM, which Samsung's kernel lets take it only after its mailbox driver says the APM
 * is ready (i2c-exynos5.c, the APM notifier: then the pins go to the APM) -- nothing here starts the APM, so the bus
 * is the AP's, as in Samsung's kernel before that -- and with the CP, which does not run. S-Boot drives the bus by
 * hand (the tree's gpio_scl/gpio_sda), leaving the pins as GPIO; they are given to the controller (function 2, the
 * tree's hs-i2c15-bus). The input clock is PCLK_CCORE_66; the controller's Q-channel CMU_CCORE +0x2410.
 *
 * Sources: the phone's device tree (hsi2c@10550000, s2mps16_pmic@66/regulators/LDO32-33, touchscreen@48), Samsung's
 * kernel for the 8890 (LineageOS android_kernel_samsung_universal8890: include/linux/mfd/samsung/s2mps16.h,
 * drivers/regulator/s2mps16.c, drivers/i2c/busses/i2c-exynos5.c, drivers/input/touchscreen/sec_ts/sec_ts.c,
 * S5E8890-cmusfr.h, pinctrl-exynos.c). GPL-2.0: read for facts, no code taken.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_hsi2c.h>

bool hakios_s2mps16_touch_power(int (*say)(const char *, ...));
bool hakios_s2mps16_g3d_supply(int (*say)(const char *, ...), uint8_t *b6ctrl3);
bool hakios_s2mps16_buck_volt(int (*say)(const char *, ...), unsigned buck, uint8_t from, uint8_t to);

#define PMIC                    0x66
#define REG_ID                  0x00
#define ON_MODE                 0xC0

static const struct {
	const char *name;
	uint8_t reg, vsel;
} supplies[] = {
	{ "LDO32 tsp_io 1.85 V", 0x5C, 0x2E },          /* first: Samsung's order */
	{ "LDO33 tsp_avdd 3.3 V", 0x5D, 0x3C },
};

static struct hakios_hsi2c bus = {
	.name = "s2mps16",
	.regs_pa = 0x10550000,
	.cmu_pa = 0x105B0000, .cmu_size = 0x3000,       /* CMU_CCORE */
	.qch = 0x2410,                                  /* QSTATE_CTRL_HSI2C */
	.pins_pa = 0x105A0000,                          /* etc0: SDA pin 0, SCL pin 1 */
	.pins_mask = 0xffu, .pins_want = 0x22u,
	.ipclk_mhz = 66, .bus_hz = 400000,
};

/* the bus has two callers, the touch screen's driver and the G3D's, each from its own thread */
static LCK_GRP_DECLARE(bus_lock_group, "hakios-s2mps16");
static LCK_MTX_DECLARE(bus_lock, &bus_lock_group);
static bool bus_ready;

static bool
bus_start(int (*say)(const char *, ...))
{
	if (!bus_ready && hakios_hsi2c_map(&bus, say)) {
		hakios_hsi2c_power(&bus);
		hakios_hsi2c_reset(&bus);
		bus_ready = true;
	}
	return bus_ready;
}

static bool
touch_power(int (*say)(const char *, ...))
{
	uint8_t id = 0, v[2] = { 0 };
	uint32_t write = 0, e;

	if (!bus_start(say)) {
		return false;
	}
	if ((e = hakios_hsi2c_read_reg(&bus, PMIC, REG_ID, &id, 1)) != 0 ||
	    (e = hakios_hsi2c_read_reg(&bus, PMIC, supplies[0].reg, &v[0], 1)) != 0 ||
	    (e = hakios_hsi2c_read_reg(&bus, PMIC, supplies[1].reg, &v[1], 1)) != 0) {
		say("s2mps16: the PMIC (0x66) did not answer (%x)\n", e);
		return false;
	}
	say("s2mps16: ID %02x; %s: %02x, %s: %02x (bits 7:6 on, 5:0 the voltage)\n", id, supplies[0].name, v[0],
	    supplies[1].name, v[1]);
	if ((v[0] & ON_MODE) == ON_MODE && (v[1] & ON_MODE) == ON_MODE) {
		return true;
	}
	if (!PE_parse_boot_argn("s2mps16_tsp", &write, sizeof(write)) || write == 0) {
		say("s2mps16: the touch screen's supplies are off; s2mps16_tsp=1 switches them on\n");
		return false;
	}
	for (int i = 0; i < 2; i++) {
		if ((v[i] & 0x3F) != supplies[i].vsel) {
			say("s2mps16: %s holds voltage %02x, not the tree's %02x: not switched on\n", supplies[i].name, v[i] & 0x3F,
			    supplies[i].vsel);
			return false;
		}
	}
	for (int i = 0; i < 2; i++) {
		uint8_t w[2] = { supplies[i].reg, (uint8_t)(v[i] | ON_MODE) }, now = 0;

		if ((v[i] & ON_MODE) == ON_MODE) {
			continue;
		}
		e = hakios_hsi2c_message(&bus, PMIC, false, w, sizeof(w), true);
		if (e == 0) {
			e = hakios_hsi2c_read_reg(&bus, PMIC, supplies[i].reg, &now, 1);
		}
		say("s2mps16: s2mps16_tsp=1: %s %02x -> %02x%s\n", supplies[i].name, v[i], now, e ? " (FAILED)" : now == w[1] ?
		    "" : " (NOT as written)");
		if (e != 0 || now != w[1]) {
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	delay_for_interval(65, NSEC_PER_MSEC);
	return true;
}

bool
hakios_s2mps16_touch_power(int (*say)(const char *, ...))
{
	bool on;

	lck_mtx_lock(&bus_lock);
	on = touch_power(say);
	lck_mtx_unlock(&bus_lock);
	return on;
}

/* BUCK1-4, BUCK6 and BUCK11 as found, said; B6CTRL3 (BUCK6's voltage while DVS is off) back. Reads only. */
bool
hakios_s2mps16_g3d_supply(int (*say)(const char *, ...), uint8_t *b6ctrl3)
{
	static const uint8_t regs[] = { 0x24, 0x25, 0x26, 0x30, 0x31, 0x34,     /* B6CTRL1-3, B11CTRL1-2, BUCK_RAMP */
	                                0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21 };     /* B1CTRL1-B4CTRL2 */
	static const char *const names[] = { "vdd_mif", "vdd_mngs", "vdd_apollo", "vdd_int" };
	uint8_t v[sizeof(regs)] = { 0 };
	uint32_t e = 0;

	lck_mtx_lock(&bus_lock);
	if (!bus_start(say)) {
		lck_mtx_unlock(&bus_lock);
		return false;
	}
	for (unsigned i = 0; i < sizeof(regs) && e == 0; i++) {
		e = hakios_hsi2c_read_reg(&bus, PMIC, regs[i], &v[i], 1);
	}
	lck_mtx_unlock(&bus_lock);
	if (e != 0) {
		say("s2mps16: the PMIC (0x66) did not answer (%x)\n", e);
		return false;
	}
	say("s2mps16: BUCK6 vdd_g3d: B6CTRL1 %02x (on mode %u), B6CTRL2 %02x (%u mV, under DVS), B6CTRL3 %02x (%u mV); "
	    "BUCK11 vdd_extra: B11CTRL1 %02x (on mode %u), B11CTRL2 %02x (%u mV); BUCK_RAMP %02x\n", v[0], v[0] >> 6, v[1],
	    300 + v[1] * 25 / 4, v[2], 300 + v[2] * 25 / 4, v[3], v[3] >> 6, v[4], 300 + v[4] * 25 / 4, v[5]);
	for (unsigned i = 0; i < 4; i++) {
		say("s2mps16: BUCK%u %s: CTRL1 %02x (on mode %u), CTRL2 %02x (%u mV)\n", i + 1, names[i], v[6 + 2 * i],
		    v[6 + 2 * i] >> 6, v[7 + 2 * i], 300 + v[7 + 2 * i] * 25 / 4);
	}
	*b6ctrl3 = v[2];
	return true;
}

/* BUCK2, 6 or 11's voltage register (300 mV + 6.25 mV a step) from `from` to `to`; false if it held anything else or did not take. */
bool
hakios_s2mps16_buck_volt(int (*say)(const char *, ...), unsigned buck, uint8_t from, uint8_t to)
{
	uint8_t reg = buck == 2 ? 0x1D : buck == 6 ? 0x26 : buck == 11 ? 0x31 : 0;      /* B2CTRL2, B6CTRL3, B11CTRL2 */
	uint8_t was = 0, now = 0, w[2] = { reg, to };
	uint32_t e;

	if (reg == 0) {
		say("s2mps16: BUCK%u is not one this driver writes\n", buck);
		return false;
	}
	lck_mtx_lock(&bus_lock);
	if (!bus_start(say)) {
		lck_mtx_unlock(&bus_lock);
		return false;
	}
	e = hakios_hsi2c_read_reg(&bus, PMIC, reg, &was, 1);
	if (e == 0 && was == from) {
		e = hakios_hsi2c_message(&bus, PMIC, false, w, sizeof(w), true);
		if (e == 0) {
			e = hakios_hsi2c_read_reg(&bus, PMIC, reg, &now, 1);
		}
	}
	lck_mtx_unlock(&bus_lock);
	say("s2mps16: BUCK%u: %02x (%u mV) -> %02x (%u mV): %s\n", buck, was, 300 + was * 25 / 4, to, 300 + to * 25 / 4,
	    e != 0 ? "the PMIC did not answer" : was != from ? "not the value expected: not written" : now != to ?
	    "NOT as written" : "done");
	if (e != 0 || was != from || now != to) {
		return false;
	}
	delay_for_interval(100, NSEC_PER_USEC);         /* ~12 us a 150 mV step at 12 mV/us */
	return true;
}
