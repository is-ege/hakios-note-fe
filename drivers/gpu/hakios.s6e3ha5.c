/*
 * The Galaxy Note FE's panel brightness (gracerlte, Exynos 8890): a Samsung S6E3HA5 AMOLED (ID 41 60 80) in MIPI
 * command mode on DSIM0 (0x13900000), sent through the DSIM's command FIFO (sys/hakios.exynos_dsim.h) while the DECON
 * keeps showing frames. Only with boot-arg panel=1; panel_brightness=N sets a level of 0-255 once at start.
 *
 * Samsung's brightness (dsim_backlight_grace.c, low_level_set_brightness()) is a gamma (CA) worked out from the
 * panel's MTP, the AOR (B1: how many of a frame's 2584 lines the pixels are dark), ELVSS, VINT and IRC, taken by
 * F7 03 and F7 00. This sends the AOR alone, between the level-2 key:
 *   F0 5A 5A, B1 hi lo, F7 03, F7 00, F0 A5 A5
 * from S-Boot's own AOR (hakios_s6e3ha5_boot_reads: level 255) to Samsung's dimmest (0x9E2: level 0), the lit lines by the
 * square of the level. So the panel goes from S-Boot's brightness down, never above it. Samsung's AOR table is not
 * used: each of its values goes with the gamma of its step, and alone they are not in order.
 * B1's two bytes: the AOR's bits 11-8 in the first's high half, bits 7-0 the second (0x9E2: 90 E2).
 * Not touched: the gamma, and ELVSS (B5) and VINT (F4), which are supply voltages of the panel.
 *
 * The panel's scaler (hakios_s6e3ha5_resolution, for the DECON's driver; Samsung's "FHD" screen setting,
 * _s6e3ha5_wqhd_dsu_command() and dsim_reg_set_partial_update()): frames of 1080 x 1920, which the panel enlarges to
 * its 1440 x 2560, or of 1440 x 2560 again. The DSIM's two slice sizes; then, between the level-2 key, the columns
 * and rows (2A, 2B), B0 08, F2 04 B0, the DSC picture parameters of that size (a PPS packet) and the scaler (BA 02 on,
 * BA 03 from it back); then the DSIM's own resolution (a third of the width: DSC), with its standby off meanwhile.
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

#define DSIM_RESOL      0x20            /* bits 27-16 the rows, 11-0 the columns sent */
#define DSIM_SFR_CTRL   0x48
#define  SFR_STANDBY    (1u << 4)
#define DSIM_SLICE01    0x5C            /* the two DSC slices' widths, bits 31-16 and 15-0 */
#define DSI_PPS         0x0A            /* the packet type of DSC picture parameters */

/* Samsung's S6E3HA5_SEQ_PPS_WQHD_SLICE2 and S6E3HA5_SEQ_DDI_SCALER_FHD_PPS_SLICE2: two slices, 32 rows high */
#define PPS_TAIL \
	0x18, 0x00, 0x10, 0xF0, 0x03, 0x0C, 0x20, 0x00, 0x06, 0x0B, 0x0B, 0x33, 0x0E, 0x1C, 0x2A, 0x38, 0x46, 0x54, 0x62, \
	0x69, 0x70, 0x77, 0x79, 0x7B, 0x7D, 0x7E, 0x01, 0x02, 0x01, 0x00, 0x09, 0x40, 0x09, 0xBE, 0x19, 0xFC, 0x19, 0xFA, \
	0x19, 0xF8, 0x1A, 0x38, 0x1A, 0x78, 0x1A, 0xB6, 0x2A, 0xF6, 0x2B, 0x34, 0x2B, 0x74, 0x3B, 0x74, 0x6B, 0xF4
static const struct resolution {
	uint32_t width, height;
	uint8_t scaler;                 /* BA's byte to come to it */
	uint8_t pps[88];
} resolutions[] = {
	{ 1440, 2560, 0x03, { 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x0A, 0x00, 0x05, 0xA0, 0x00, 0x20, 0x02, 0xD0, 0x02, 0xD0,
		              0x02, 0x00, 0x02, 0x68, 0x00, 0x20, 0x03, 0x87, 0x00, 0x0A, 0x00, 0x0C, 0x03, 0x19, 0x02, 0x63,
		              PPS_TAIL } },
	{ 1080, 1920, 0x02, { 0x11, 0x00, 0x00, 0x89, 0x30, 0x80, 0x07, 0x80, 0x04, 0x38, 0x00, 0x20, 0x02, 0x1C, 0x02, 0x1C,
		              0x02, 0x00, 0x02, 0x0E, 0x00, 0x20, 0x03, 0x15, 0x00, 0x07, 0x00, 0x0C, 0x03, 0x19, 0x03, 0x2E,
		              PPS_TAIL } },
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

static const struct resolution *
resolution_of(uint32_t width)
{
	for (unsigned k = 0; k < sizeof(resolutions) / sizeof(resolutions[0]); k++) {
		if (resolutions[k].width == width) {
			return &resolutions[k];
		}
	}
	return NULL;
}

/* NULL when the panel is not driven (panel=1 not given) or takes no such width */
const uint8_t *
hakios_s6e3ha5_pps(uint32_t width)
{
	const struct resolution *r = resolution_of(width);

	return dsim != 0 && r != NULL ? r->pps : NULL;
}

/* the DECON idle between two frames: the DSIM and the panel to frames of that width */
bool
hakios_s6e3ha5_resolution(uint32_t width)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, seamless[] = { 0xB0, 0x08 }, seamless_1[] = { 0xF2, 0x04, 0xB0 },
	    key_off[] = { 0xF0, 0xA5, 0xA5 };
	const struct resolution *r = resolution_of(width);
	bool ok;

	if (dsim == 0 || r == NULL) {
		return false;
	}
	const uint8_t columns[] = { 0x2A, 0, 0, (uint8_t)((r->width - 1) >> 8), (uint8_t)(r->width - 1) };
	const uint8_t rows[] = { 0x2B, 0, 0, (uint8_t)((r->height - 1) >> 8), (uint8_t)(r->height - 1) };
	const uint8_t scaler[] = { 0xBA, r->scaler };
	uint32_t resol = rd(DSIM_RESOL), slices = rd(DSIM_SLICE01);

	lck_mtx_lock(panel_lock);
	wr(DSIM_SLICE01, (r->width / 2) << 16 | r->width / 2);
	ok = dsim_send(key_on, sizeof(key_on));
	ok = ok && dsim_send(columns, sizeof(columns)) && dsim_send(rows, sizeof(rows)) && dsim_send(seamless, sizeof(seamless)) &&
	    dsim_send(seamless_1, sizeof(seamless_1)) && dsim_packet(DSI_PPS | (uint32_t)sizeof(r->pps) << 8, r->pps, sizeof(r->pps)) &&
	    dsim_send(scaler, sizeof(scaler));
	ok = dsim_send(key_off, sizeof(key_off)) && ok;
	wr(DSIM_SFR_CTRL, rd(DSIM_SFR_CTRL) & ~SFR_STANDBY);
	wr(DSIM_RESOL, (resol & ~0x0fff0fffu) | r->height << 16 | r->width / 3);
	wr(DSIM_SFR_CTRL, rd(DSIM_SFR_CTRL) | SFR_STANDBY);
	lck_mtx_unlock(panel_lock);
	say("s6e3ha5: %ux%u: the DSIM's slices %x -> %x, resolution %x -> %x; the panel's commands %s (FIFOCTRL %x, INTSRC %x)\n",
	    r->width, r->height, slices, rd(DSIM_SLICE01), resol, rd(DSIM_RESOL), ok ? "sent" : "NOT all sent", rd(DSIM_FIFOCTRL),
	    rd(DSIM_INTSRC));
	return ok;
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

/*
 * The panel's ID and S-Boot's AOR, for the DECON's driver to call while no frame is sent: a read turns the link round to
 * the panel, and one that meets a frame times out and leaves the panel taking no command after it (11 November: the
 * switch to 1080 x 1920 then not shown, though sent).
 */
void
hakios_s6e3ha5_boot_reads(void)
{
	static const uint8_t key_on[] = { 0xF0, 0x5A, 0x5A }, key_off[] = { 0xF0, 0xA5, 0xA5 };
	uint8_t id[3] = { 0 }, aor[2] = { 0 };
	uint32_t level = 0;
	bool have_id, have_aor;

	if (dsim == 0) {
		return;
	}
	lck_mtx_lock(panel_lock);
	have_id = dsim_read(REG_ID, id, sizeof(id));
	have_aor = dsim_send(key_on, sizeof(key_on)) && dsim_read(REG_AOR, aor, sizeof(aor));
	(void)dsim_send(key_off, sizeof(key_off));
	lck_mtx_unlock(panel_lock);
	if (have_aor && ((uint32_t)(aor[0] >> 4) << 8 | aor[1]) < AOR_DIMMEST) {
		aor_boot = (uint32_t)(aor[0] >> 4) << 8 | aor[1];
	}
	say("s6e3ha5: DSIM0: FIFOCTRL %x, INTSRC %x; the panel's ID %02x %02x %02x%s; S-Boot's AOR (B1) %02x %02x%s: level 255 "
	    "is AOR 0x%03x, level 0 0x%03x; kern.hakios.brightness sets it\n", rd(DSIM_FIFOCTRL), rd(DSIM_INTSRC), id[0], id[1],
	    id[2], have_id ? "" : " (NOT READ)", aor[0], aor[1], have_aor ? "" : " (NOT READ)", aor_boot, AOR_DIMMEST);
	if (PE_parse_boot_argn("panel_brightness", &level, sizeof(level))) {
		(void)panel_set((int)level);
	}
}

static void
s6e3ha5_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	panel_lock = lck_mtx_alloc_init(lck_grp_alloc_init("s6e3ha5", LCK_GRP_ATTR_NULL), LCK_ATTR_NULL);
	dsim = ml_io_map(dsim_soc.pa, 0x1000);
	if (dsim == 0) {
		say("s6e3ha5: cannot map the DSIM at 0x%llx\n", dsim_soc.pa);
		return;
	}
	(void)hakios_panel_register("s6e3ha5", panel_set);
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
