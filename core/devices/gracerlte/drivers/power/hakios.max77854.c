/*
 * The Galaxy Note FE's MAX77854 (charger, fuel gauge and MUIC) on HSI2C 0x13650000 (max77854=1): what the MUIC's USB
 * switch and status were left at, and the charger's SAFEOUT control. The USB log (usb_gadget=) needs D+/D- on the AP's
 * USB PHY, and the MUIC's switch decides where they go. S-Boot leaves it open (CTRL1 0x00, seen 8 November 2026, after
 * TWRP had it on USB), so max77854_usbsw=1 writes CTRL1 = 0x09, the AP's USB, as Samsung's com_to_usb_ap does on
 * every USB attach, and reads it back. S-Boot sets it again at boot.
 *
 * MUIC at 0x25: ID 0x00, INT1-3 0x01-0x03, STATUS1-3 0x04-0x06, INTMASK1-3, CDETCTRL1-2 0x0A-0x0B, CTRL1-3 0x0C-0x0E,
 * CTRL4 0x16. CTRL1: COMN1SW [2:0] and COMP2SW [5:3] (0 open, 1 the AP's USB, 3 the AP's UART, 4 the CP's USB, 5 the
 * CP's UART), NOBCCOMP 6: 0x09 is Samsung's "USB to the AP" (com_to_usb_ap). The charger at 0x69: SAFEOUT_CTRL 0xC6.
 * Its bus, HSI2C1 of PERIC0: S-Boot leaves the controller's clock off between
 * transfers as Samsung's I2C driver does (its HWACG gate, CMU_PERIC0 QSTATE_CTRL_HSI2C1 0x13612418 bit 1, set around
 * every transfer), and the first run here stalled with a byte in the FIFO; so that bit is set and left set. The input
 * clock is ACLK_PERIC0_66 (66 MHz), the bus 100 kHz (TIMING registers in counts, as the touch's bus). The pins (gpd1-2 SDA, gpd1-3 SCL: PERIC0 pinctrl 0x136D0040)
 * are left by S-Boot as GPIO (function 1: it drives this bus by hand, the device tree's gpio_sda/gpio_scl); they are
 * given to the controller (function 2, the device tree's hs-i2c1-bus) as Linux's pinctrl does when its driver starts.
 *
 * The battery: the fuel gauge at 0x36 (16-bit, little-endian: 0x00 status, bit 3 no battery; 0x05 RepCap and 0x35
 * FullCapRep, 1 mAh with the tree's fg_resistor 2; 0x06 RepSOC, 1/256 %; 0x08 temperature, 1/256 C; 0x09 VCELL, 78.125 uV;
 * 0x0A current, signed, 312.5 uA, more than 0 charging; 0x17 cycles, 1/100) and the charger's 0xB2 INT_OK (bit 6 the USB
 * input, bit 5 wireless) and 0xB4 DETAILS_01 ([3:0]: 0-2 charging, 3-4 full), read every 5 seconds and handed to iOS's
 * power source (hakios_battery_report); the MAX77705's registers (drivers/power/hakios.max77705.c).
 *
 * The charger's one write: S-Boot leaves its watchdog on (CNFG_00 WDTEN), and unfed it stops the charging ~100 s into the
 * run for the rest of it (DETAILS_01 0xB, the battery then covering whatever the USB input does not; run 20261112T150418).
 * Each poll restarts it as Samsung's driver does (CNFG_06 WDTCLR, the CHGPROT bits as found; approved by Ege, 12 November
 * 2026). Currents, voltages and the mode stay as S-Boot set them (500 mA in from a computer's USB).
 * The temperature is the battery's thermistor, channel 1 of the SoC's ADC (adc@13620000, exynos-adc-v2: CON1 0x00 bit 2
 * reset, bit 0 start; CON2 0x04 = 0x760 and the channel; DAT 0x0C [11:0]; INT_EN 0x10; INT_ST 0x14), read as Samsung
 * does (five conversions, the highest and the lowest dropped) through the tree's battery,temp_table_adc and _data. Its
 * clock: PCLK_ADCIF, bit 7 of CG_CTRL_VAL_ACLK_PERIC0_66 (0x13610800), and the HWACG bit QSTATE_CTRL_ADCIF 0x1361240C
 * bit 1, set as HSI2C1's is; 25 C is reported if the clock is off or a conversion does not end.
 *
 * Sources: Samsung's kernel for the 8890 (LineageOS android_kernel_samsung_universal8890): drivers/mfd/max77854.c
 * (the I2C addresses), include/linux/mfd/max77854-private.h (the registers), include/linux/muic/max77854-muic.h and
 * drivers/muic/max77854-muic.c (CTRL1's fields and values), drivers/battery_v2/max77854_fuelgauge.c and include/charger/max77854_charger.h; the phone's device tree (hsi2c@13650000/max77854@66).
 * GPL-2.0: read for facts, no code taken.
 *
 * Status: run on the phone, 8 November 2026: the reads, and the switch (CTRL1 00 -> 09, read back) in every run since.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_hsi2c.h>
#include <sys/hakios.input.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern void loader_log_line(const char *line);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);

#define QSTATE_HSI2C1_PA        0x13612418UL    /* bit 1: the controller's clock on */
#define QSTATE_ADCIF_PA         0x1361240CUL    /* bit 1: the ADC's clock on */
#define CG_VAL_PERIC0_66_PA     0x13610800UL    /* bit 7: PCLK_ADCIF */
#define ADC_PA                  0x13620000UL
#define ADC_CON1                0x00
#define ADC_CON2                0x04
#define ADC_DAT                 0x0C
#define ADC_INT_EN              0x10
#define ADC_INT_ST              0x14
#define ADC_CHANNEL             1               /* the tree's adc-temp */
#define GPD1_CON_PA             0x136D0040UL    /* 4 bits a pin: SDA pin 2, SCL pin 3, function 2 */
#define MUIC_ADDR               0x25
#define CHG_ADDR                0x69
#define MUIC_CTRL1              0x0C
#define  CTRL1_USB_AP           0x09            /* COMN1SW and COMP2SW 1: D- and D+ to the AP's USB */
#define MUIC_CTRL4              0x16
#define CHG_SAFEOUT_CTRL        0xC6
#define CHG_INT_OK              0xB2            /* the charger's status and settings, read together: 0xB2-0xC0 */
#define CHG_DETAILS_00          0xB3            /* [6:5] CHGIN_DTLS: 3 the input valid */
#define CHG_DETAILS_01          0xB4
#define CHG_DETAILS_02          0xB5
#define CHG_CNFG_00             0xB7            /* bit 4 WDTEN: the charger's watchdog; [3:0] MODE */
#define  WDTEN                  0x10
#define CHG_CNFG_02             0xB9            /* [5:0] the charge current, 50 mA */
#define CHG_CNFG_06             0xBD            /* [3:2] CHGPROT, [1:0] WDTCLR: 1 restarts the watchdog */
#define  WDTCLR                 0x01
#define CHG_CNFG_09             0xC0            /* [6:0] the input current limit, 33.3 mA */
#define CHG_LAST                CHG_CNFG_09
#define FG_ADDR                 0x36
#define FG_STATUS               0x00
#define FG_REPCAP               0x05
#define FG_REPSOC               0x06
#define FG_TEMPERATURE          0x08
#define FG_VCELL                0x09
#define FG_CURRENT              0x0A
#define FG_CYCLES               0x17
#define FG_FULLCAPREP           0x35
#define DESIGN_MAH              3103            /* the tree's fuelgauge,capacity */
#define POLL_S                  5

static struct hakios_hsi2c bus = {
	.name = "max77854",
	.regs_pa = 0x13650000,
	.ipclk_mhz = 66,
	.bus_hz = 100000,
	.timing_counts = true,
};

/* the tree's battery,temp_table_adc and _data: the ADC's reading, and tenths of a degree */
static const uint16_t temp_adc[] = { 303, 357, 416, 473, 555, 649, 756, 832, 886, 940, 1025, 1094, 1127, 1196, 1269, 1384, 1588, 1809, 1947, 2041, 2276, 2515, 2745, 2960, 3037, 3157, 3261, 3329, 3385, 3470, 3593, 3701 };
static const int16_t temp_data[] = { 900, 850, 800, 750, 700, 650, 600, 570, 550, 530, 500, 480, 470, 450, 430, 400, 360, 310, 280, 260, 210, 160, 100, 50, 30, 0, -30, -50, -70, -100, -150, -200 };
_Static_assert(sizeof(temp_adc) / sizeof(temp_adc[0]) == sizeof(temp_data) / sizeof(temp_data[0]), "one table");
static volatile uint32_t *adc;
static int last_adc = -1;

static int say(const char *format, ...) __printflike(1, 2);

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

static const char *
com_name(uint8_t sw)
{
	static const char *const names[8] = { "open", "AP USB", "?2", "AP UART", "CP USB", "CP UART", "?6", "?7" };

	return names[sw & 7];
}

static uint32_t
fg16(uint8_t reg, uint16_t *value)
{
	uint8_t b[2];
	uint32_t e = hakios_hsi2c_read_reg(&bus, FG_ADDR, reg, b, 2);

	*value = (uint16_t)(b[1] << 8 | b[0]);
	return e;
}

/* one conversion of the thermistor's channel; -1 if it does not finish */
static int
adc_convert(void)
{
	adc[ADC_CON2 / 4] = (adc[ADC_CON2 / 4] & ~0xfu) | ADC_CHANNEL;
	adc[ADC_CON1 / 4] |= 1;
	for (int waited = 0; waited < 10000; waited++) {
		if (adc[ADC_INT_ST / 4] & 1) {
			int value = (int)(adc[ADC_DAT / 4] & 0xfff);

			adc[ADC_INT_ST / 4] = 1;
			return value;
		}
		delay(1);
	}
	return -1;
}

/* the battery's temperature in hundredths of a degree; 2500 when the ADC does not answer */
static int32_t
battery_temperature(void)
{
	const unsigned n = sizeof(temp_adc) / sizeof(temp_adc[0]);
	int sample[5], low = 0, high = 0, sum = 0, raw;

	last_adc = -1;
	if (adc == NULL) {
		return 2500;
	}
	for (int i = 0; i < 5; i++) {
		if ((sample[i] = adc_convert()) < 0) {
			return 2500;
		}
		low = sample[i] < sample[low] ? i : low;
		high = sample[i] > sample[high] ? i : high;
	}
	for (int i = 0; i < 5; i++) {
		sum += (i == low || i == high) ? 0 : sample[i];
	}
	last_adc = raw = sum / (low == high ? 4 : 3);
	if (raw <= temp_adc[0]) {
		return temp_data[0] * 10;
	}
	for (unsigned i = 1; i < n; i++) {
		if (raw <= temp_adc[i]) {
			return temp_data[i - 1] * 10 + (temp_data[i] - temp_data[i - 1]) * 10 * (raw - temp_adc[i - 1]) /
			       (temp_adc[i] - temp_adc[i - 1]);
		}
	}
	return temp_data[n - 1] * 10;
}

static thread_call_t battery_call;

static void
battery_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static unsigned tick, failed, wdt_failed;
	static uint8_t dtls_before = 0xff;
	uint16_t status, soc, vcell, current, cap, full, cycles, temp;
	uint8_t chg[CHG_LAST - CHG_INT_OK + 1], ok, dtls;
	uint64_t deadline, t0 = mach_absolute_time(), ns;

	if (fg16(FG_STATUS, &status) || fg16(FG_REPSOC, &soc) || fg16(FG_VCELL, &vcell) || fg16(FG_CURRENT, &current) ||
	    fg16(FG_REPCAP, &cap) || fg16(FG_FULLCAPREP, &full) || fg16(FG_CYCLES, &cycles) || fg16(FG_TEMPERATURE, &temp) ||
	    hakios_hsi2c_read_reg(&bus, CHG_ADDR, CHG_INT_OK, chg, sizeof(chg))) {
		if (++failed == 1 || failed % 60 == 0) {
			say("max77854: the gauge or the charger did not answer (%x), %u times in a row\n", bus.last_error, failed);
		}
	} else {
		struct hakios_battery_state b = { .size = sizeof(b) };

		failed = 0;
		ok = chg[CHG_INT_OK - CHG_INT_OK];
		dtls = chg[CHG_DETAILS_01 - CHG_INT_OK];
		if (chg[CHG_CNFG_00 - CHG_INT_OK] & WDTEN) {
			uint8_t w[2] = { CHG_CNFG_06, (uint8_t)((chg[CHG_CNFG_06 - CHG_INT_OK] & ~3u) | WDTCLR) };
			uint32_t e = hakios_hsi2c_message(&bus, CHG_ADDR, false, w, sizeof(w), true);

			if (e != 0 && (++wdt_failed == 1 || wdt_failed % 60 == 0)) {
				say("max77854: the charger's watchdog not restarted (%x), %u times in a row\n", e, wdt_failed);
			} else if (e == 0) {
				wdt_failed = 0;
			}
		}
		b.percent_hundredths = MIN((int32_t)((uint32_t)soc * 100 / 256), 10000);
		b.voltage_uv = (int32_t)((uint64_t)vcell * 78125 / 1000);
		b.current_ua = (int32_t)(int16_t)current * 3125 / 10;
		b.temperature_hundredths = battery_temperature();
		b.present = !(status & (1u << 3));
		b.external = (ok & (3u << 5)) != 0;
		b.status = (dtls & 0xf) <= 2 ? 1 : (dtls & 0xf) <= 4 ? 4 : b.external ? 3 : 2;
		b.charge_uah = (int32_t)cap * 1000;
		b.full_uah = (int32_t)full * 1000;
		b.design_uah = DESIGN_MAH * 1000;
		b.cycles = cycles / 100;
		b.usb_type = b.external ? 2 : 0;
		hakios_battery_report(&b);
		/* each change of the charger's state, and every 5 minutes */
		if (tick++ % 60 == 0 || dtls != dtls_before) {
			uint8_t lim = chg[CHG_CNFG_09 - CHG_INT_OK] & 0x7f, cc = chg[CHG_CNFG_02 - CHG_INT_OK] & 0x3f;

			dtls_before = dtls;
			say("max77854: charger: DETAILS_00-02 %02x %02x %02x CNFG_00 %02x (watchdog %s, mode %x) CNFG_02 %02x "
			    "(%d mA) CNFG_06 %02x CNFG_09 %02x (input %d mA)\n", chg[CHG_DETAILS_00 - CHG_INT_OK], dtls,
			    chg[CHG_DETAILS_02 - CHG_INT_OK], chg[CHG_CNFG_00 - CHG_INT_OK],
			    chg[CHG_CNFG_00 - CHG_INT_OK] & 0x10 ? "on" : "off", chg[CHG_CNFG_00 - CHG_INT_OK] & 0xf, cc,
			    cc <= 2 ? 100 : cc * 50, chg[CHG_CNFG_06 - CHG_INT_OK], lim, lim <= 3 ? 100 : lim * 100 / 3);
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
			say("max77854: battery: %d.%02d%%, %d mV, %d mA, %s, %s, %d of %d mAh (full %d), %d cycles; INT_OK %02x "
			    "DETAILS_01 %02x; %d.%d C (ADC %d; the gauge's own %d C); read in %llu us\n", b.percent_hundredths / 100,
			    b.percent_hundredths % 100, b.voltage_uv / 1000, b.current_ua / 1000, b.present ? "a battery" : "NO BATTERY",
			    b.status == 1 ? "charging" : b.status == 4 ? "full" : b.status == 3 ? "on a charger, not charging" :
			    "discharging", b.charge_uah / 1000, DESIGN_MAH, b.full_uah / 1000, b.cycles, ok, dtls, b.temperature_hundredths / 100,
			    b.temperature_hundredths % 100 / 10, last_adc, (int16_t)temp / 256, ns / 1000);
		}
	}
	clock_interval_to_deadline(POLL_S, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(battery_call, deadline);
}

static void
max77854_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint8_t muic[15] = { 0 }, ctrl4 = 0, safeout = 0;
	uint32_t e;

	vm_offset_t qstate = ml_io_map(QSTATE_HSI2C1_PA & ~0xfffUL, 0x1000), pins = ml_io_map(GPD1_CON_PA & ~0xfffUL, 0x1000);

	if (!hakios_hsi2c_map(&bus, say) || qstate == 0 || pins == 0) {
		return;
	}
	volatile uint32_t *q = (volatile uint32_t *)(qstate + (QSTATE_HSI2C1_PA & 0xfff));
	volatile uint32_t *con = (volatile uint32_t *)(pins + (GPD1_CON_PA & 0xfff));
	uint32_t before = *q, con_before = *con;

	*q = before | 2;
	*con = (con_before & ~0xFF00u) | 0x2200u;
	__asm__ volatile ("dsb sy" ::: "memory");
	say("max77854: HSI2C1 clock (QSTATE_CTRL) %x -> %x; pins gpd1 CON %08x -> %08x (SDA and SCL: 1 GPIO, 2 I2C)\n", before,
	    *q, con_before, *con);
	hakios_hsi2c_reset(&bus);
	if ((e = hakios_hsi2c_read_reg(&bus, MUIC_ADDR, 0x00, muic, sizeof(muic))) != 0) {
		say("max77854: the MUIC (0x25) did not answer (%x)\n", e);
		return;
	}
	(void)hakios_hsi2c_read_reg(&bus, MUIC_ADDR, MUIC_CTRL4, &ctrl4, 1);
	e = hakios_hsi2c_read_reg(&bus, CHG_ADDR, CHG_SAFEOUT_CTRL, &safeout, 1);
	say("max77854: MUIC ID %02x STATUS1-3 %02x %02x %02x CDETCTRL1-2 %02x %02x CTRL1-4 %02x %02x %02x %02x\n", muic[0],
	    muic[4], muic[5], muic[6], muic[10], muic[11], muic[12], muic[13], muic[14], ctrl4);
	say("max77854: the USB switch (CTRL1 %02x): D- to %s, D+ to %s; charger SAFEOUT_CTRL %02x%s\n", muic[12],
	    com_name(muic[12] & 7), com_name((muic[12] >> 3) & 7), safeout, e ? " (not read)" : "");

	uint32_t usbsw = 0;
	if (PE_parse_boot_argn("max77854_usbsw", &usbsw, sizeof(usbsw)) && usbsw != 0 && muic[12] != CTRL1_USB_AP) {
		uint8_t w[2] = { MUIC_CTRL1, CTRL1_USB_AP }, now = 0;

		e = hakios_hsi2c_message(&bus, MUIC_ADDR, false, w, sizeof(w), true);
		if (e == 0) {
			e = hakios_hsi2c_read_reg(&bus, MUIC_ADDR, MUIC_CTRL1, &now, 1);
		}
		say("max77854: max77854_usbsw=1: CTRL1 %02x -> %02x%s\n", muic[12], now,
		    e ? " (FAILED)" : now == CTRL1_USB_AP ? " (D- and D+ to the AP's USB)" : " (NOT as written)");
	}

	vm_offset_t cg = ml_io_map(CG_VAL_PERIC0_66_PA & ~0xfffUL, 0x1000), a = ml_io_map(ADC_PA, 0x1000);
	volatile uint32_t *aq = (volatile uint32_t *)(qstate + (QSTATE_ADCIF_PA & 0xfff));
	uint32_t aq_before = *aq, val;

	*aq = aq_before | 2;
	__asm__ volatile ("dsb sy" ::: "memory");
	val = cg ? *(volatile uint32_t *)(cg + (CG_VAL_PERIC0_66_PA & 0xfff)) : 0;
	say("max77854: the ADC's clock: QSTATE_CTRL_ADCIF %x -> %x, CG_CTRL_VAL_ACLK_PERIC0_66 %08x (PCLK_ADCIF, bit 7: %u)\n",
	    aq_before, *aq, val, (val >> 7) & 1);
	if (a != 0 && (val & (1u << 7))) {
		adc = (volatile uint32_t *)a;
		adc[ADC_CON1 / 4] = 1u << 2;
		adc[ADC_CON2 / 4] = 0x760;
		adc[ADC_INT_EN / 4] = 1;
	}
	battery_call = thread_call_allocate(battery_poll, NULL);
	battery_poll(NULL, NULL);
}

__startup_func
static void
max77854_init(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("max77854", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	thread_call_enter(thread_call_allocate(max77854_run, NULL));
}
STARTUP(EARLY_BOOT, STARTUP_RANK_MIDDLE, max77854_init);
