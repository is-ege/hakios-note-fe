/*
 * The Galaxy Note FE's panel brightness (gracerlte, Exynos 8890): a Samsung S6E3HA5 AMOLED (ID 41 60 80) in MIPI
 * command mode on DSIM0 (0x13900000), sent through the DSIM's command FIFO (sys/hakios.exynos_dsim.h) while the DECON
 * keeps showing frames. Only with boot-arg panel=1; panel_brightness=N sets a level of 0-255 once at start.
 *
 * Samsung's brightness (dsim_backlight_grace.c, low_level_set_brightness()) is a gamma (CA) worked out from the
 * panel's MTP, the AOR (B1: how many of a frame's 2584 lines the pixels are dark), ELVSS, VINT and IRC, taken by
 * F7 03 and F7 00. This sends the AOR alone, between the level-2 key:
 *   F0 5A 5A, B1 hi lo, F7 03, F7 00, F0 A5 A5
 * from S-Boot's own AOR (read at start: level 255) to Samsung's dimmest (0x9E2: level 0), the lit lines by the
 * square of the level. So the panel goes from S-Boot's brightness down, never above it. Samsung's AOR table is not
 * used: each of its values goes with the gamma of its step, and alone they are not in order.
 * B1's two bytes: the AOR's bits 11-8 in the first's high half, bits 7-0 the second (0x9E2: 90 E2).
 * Not touched: the gamma, and ELVSS (B5) and VINT (F4), which are supply voltages of the panel.
 */
#include <stdarg.h>
#include <stdint.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern int hakios_panel_register(const char *name, int (*set)(int level));     /* hakios.backlight.cpp */

SYSCTL_DECL(_kern_hakios);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#include <sys/hakios.exynos_dsim.h>

/* Samsung's regs-dsim.h for the 8890 */
static const struct hakios_dsim dsim_soc = {
	.who = "s6e3ha5", .pa = 0x13900000UL, .intsrc = 0x34, .pkthdr = 0x3C, .payload = 0x40, .rxfifo = 0x44, .fifoctrl = 0x4C,
};

#define REG_ID          0x04
#define REG_AOR         0xB1
#define AOR_BRIGHTEST   0x00C           /* Samsung's at its top level: taken when S-Boot's cannot be read */
#define AOR_DIMMEST     0x9E2           /* Samsung's at level 0 (inter_aor_tbl_ha5) */

static lck_mtx_t *panel_lock;
static int panel_level = -1;
static uint32_t panel_failures;
static uint32_t aor_boot = AOR_BRIGHTEST;

static int
panel_set(int level)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, take[] = { 0xF7, 0x03 }, take_l[] = { 0xF7, 0x00 },
	    key_off[] = { 0xF0, 0xA5, 0xA5 };
	uint32_t aor;
	bool ok;

	if (dsim == 0 || level < 0 || level > 255) {
		return EINVAL;
	}
	aor = AOR_DIMMEST - (AOR_DIMMEST - aor_boot) * (uint32_t)(level * level) / (255 * 255);
	uint8_t aor_packet[] = { REG_AOR, (uint8_t)((aor >> 8) << 4), (uint8_t)aor };

	lck_mtx_lock(panel_lock);
	ok = dsim_send(key_on, sizeof(key_on));
	ok = ok && dsim_send(aor_packet, sizeof(aor_packet)) && dsim_send(take, sizeof(take)) && dsim_send(take_l, sizeof(take_l));
	ok = dsim_send(key_off, sizeof(key_off)) && ok;         /* (the key off whatever happened) */
	if (ok) {
		panel_level = level;
	} else {
		panel_failures++;
	}
	lck_mtx_unlock(panel_lock);
	if (!ok && panel_failures <= 3) {
		say("s6e3ha5: brightness %d (AOR 0x%03x) not sent: the DSIM's FIFO did not take it (FIFOCTRL %x, INTSRC %x)\n",
		    level, aor, rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC));
	} else if (ok) {
		/* at most a line a second: a slider dragged sends many */
		static uint64_t said_at;
		uint64_t now = mach_absolute_time(), second;

		nanoseconds_to_absolutetime(NSEC_PER_SEC, &second);
		if (said_at == 0 || now - said_at >= second) {
			say("s6e3ha5: brightness %d sent (AOR 0x%03x)\n", level, aor);
			said_at = now;
		}
	}
	return ok ? 0 : EIO;
}

/* what inject_sb.c's brightness_bridge writes iOS's slider to */
static int
panel_sysctl_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = panel_level, error;

	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error != 0 || req->newptr == 0) {
		return error;
	}
	return panel_set(value);
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_ANYBODY | CTLFLAG_KERN,
    NULL, 0,
    panel_sysctl_brightness, "I", "the panel's brightness, 0-255 (-1: never set); writing sets it (Galaxy Note FE, panel=1)");

static void
s6e3ha5_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, key_off[] = { 0xF0, 0xA5, 0xA5 };
	uint8_t id[3] = { 0 }, aor[2] = { 0 };
	uint32_t level = 0;
	bool have_id, have_aor;

	panel_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s6e3ha5", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	dsim = ml_io_map(dsim_soc.pa, 0x1000);
	if (dsim == 0) {
		say("s6e3ha5: cannot map the DSIM at 0x%llx\n", dsim_soc.pa);
		return;
	}
	have_id = dsim_read(REG_ID, id, sizeof(id));
	have_aor = dsim_send(key_on, sizeof(key_on)) && dsim_read(REG_AOR, aor, sizeof(aor));
	(void)dsim_send(key_off, sizeof(key_off));
	if (have_aor && ((uint32_t)(aor[0] >> 4) << 8 | aor[1]) < AOR_DIMMEST) {
		aor_boot = (uint32_t)(aor[0] >> 4) << 8 | aor[1];
	}
	say("s6e3ha5: DSIM0: FIFOCTRL %x, INTSRC %x; the panel's ID %02x %02x %02x%s; S-Boot's AOR (B1) %02x %02x%s: level 255 "
	    "is AOR 0x%03x, level 0 0x%03x; kern.hakios.brightness sets it\n", rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC), id[0], id[1],
	    id[2], have_id ? "" : " (NOT READ)", aor[0], aor[1], have_aor ? "" : " (NOT READ)", aor_boot, AOR_DIMMEST);
	(void)hakios_panel_register("s6e3ha5", panel_set);
	if (PE_parse_boot_argn("panel_brightness", &level, sizeof(level))) {
		(void)panel_set((int)level);
	}
}

__startup_func
static void
s6e3ha5_init(void)
{
	uint32_t enabled = 0;

	if (!PE_parse_boot_argn("panel", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	thread_call_enter(thread_call_allocate(s6e3ha5_run, NULL));
}
STARTUP(EARLY_BOOT, STARTUP_RANK_MIDDLE, s6e3ha5_init);
