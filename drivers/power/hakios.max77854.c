/*
 * The Galaxy Note FE's MAX77854 (charger, fuel gauge and MUIC) on HSI2C 0x13650000 (max77854=1): what the MUIC's USB
 * switch and status were left at, and the charger's SAFEOUT control. The USB log (usb_gadget=) needs D+/D- on the AP's
 * USB PHY, and the MUIC's switch decides where they go. S-Boot leaves it open (CTRL1 0x00, seen 8 November 2026, after
 * TWRP had it on USB), so max77854_usbsw=1 writes CTRL1 = 0x09, the AP's USB, as Samsung's com_to_usb_ap does on
 * every USB attach, and reads it back. That one register is the only write to the chip; S-Boot sets it again at boot.
 *
 * MUIC at 0x25: ID 0x00, INT1-3 0x01-0x03, STATUS1-3 0x04-0x06, INTMASK1-3, CDETCTRL1-2 0x0A-0x0B, CTRL1-3 0x0C-0x0E,
 * CTRL4 0x16. CTRL1: COMN1SW [2:0] and COMP2SW [5:3] (0 open, 1 the AP's USB, 3 the AP's UART, 4 the CP's USB, 5 the
 * CP's UART), NOBCCOMP 6: 0x09 is Samsung's "USB to the AP" (com_to_usb_ap). The charger at 0x69: SAFEOUT_CTRL 0xC6.
 * Its bus, HSI2C1 of PERIC0: S-Boot leaves the controller's clock off between
 * transfers as Samsung's I2C driver does (its HWACG gate, CMU_PERIC0 QSTATE_CTRL_HSI2C1 0x13612418 bit 1, set around
 * every transfer), and the first run here stalled with a byte in the FIFO; so that bit is set and left set. The input
 * clock is ACLK_PERIC0_66 (66 MHz), the bus 100 kHz. The pins (gpd1-2 SDA, gpd1-3 SCL: PERIC0 pinctrl 0x136D0040)
 * are left by S-Boot as GPIO (function 1: it drives this bus by hand, the device tree's gpio_sda/gpio_scl); they are
 * given to the controller (function 2, the device tree's hs-i2c1-bus) as Linux's pinctrl does when its driver starts.
 *
 * Sources: Samsung's kernel for the 8890 (LineageOS android_kernel_samsung_universal8890): drivers/mfd/max77854.c
 * (the I2C addresses), include/linux/mfd/max77854-private.h (the registers), include/linux/muic/max77854-muic.h and
 * drivers/muic/max77854-muic.c (CTRL1's fields and values); the phone's device tree (hsi2c@13650000/max77854@66).
 * GPL-2.0: read for facts, no code taken.
 *
 * Status: run on the phone, 8 November 2026: the reads, and the switch (CTRL1 00 -> 09, read back) in every run since.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.exynos_hsi2c.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern int vsnprintf(char *, size_t, const char *, va_list) __printflike(3, 0);

#define QSTATE_HSI2C1_PA        0x13612418UL    /* bit 1: the controller's clock on */
#define GPD1_CON_PA             0x136D0040UL    /* 4 bits a pin: SDA pin 2, SCL pin 3, function 2 */
#define MUIC_ADDR               0x25
#define CHG_ADDR                0x69
#define MUIC_CTRL1              0x0C
#define  CTRL1_USB_AP           0x09            /* COMN1SW and COMP2SW 1: D- and D+ to the AP's USB */
#define MUIC_CTRL4              0x16
#define CHG_SAFEOUT_CTRL        0xC6

static struct hakios_hsi2c bus = {
	.name = "max77854",
	.regs_pa = 0x13650000,
	.ipclk_mhz = 66,
	.bus_hz = 100000,
};

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
