/*
 * The Galaxy Note FE's display controller (Exynos 8890 DECON_F) as the display driver's scan-out: the S9's
 * hakios.exynos_dpu.c for this SoC, behind the same calls (MiniIOMobileFramebuffer.cpp, fb_flip=1). A frame is drawn
 * into a buffer of its own and the window's DMA channel is pointed at it from the next refresh on, instead of the frame
 * being drawn into the buffer the DECON is reading (which tears: the stripes seen on the phone, 8 November).
 *
 * What S-Boot leaves: DECON_F (0x13960000) in MIPI command mode, its frames started by the panel's TE (the shim unmasks
 * that trigger, shim.c), one window on, whose channel (WIN_CONTROL CHMAP) is a VPP DMA block reading the 1440 x 2560
 * boot framebuffer at 0xD8000000. The flip: the block's BASE_ADDR_Y(0)/CB(0), then PINGPONG_UPDATE; the block takes
 * the address at its next frame and clears the bit, and SHA_BASE_ADDR_Y says what it reads (Samsung's
 * vpp_reg_set_in_buf_addr, vpp_reg_wait_pingpong_clear). Only then is the next frame flipped (hakios_scanout_latched),
 * so no buffer is written while it is read.
 *
 * Two modes, as the S9's: 1 (fb_flip) flips on S-Boot's channel, the scaling the processor's; 2 (fb_dpp) the scaling
 * the panel's: the S6E3HA5 enlarges 1080 x 1920 to its 1440 x 2560 itself (Samsung's "FHD" screen setting, its
 * CONFIG_FB_DSU: decon_dsu_handler, decon_reg_configure_lcd_dsu, decon_reg_set_partial_update). Between two frames,
 * the trigger masked: the two DSC encoders' picture parameters (the panel driver's table for that size, each encoder
 * half the width), the blender's, splitter's, frame FIFOs' and interface's sizes, then the DSIM and the panel
 * (hakios_s6e3ha5_resolution), then the window: the display's own picture (1035 x 1840), not scaled, in the middle of
 * the 1080 x 1920, the rest the blender's black. Each register is first compared with what Samsung's arithmetic gives
 * for 1440 x 2560: a DECON that S-Boot set up otherwise is left alone. If no frame ends afterwards, everything goes
 * back. (The DECON's own scalers, VGR0 and VGR1, need a call into the secure world that Samsung's kernel makes and
 * this one does not; the code that tried VGR1 is in this file's history.)
 *
 * The panel's TE reaches the DECON through pin gpb0-1 (PERIC0, 0x136D00E0) in its function 2 (Samsung's pinctrl state
 * decon_te_on, the tree's decon_f_te_on). With the pin left an input, the DECON is never held back for a TE: measured on
 * the phone (8 November), frames started back to back, one every 24 ms against the panel's 16.7 ms, unaligned with its
 * scan. The pin was found already given to the DECON; the probe makes sure of it, as Samsung's driver does when the
 * display is on.
 *
 * Those 24 ms are S-Boot's VCLK: the link is Samsung's (DSC on, two encoders, 480 x 2560 sent on four lanes of
 * 897 Mbit/s, about 8 ms), but the DECON's VCLK0 comes from the 26 MHz oscillator, two compressed pixels a cycle:
 * 614 400 cycles, 23.6 ms (the registers, read on the phone 8 November). That is longer than a refresh: a TE always
 * comes during a frame, the DECON keeps it, and starts
 * the next frame the moment one ends -- out of step with the panel's scan, which then shows part of one frame and part
 * of the next (the stripes). So the trigger masks itself at every frame's start (TRIG_AUTO_MASK) and the watcher
 * unmasks it at the frame's end: each frame starts at a TE after the last one's end, every other refresh (30 a second),
 * and a frame begun at the scan's start is never overtaken by it. Seen on the phone, 8 November: 152 frames in 5 s,
 * the stripes gone.
 *
 * Boot-arg decon_vclk42=1: VCLK0 at Samsung's 42 MHz for this mode (decon_reg_8890.c decon_clocks_table: 1440 x 2560,
 * two DSC encoders), CMU_TOP's BUS2 PLL (672 MHz) divided by 16, as its ECLK is by 4: a frame in 14.6 ms, every
 * refresh. Switched in the watcher at a frame's end, the DECON idle and its trigger masked; put back if a frame does
 * not end or underruns. No PLL, voltage, DSI or panel write. (Samsung's S5E8890-cmu.c, pwrcal-cmu.c.)
 *
 * Its status, watched from the probe on (nothing here takes the DECON's interrupt, which stays off at the GIC; its
 * frame-done and FIFO-level bits enabled in INTERRUPT_ENABLE only so that INTERRUPT_PENDING shows them): the frames sent
 * to the panel (the refreshes, hakios_scanout_refreshes) and the underruns (FIFO level, Samsung's decon_f_irq_handler
 * counts them the same way), said every 5 s while they change.
 *
 * Registers (Samsung's kernel for the 8890, LineageOS android_kernel_samsung_universal8890, drivers/video/fbdev/exynos/
 * decon_8890: regs-decon.h, decon_reg_8890.c decon_get_win_channel, dsc_reg_init, decon_reg_config_dsc_size,
 * decon_common.h enum decon_idma_type; vpp/regs-vpp.h,
 * vpp/vpp_reg_8890.c; the phone's device tree: decon_f@0x13960000, vpp@0x13951000-0x13E05000 as vpp0-8, vppN serving
 * idma type N. GPL-2.0: read for these facts, nothing copied):
 *   DECON_F: GLOBAL_CONTROL 0x000 (bit 8 command mode, bit 4 running), WIN_CONTROL(w) 0x130 + 0x20 w (bit 0 on, bits
 *   12-14 the channel), WIN_START/END_POSITION 0x134/0x138 + 0x20 w; eight windows.
 *   Channels to idma types: 7 G0, 0 G1, 1 G2, 2 G3, 3 VG0, 4 VG1, 5 VGR0, 6 VGR1.
 *   VPP: ENABLE 0x00, IN_CON 0x08 (bits 11-15 the format), SRC_SIZE 0x10 (width = the row in pixels, height), IMG_SIZE
 *   0x18, PINGPONG_UPDATE 0x1C (bit 0), BASE_ADDR_Y(0) 0x74, BASE_ADDR_CB(0) 0x94, SHA_BASE_ADDR_Y 0xB74.
 */
#include <stdarg.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/hakios.exynos8890_mem.h>
#include <sys/hakios.viewport.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern void IOSleep(unsigned milliseconds);

/* the display driver's side (MiniIOMobileFramebuffer.cpp), as hakios.exynos_dpu.c has them */
int hakios_scanout_viewport(struct hakios_viewport *v, uint32_t *pw, uint32_t *ph);
int hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes);
int hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys);
int hakios_scanout_attach(int mode, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height,
    uint32_t rowbytes, unsigned shown);
int hakios_scanout_flip(unsigned index);
int hakios_scanout_latched(void);
void hakios_scanout_detach(const char *why);
uint32_t hakios_scanout_refreshes(void);
int hakios_scanout_carve(uint64_t bytes, uint64_t *phys);
void hakios_scanout_uncarve(uint64_t phys);
int hakios_scanout_carved(uint64_t phys);
int hakios_scanout_show(uint64_t phys, uint32_t rowbytes);

static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[320];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

#define DECON_PA                0x13960000UL
#define GLOBAL_CONTROL          0x000
#define  GC_CMD_MODE            (1u << 8)
#define  GC_RUN                 (1u << 4)
#define INTERRUPT_ENABLE        0x040
#define  INT_VSTATUS            (1u << 24)      /* the frame's start (S-Boot selects its point, bits 20-21) */
#define  INT_FRAME_DONE         (1u << 12)
#define  INT_FIFO_LEVEL         (1u << 8)
#define  INT_EN                 (1u << 0)
#define UNDER_RUN_THRESHOLD     0x044
#define INTERRUPT_PENDING       0x048
#define SHADOW_REQ              0x060
#define  SHADOW_GLOBAL          (1u << 31)
#define  SHADOW_WIN(w)          (1u << (w))
#define BLENDER_BG_COLOR        0x118
#define TRIG_CONTROL            0x070           /* bit 12 auto mask, 8 SW trigger, 5 HW trigger masked, 4 HW trigger */
#define  TRIG_AUTO_MASK         (1u << 12)
#define  TRIG_HW_MASK           (1u << 5)
#define TE_PIN_CON_PA           0x136D00E0UL    /* gpb0: CON, DAT +4; the TE is pin 1 */
#define TE_PIN                  1
#define TE_FUNCTION             2
#define WIN_CONTROL(w)          (0x130 + 0x20 * (w))
#define  WIN_EN                 (1u << 0)
#define  WIN_CHMAP(v)           (((v) >> 12) & 7)
#define  WIN_CHMAP_MASK         (7u << 12)
#define WIN_START(w)            (0x134 + 0x20 * (w))
#define WIN_END(w)              (0x138 + 0x20 * (w))
#define WIN_PIXEL_COUNT(w)      (0x144 + 0x20 * (w))    /* the window's pixels: the DECON waits for as many */
#define WINDOWS                 8

#define VG_ENABLE               0x00
#define  VG_OP_STATUS           (1u << 2)
#define  VG_SFR_UPDATE_FORCE    (1u << 4)
#define VG_IRQ                  0x04
#define  VG_IRQ_ERRORS          ((1u << 17) | (1u << 19))       /* deadlock, read slave error */
#define VG_IN_CON               0x08
#define VG_OUT_CON              0x0C
#define VG_SRC_OFFSET           0x14
#define VG_SRC_SIZE             0x10
#define VG_IMG_SIZE             0x18
#define VG_PINGPONG             0x1C
#define  PINGPONG_UPDATE        (1u << 0)
#define VG_SCALED_SIZE          0x2C
#define VG_H_RATIO              0x44
#define VG_V_RATIO              0x48
#define VG_SMART_IF_PIXELS      0xA48
#define VG_BASE_Y               0x74
#define VG_BASE_CB              0x94
#define VG_SHA_BASE_Y           0xB74

/* idma type -> its VPP block (vppN serves type N); channel -> idma type */
static const uint64_t vpp_pa[8] = { 0x13951000, 0x13952000, 0x13953000, 0x13954000, 0x13E01000, 0x13E02000,
	                            0x13E03000, 0x13E04000 };
static const char *const type_name[8] = { "G0", "G1", "VG0", "VG1", "G2", "G3", "VGR0", "VGR1" };
static const unsigned type_of_channel[8] = { 1, 4, 5, 2, 3, 6, 7, 0 };
/* the sizes a resolution sets (regs-decon.h); the encoders' are 0x2000 and 0x3000 above the DECON */
#define BLENDER_BG_IMAGE_SIZE   0x110
#define BLENDER_BG_PIXELS       0x114
#define DISPIF0_SIZE_0          0x0A0
#define DISPIF0_SIZE_1          0x0A4
#define SPLITTER_SIZE_0         0x244
#define SPLITTER_SIZE_1         0x248
#define FRAME_FIFO_0_SIZE_0     0x250
#define FRAME_FIFO_0_SIZE_1     0x254
#define FRAME_FIFO_1_SIZE_0     0x258
#define FRAME_FIFO_1_SIZE_1     0x25C
#define DSC(e)                  (0x2000 + 0x1000 * (e))
#define DSC_PPS(n)              (0x20 + (n))    /* the picture parameters' bytes n to n + 3, the first the highest */
#define DSC_IN_PIXEL_COUNT      0x0C
#define DSC_COMP_PIXEL_COUNT    0x10
#define LCD_REGS                30
#define LCD_REGS_SURE           26              /* the rest only where S-Boot had Samsung's value */
/* VCLK0's path (decon_vclk42): CMU_TOP's mux (bits 12-13, 2 BUS2) and divider (bits 0-3, 25 busy), its gate; CMU_DISP0's
 * user mux (bit 12: 1 TOP's, 0 the oscillator), the DECON's mux (bits 12-14: 0 the user mux, 2 DISP_PLL's mux; status
 * one-hot from bit 12), DISP_PLL's mux (bit 12: 0 the oscillator) */
#define CMU_TOP_PA              0x10570000UL
#define  TOP_BUS2_PLL           0x140
#define  TOP_VCLK0_MUX          0x2F8
#define  TOP_VCLK0_DIV          0x458
#define  TOP_VCLK0_GATE         0x904
#define CMU_DISP0_PA            0x13AD0000UL
#define  DISP0_PLL_MUX          0x200
#define  DISP0_VCLK0_USER       0x210
#define  DISP0_VCLK0_MUX        0x260
#define  DISP0_VCLK0_DIV        0x408
#define  DISP0_VCLK0_MUX_STAT   0x660

static vm_offset_t decon, vpp;           /* vpp: the channel the window reads */
static int probed;                      /* 1: as expected; -1: not, nothing is written */
static unsigned win, boot_type;
static uint32_t boot_base, boot_win_start, boot_win_end, boot_bg;
static int mode;                        /* 0 the boot buffer, 1 flips on G1, 2 the panel's scaler */
static struct hakios_viewport window;   /* mode 2: where the picture is ... */
static uint32_t lcd_w, lcd_h;           /* ... on what the DECON sends; the panel's own size otherwise */
static uint32_t panel_w, panel_h;
static uint32_t boot_src, boot_img, boot_scaled, boot_pixels;
static uint32_t lcd_boot[LCD_REGS];      /* lcd_reg's values as S-Boot left them */
static bool hold;                       /* the watcher leaves the trigger masked at a frame's end */
static uint64_t buffers[4];
static unsigned nbuffers;
static uint32_t flipped;                /* the address last asked for */
static uint32_t refreshes, underruns, starts;
static vm_offset_t cmu_top, cmu_disp0;
static int vclk;                        /* 0 S-Boot's, 1 asked for, 2 at 42 MHz, 3 kept, -1 refused or put back */
static uint32_t vclk_was[4];            /* S-Boot's TOP divider, TOP gate, DISP0 user mux, DISP0 mux */

/* drivers/gpu/hakios.s6e3ha5.c: its reads, the DSC picture parameters the panel takes at a width, the DSIM and panel told */
extern void hakios_s6e3ha5_boot_reads(void);
extern const uint8_t *hakios_s6e3ha5_pps(uint32_t width);
extern bool hakios_s6e3ha5_resolution(uint32_t width);

extern kern_return_t kernel_thread_start_priority(thread_continue_t continuation, void *parameter, integer_t priority,
    thread_t *new_thread);
extern kern_return_t thread_terminate(thread_t thread);

static inline uint32_t rd(vm_offset_t base, uint32_t offset) { return *(volatile uint32_t *)(base + offset); }
static inline void wr(vm_offset_t base, uint32_t offset, uint32_t value) { *(volatile uint32_t *)(base + offset) = value; }

static bool
cmu_wait(vm_offset_t base, uint32_t offset, uint32_t mask, uint32_t want)
{
	for (unsigned i = 0; i < 100000; i++) {
		if ((rd(base, offset) & mask) == want) {
			return true;
		}
	}
	return false;
}

static void
vclk_back(void)
{
	wr(cmu_disp0, DISP0_VCLK0_MUX, vclk_was[3]);
	cmu_wait(cmu_disp0, DISP0_VCLK0_MUX_STAT, 0xffu << 12, 1u << (12 + (vclk_was[3] >> 12 & 7)));
	wr(cmu_disp0, DISP0_VCLK0_USER, vclk_was[2]);
	wr(cmu_top, TOP_VCLK0_GATE, vclk_was[1]);
	wr(cmu_top, TOP_VCLK0_DIV, vclk_was[0]);
}

/* VCLK0 26 -> 42 MHz, the DECON idle: only from the path S-Boot leaves (see the head of the file) */
static bool
vclk_42(void)
{
	uint32_t bus2 = rd(cmu_top, TOP_BUS2_PLL), top_mux = rd(cmu_top, TOP_VCLK0_MUX), p = bus2 >> 8 & 0x3f;
	uint32_t bus2_mhz = (bus2 >> 31) && (bus2 >> 29 & 1) && p != 0 ? 26 * (bus2 >> 16 & 0x3ff) / (p << (bus2 & 7)) : 0;

	vclk_was[0] = rd(cmu_top, TOP_VCLK0_DIV);
	vclk_was[1] = rd(cmu_top, TOP_VCLK0_GATE);
	vclk_was[2] = rd(cmu_disp0, DISP0_VCLK0_USER);
	vclk_was[3] = rd(cmu_disp0, DISP0_VCLK0_MUX);
	if (bus2_mhz != 672 || (top_mux >> 12 & 3) != 2 || !(top_mux >> 21 & 1) || (vclk_was[2] >> 12 & 1) != 0 ||
	    (vclk_was[3] >> 12 & 7) != 2 || (rd(cmu_disp0, DISP0_PLL_MUX) >> 12 & 1) != 0 ||
	    (rd(cmu_disp0, DISP0_VCLK0_DIV) & 7) != 0) {
		say("exynos8890_decon: VCLK0 not on S-Boot's path (BUS2 %u MHz, TOP mux %x div %x, DISP0 user %x mux %x, "
		    "DISP_PLL mux %x div %x): left at the oscillator\n", bus2_mhz, top_mux, vclk_was[0], vclk_was[2], vclk_was[3],
		    rd(cmu_disp0, DISP0_PLL_MUX), rd(cmu_disp0, DISP0_VCLK0_DIV));
		return false;
	}
	wr(cmu_top, TOP_VCLK0_DIV, (vclk_was[0] & ~0xfu) | 15);
	if (!cmu_wait(cmu_top, TOP_VCLK0_DIV, 1u << 25, 0)) {
		goto back;
	}
	wr(cmu_top, TOP_VCLK0_GATE, vclk_was[1] | 1);
	wr(cmu_disp0, DISP0_VCLK0_USER, (vclk_was[2] & ~(1u << 27)) | (1u << 12));     /* (still unused) */
	wr(cmu_disp0, DISP0_VCLK0_MUX, vclk_was[3] & ~(7u << 12));                     /* the switch */
	if (!cmu_wait(cmu_disp0, DISP0_VCLK0_MUX_STAT, 0xffu << 12, 1u << 12)) {
		goto back;
	}
	return true;
back:
	say("exynos8890_decon: VCLK0: a clock did not settle (TOP div %x, DISP0 mux status %x): put back\n",
	    rd(cmu_top, TOP_VCLK0_DIV), rd(cmu_disp0, DISP0_VCLK0_MUX_STAT));
	vclk_back();
	return false;
}

/* the DECON's frame-done and FIFO-level status, polled every millisecond (a frame is 16 ms) */
static void
watch(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t said_refreshes = 0, said_underruns = 0, said_starts = 0, sent = 0, vclk_refreshes = 0, vclk_underruns = 0;
	uint64_t started = 0, took = 0, longest = 0, now;
	unsigned last_done = 0;

	for (unsigned tick = 1;; tick++) {
		uint32_t p = rd(decon, INTERRUPT_PENDING) & (INT_VSTATUS | INT_FRAME_DONE | INT_FIFO_LEVEL);

		if (p != 0) {
			wr(decon, INTERRUPT_PENDING, p);
			now = mach_absolute_time();
			if (p & INT_FRAME_DONE) {
				last_done = tick;
				if (vclk == 1) {
					vclk = vclk_42() ? 2 : -1;
					vclk_refreshes = refreshes;
					vclk_underruns = underruns;
					say("exynos8890_decon: VCLK0 %s\n", vclk == 2 ? "at 42 MHz (BUS2 / 16)" : "left at 26 MHz");
				}
				/* the hardware masked itself at the frame's start (auto mask): unmasked now, for the next TE */
				if (!hold) {
					wr(decon, TRIG_CONTROL, (rd(decon, TRIG_CONTROL) & ~TRIG_HW_MASK) | TRIG_AUTO_MASK);
				}
			}
			if ((p & INT_FRAME_DONE) && started != 0) {
				uint64_t t = now - started;     /* (1 ms polling: within a millisecond or two) */

				took += t;
				longest = t > longest ? t : longest;
				sent++;
				started = 0;
			}
			if (p & INT_VSTATUS) {
				starts++;
				started = now;
			}
			refreshes += (p & INT_FRAME_DONE) != 0;
			underruns += (p & INT_FIFO_LEVEL) != 0;
		}
		if (vclk == 2 && (underruns != vclk_underruns || tick - last_done > 200)) {
			vclk_back();
			vclk = -1;
			say("exynos8890_decon: VCLK0 put back to 26 MHz: %s\n", underruns != vclk_underruns ? "an underrun" :
			    "no frame ended in 200 ms");
		} else if (vclk == 2 && refreshes - vclk_refreshes >= 300) {
			vclk = 3;
			say("exynos8890_decon: VCLK0 kept at 42 MHz: 300 frames, no underrun\n");
		}
		if (tick % 5000 == 0 && (refreshes != said_refreshes || underruns != said_underruns)) {
			uint64_t avg_us = 0, max_us = 0;

			absolutetime_to_nanoseconds(sent ? took / sent : 0, &avg_us);
			absolutetime_to_nanoseconds(longest, &max_us);
			say("exynos8890_decon: 5 s: %u frames started, %u sent to the panel, %u underruns; start to done %llu us on "
			    "average, %llu at most (in all %u, %u, %u)\n", starts - said_starts, refreshes - said_refreshes,
			    underruns - said_underruns, avg_us / 1000, max_us / 1000, starts, refreshes, underruns);
			said_refreshes = refreshes;
			said_underruns = underruns;
			said_starts = starts;
			sent = 0;
			took = longest = 0;
		}
		IOSleep(1);
	}
}

static void
point(uint32_t pa)
{
	wr(vpp, VG_BASE_Y, pa);
	wr(vpp, VG_BASE_CB, pa);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(vpp, VG_PINGPONG, PINGPONG_UPDATE);
	flipped = pa;
}

/* DECON_F in command mode and running, one window on, its channel's VPP reading the boot framebuffer at its size. */
int
hakios_scanout_probe(uint64_t fb_phys, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	uint32_t gc, on = 0, ctl = 0;

	if (probed != 0) {
		return probed > 0 ? 0 : -1;
	}
	probed = -1;
	if (fb_phys != EXYNOS8890_MEM_FB_PA || width != 1440 || height != 2560 || rowbytes != 1440 * 4) {
		say("exynos8890_decon: the boot framebuffer is at %#llx, %ux%u, %u bytes a row: not S-Boot's (0xd8000000, "
		    "1440x2560): the DECON is left alone\n", fb_phys, width, height, rowbytes);
		return -1;
	}
	if ((decon = ml_io_map(DECON_PA, 0x1000)) == 0) {
		say("exynos8890_decon: cannot map DECON_F\n");
		return -1;
	}
	gc = rd(decon, GLOBAL_CONTROL);
	for (unsigned w = 0; w < WINDOWS; w++) {
		if (rd(decon, WIN_CONTROL(w)) & WIN_EN) {
			on++;
			win = w;
			ctl = rd(decon, WIN_CONTROL(w));
		}
	}
	say("exynos8890_decon: DECON_F: control %x (%s mode, %s), %u windows on\n", gc, (gc & GC_CMD_MODE) ? "command" : "video",
	    (gc & GC_RUN) ? "running" : "NOT running", on);
	if (on != 1 || !(gc & GC_CMD_MODE) || !(gc & GC_RUN)) {
		say("exynos8890_decon: not one window on a running command-mode DECON: left alone\n");
		return -1;
	}
	boot_type = type_of_channel[WIN_CHMAP(ctl)];
	if ((vpp = ml_io_map(vpp_pa[boot_type], 0x1000)) == 0) {
		say("exynos8890_decon: cannot map %s\n", type_name[boot_type]);
		return -1;
	}
	boot_base = rd(vpp, VG_BASE_Y);
	say("exynos8890_decon: window %u (control %x, %x to %x) on channel %u, %s at %#llx: enable %x in %x src %x img %x "
	    "base %08x/%08x, reading %08x, ping-pong %x\n", win, ctl, rd(decon, WIN_START(win)), rd(decon, WIN_END(win)),
	    WIN_CHMAP(ctl), type_name[boot_type], vpp_pa[boot_type], rd(vpp, VG_ENABLE), rd(vpp, VG_IN_CON),
	    rd(vpp, VG_SRC_SIZE), rd(vpp, VG_IMG_SIZE), boot_base, rd(vpp, VG_BASE_CB), rd(vpp, VG_SHA_BASE_Y),
	    rd(vpp, VG_PINGPONG));
	if (boot_base != (uint32_t)fb_phys || rd(vpp, VG_SHA_BASE_Y) != (uint32_t)fb_phys ||
	    rd(vpp, VG_IMG_SIZE) != (height << 16 | width) || (rd(vpp, VG_SRC_SIZE) & 0x1fff) != width) {
		say("exynos8890_decon: the channel is not reading the boot framebuffer at its size: left alone\n");
		return -1;
	}
	flipped = boot_base;
	boot_win_start = rd(decon, WIN_START(win));
	boot_win_end = rd(decon, WIN_END(win));
	boot_bg = rd(decon, BLENDER_BG_COLOR);
	lcd_w = panel_w = (boot_win_end & 0x1fff) + 1;
	lcd_h = panel_h = (boot_win_end >> 16 & 0x1fff) + 1;
	boot_src = rd(vpp, VG_SRC_SIZE);
	boot_img = rd(vpp, VG_IMG_SIZE);
	boot_scaled = rd(vpp, VG_SCALED_SIZE);
	boot_pixels = rd(vpp, VG_SMART_IF_PIXELS);
	probed = 1;
	say("exynos8890_decon: resources: occupancy %x %x, selection %x %x; background %x\n", rd(decon, 0x10),
	    rd(decon, 0x14), rd(decon, 0x18), rd(decon, 0x1C), boot_bg);
	{
		/* the power domains of the two VPP groups: pd-disp0 (G0 G1 VG0 VG1), pd-disp1 (G2 G3 VGR0 VGR1, the scalers) */
		vm_offset_t pmu = ml_io_map(0x105C4000UL, 0x1000);

		if (pmu != 0) {
			say("exynos8890_decon: PMU pd-disp0 configuration %x status %x, pd-disp1 configuration %x status %x\n",
			    rd(pmu, 0x080), rd(pmu, 0x084), rd(pmu, 0x1A0), rd(pmu, 0x1A4));
		}
	}
	{
		/* S-Boot's link to the panel, only read: what a faster frame would change (Samsung's regs-decon.h, regs-dsim.h) */
		vm_offset_t dsc = ml_io_map(DECON_PA + 0x2000, 0x2000), dsim = ml_io_map(0x13900000UL, 0x1000);

		say("exynos8890_decon: clock control %x, vclk divider %x, eclk divider %x; data path %x, splitter %x size %x, "
		    "frame FIFO %x size %x %x\n", rd(decon, 0xF0), rd(decon, 0xF4), rd(decon, 0xF8), rd(decon, 0x230),
		    rd(decon, 0x240), rd(decon, 0x244), rd(decon, 0x24C), rd(decon, 0x250), rd(decon, 0x254));
		if (dsc != 0) {
			say("exynos8890_decon: DSC0 control %x %x %x, pixels %x %x, pps %08x %08x %08x %08x; DSC1 control %x\n",
			    rd(dsc, 0x00), rd(dsc, 0x04), rd(dsc, 0x08), rd(dsc, 0x0C), rd(dsc, 0x10), rd(dsc, 0x20), rd(dsc, 0x24),
			    rd(dsc, 0x28), rd(dsc, 0x2C), rd(dsc, 0x1000));
		}
		if (dsim != 0) {
			say("exynos8890_decon: DSIM0 link %x clock %x resolution %x config %x, compression %x slices %x %x, "
			    "command config %x, PLL %x %x %x, timing %x %x %x\n", rd(dsim, 0x04), rd(dsim, 0x10), rd(dsim, 0x20),
			    rd(dsim, 0x30), rd(dsim, 0x58), rd(dsim, 0x5C), rd(dsim, 0x60), rd(dsim, 0x78), rd(dsim, 0x94),
			    rd(dsim, 0x98), rd(dsim, 0x9C), rd(dsim, 0xB4), rd(dsim, 0xB8), rd(dsim, 0xBC));
		}
		/* the DECON's ECLK0/VCLK0: CMU_TOP's BUS0/BUS2 PLL, mux and divider, then CMU_DISP0's (S5E8890-cmu.c) */
		uint32_t want = 0;

		cmu_top = ml_io_map(CMU_TOP_PA, 0x1000);
		cmu_disp0 = ml_io_map(CMU_DISP0_PA, 0x1000);
		if (cmu_top != 0 && cmu_disp0 != 0) {
			say("exynos8890_decon: CMU_TOP: BUS0 PLL %x mux %x, BUS2 PLL %x mux %x, SCLK_BUS0 mux %x; eclk0 mux %x div %x, "
			    "vclk0 mux %x div %x gate %x\n", rd(cmu_top, 0x100), rd(cmu_top, 0x200), rd(cmu_top, TOP_BUS2_PLL),
			    rd(cmu_top, 0x208), rd(cmu_top, 0x220), rd(cmu_top, 0x2F4), rd(cmu_top, 0x454), rd(cmu_top, TOP_VCLK0_MUX),
			    rd(cmu_top, TOP_VCLK0_DIV), rd(cmu_top, TOP_VCLK0_GATE));
			say("exynos8890_decon: CMU_DISP0: DISP PLL %x mux %x; eclk0 user %x mux %x div %x, vclk0 user %x mux %x "
			    "div %x\n", rd(cmu_disp0, 0x100), rd(cmu_disp0, DISP0_PLL_MUX), rd(cmu_disp0, 0x20C), rd(cmu_disp0, 0x25C),
			    rd(cmu_disp0, 0x404), rd(cmu_disp0, DISP0_VCLK0_USER), rd(cmu_disp0, DISP0_VCLK0_MUX),
			    rd(cmu_disp0, DISP0_VCLK0_DIV));
			if (PE_parse_boot_argn("decon_vclk42", &want, sizeof(want)) && want == 1) {
				vclk = 1;
			}
		}
	}
	{
		vm_offset_t pins = ml_io_map(TE_PIN_CON_PA & ~0xfffUL, 0x1000);
		volatile uint32_t *con = (volatile uint32_t *)(pins + (TE_PIN_CON_PA & 0xfff));

		if (pins != 0) {
			uint32_t before = *con;

			if (((before >> (4 * TE_PIN)) & 0xf) != TE_FUNCTION) {
				*con = (before & ~(0xfu << (4 * TE_PIN))) | (TE_FUNCTION << (4 * TE_PIN));
				__asm__ volatile ("dsb sy" ::: "memory");
			}
			say("exynos8890_decon: trigger control %x; the TE pin gpb0-1: CON %08x -> %08x (function %u: 2 the DECON's "
			    "TE), level %u\n", rd(decon, TRIG_CONTROL), before, *con, (*con >> (4 * TE_PIN)) & 0xf,
			    (con[1] >> TE_PIN) & 1);
		}
	}
	{
		uint32_t ie = rd(decon, INTERRUPT_ENABLE);
		thread_t thread;

		wr(decon, INTERRUPT_ENABLE, ie | INT_FRAME_DONE | INT_FIFO_LEVEL | INT_EN);
		say("exynos8890_decon: interrupt enable %x -> %x (watched, not taken), pending %x, underrun threshold %x\n", ie,
		    rd(decon, INTERRUPT_ENABLE), rd(decon, INTERRUPT_PENDING), rd(decon, UNDER_RUN_THRESHOLD));
		/* the panel's reads between two frames, the trigger masked (hakios_s6e3ha5_boot_reads: a frame is 24 ms here) */
		wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) | TRIG_HW_MASK);
		wr(decon, INTERRUPT_PENDING, INT_FRAME_DONE);
		for (unsigned ms = 0; ms < 30 && !(rd(decon, INTERRUPT_PENDING) & INT_FRAME_DONE); ms++) {
			IOSleep(1);
		}
		hakios_s6e3ha5_boot_reads();
		wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) & ~TRIG_HW_MASK);
		/* (the watcher unmasks the trigger after every frame: auto mask only once it runs) */
		if (kernel_thread_start_priority((thread_continue_t)watch, NULL, 80, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
			wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) | TRIG_AUTO_MASK);
		}
		say("exynos8890_decon: trigger control %x (bit 12, auto mask: one frame a TE, unmasked at its end)\n",
		    rd(decon, TRIG_CONTROL));
	}
	say("exynos8890_decon: probe: as expected: flips may be asked for (fb_flip=1)\n");
	return 0;
}

/* `count` buffers of `bytes` from the table's scan-out range (sys/hakios.exynos8890_mem.h), 64 KB apart at least */
int
hakios_scanout_memory(uint64_t bytes, unsigned count, uint64_t *phys)
{
	uint64_t step = (bytes + 0xFFFFULL) & ~0xFFFFULL;

	if (probed <= 0 || EXYNOS8890_MEM_DPU_SCAN_PA + step * count > EXYNOS8890_MEM_DPU_SCAN_END) {
		say("exynos8890_decon: memory: %u buffers of %llu bytes not given (probe %d)\n", count, bytes, probed);
		return -1;
	}
	for (unsigned k = 0; k < count; k++) {
		phys[k] = EXYNOS8890_MEM_DPU_SCAN_PA + step * k;
	}
	say("exynos8890_decon: memory: %u buffers of %llu bytes from %#llx\n", count, bytes, EXYNOS8890_MEM_DPU_SCAN_PA);
	return 0;
}

static void
dump_vpp(const char *what, vm_offset_t g)
{
	say("exynos8890_decon: %s: enable %x irq %x in %x out %x src %x offset %x img %x scaled %x ratio %x/%x base %08x "
	    "reading %08x ping-pong %x pixels %x\n", what, rd(g, VG_ENABLE), rd(g, VG_IRQ), rd(g, VG_IN_CON), rd(g, VG_OUT_CON),
	    rd(g, VG_SRC_SIZE), rd(g, VG_SRC_OFFSET), rd(g, VG_IMG_SIZE), rd(g, VG_SCALED_SIZE), rd(g, VG_H_RATIO),
	    rd(g, VG_V_RATIO), rd(g, VG_BASE_Y), rd(g, VG_SHA_BASE_Y), rd(g, VG_PINGPONG), rd(g, VG_SMART_IF_PIXELS));
}


/* what a resolution sets, in Samsung's arithmetic (two encoders, a slice each, compressed to a third) */
static const uint32_t lcd_reg[LCD_REGS] = {
	BLENDER_BG_IMAGE_SIZE, BLENDER_BG_PIXELS, SPLITTER_SIZE_0, SPLITTER_SIZE_1, FRAME_FIFO_0_SIZE_0, FRAME_FIFO_0_SIZE_1,
	FRAME_FIFO_1_SIZE_0, FRAME_FIFO_1_SIZE_1, DISPIF0_SIZE_0, DISPIF0_SIZE_1,
	DSC(0) + DSC_PPS(4), DSC(0) + DSC_PPS(8), DSC(0) + DSC_PPS(12), DSC(0) + DSC_PPS(16), DSC(0) + DSC_PPS(20),
	DSC(0) + DSC_PPS(24), DSC(0) + DSC_PPS(28), DSC(0) + DSC_PPS(32),
	DSC(1) + DSC_PPS(4), DSC(1) + DSC_PPS(8), DSC(1) + DSC_PPS(12), DSC(1) + DSC_PPS(16), DSC(1) + DSC_PPS(20),
	DSC(1) + DSC_PPS(24), DSC(1) + DSC_PPS(28), DSC(1) + DSC_PPS(32),
	DSC(0) + DSC_IN_PIXEL_COUNT, DSC(0) + DSC_COMP_PIXEL_COUNT, DSC(1) + DSC_IN_PIXEL_COUNT, DSC(1) + DSC_COMP_PIXEL_COUNT,
};

static bool
lcd_values(uint32_t w, uint32_t h, uint32_t *v)
{
	const uint8_t *pps = hakios_s6e3ha5_pps(w);
	unsigned n = 0;

	if (pps == NULL) {
		return false;
	}
	v[n++] = h << 16 | w;
	v[n++] = h * w;
	v[n++] = h << 16 | w / 3;
	v[n++] = h * (w / 3);
	for (unsigned fifo = 0; fifo < 2; fifo++) {
		v[n++] = h << 16 | w / 6;
		v[n++] = h * (w / 6);
	}
	v[n++] = h << 16 | w / 3;
	v[n++] = h * (w / 3);
	for (unsigned e = 0; e < 2; e++) {
		for (unsigned b = 4; b < 36; b += 4) {
			uint32_t word = (uint32_t)pps[b] << 24 | (uint32_t)pps[b + 1] << 16 | (uint32_t)pps[b + 2] << 8 | pps[b + 3];

			v[n++] = b == 8 ? (w / 2) << 16 | (word & 0xffff) : word;       /* (an encoder's picture is half the width) */
		}
	}
	for (unsigned e = 0; e < 2; e++) {
		v[n++] = h * (w / 2);
		v[n++] = h * (w / 6);
	}
	return true;
}

/*
 * The DECON (lcd_reg's values `v`), the DSIM and the panel to `w` x `h`, the window to `at` reading `src` x `img` of
 * `pa`: with the DECON idle after a frame and its trigger masked. True when frames end again afterwards.
 */
static bool
lcd_switch(uint32_t w, uint32_t h, const uint32_t *v, const struct hakios_viewport *at, uint32_t src, uint32_t img,
    uint32_t pa)
{
	uint32_t before = refreshes, under = underruns;
	bool told;

	hold = true;
	for (unsigned ms = 0; ms < 100 && refreshes == before; ms++) {
		IOSleep(1);
	}
	/* (a frame ended and the watcher did not unmask: the auto mask holds, and is not to be read; masked by hand too) */
	wr(decon, TRIG_CONTROL, rd(decon, TRIG_CONTROL) | TRIG_HW_MASK);
	if (refreshes == before || (rd(vpp, VG_ENABLE) & VG_OP_STATUS)) {
		/* (written all the same: this is also the way back from a switch after which frames stopped) */
		say("exynos8890_decon: %ux%u: no frame ended in 100 ms, or %s is busy (%x)\n", w, h, type_name[boot_type],
		    rd(vpp, VG_ENABLE));
	}
	for (unsigned k = 0; k < LCD_REGS; k++) {
		wr(decon, lcd_reg[k], v[k]);
	}
	/* (the panel told only when the size it takes changes) */
	told = (w == lcd_w && h == lcd_h) || hakios_s6e3ha5_resolution(w);
	if (told) {
		lcd_w = w;
		lcd_h = h;
	}
	wr(vpp, VG_SRC_SIZE, src);
	wr(vpp, VG_IMG_SIZE, img);
	/* (the two that follow the picture's size only where S-Boot had them follow it) */
	if (boot_scaled == boot_img) {
		wr(vpp, VG_SCALED_SIZE, img);
	}
	if (boot_pixels == panel_w * panel_h) {
		wr(vpp, VG_SMART_IF_PIXELS, at->width * at->height);
	}
	point(pa);              /* (the sizes are taken with the address, at the block's next frame: Samsung's vpp_set_config) */
	wr(decon, WIN_START(win), at->y << 16 | at->x);
	wr(decon, WIN_END(win), (at->y + at->height - 1) << 16 | (at->x + at->width - 1));
	wr(decon, WIN_PIXEL_COUNT(win), at->width * at->height);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_GLOBAL | SHADOW_WIN(win));
	before = refreshes;
	hold = false;
	wr(decon, TRIG_CONTROL, (rd(decon, TRIG_CONTROL) & ~TRIG_HW_MASK) | TRIG_AUTO_MASK);
	for (unsigned ms = 0; ms < 300 && refreshes - before < 3; ms++) {
		IOSleep(1);
	}
	say("exynos8890_decon: %ux%u, the window %ux%u at %u,%u: the panel %s; %u frames ended since, %u underruns; shadow "
	    "request %x\n", w, h, at->width, at->height, at->x, at->y, told ? "told" : "NOT told", refreshes - before,
	    underruns - under, rd(decon, SHADOW_REQ));
	dump_vpp(type_name[boot_type], vpp);
	return told && refreshes - before >= 3 && underruns == under;
}

/* the DECON's, the first encoder's, the DSIM's and the channel's registers as they are, for a switch that goes wrong */
static void
dump(const char *when)
{
	static vm_offset_t dsim;
	const struct { const char *name; vm_offset_t base; uint32_t from, to; } blocks[] = {
		{ "DECON", decon, 0x000, 0x0B0 }, { "DECON", decon, 0x200, 0x260 }, { "DSC0", decon + DSC(0), 0x00, 0x20 },
		{ "DSC1", decon + DSC(1), 0x00, 0x20 },
		{ "DSIM", dsim != 0 ? dsim : (dsim = ml_io_map(0x13900000UL, 0x1000)), 0x00, 0xC0 }, { "VPP", vpp, 0x00, 0x30 },
	};

	for (unsigned b = 0; b < sizeof(blocks) / sizeof(blocks[0]); b++) {
		for (uint32_t off = blocks[b].from; blocks[b].base != 0 && off < blocks[b].to; off += 0x20) {
			const volatile uint32_t *r = (const volatile uint32_t *)(blocks[b].base + off);

			say("exynos8890_decon: %s: %s +%03x: %08x %08x %08x %08x %08x %08x %08x %08x\n", when, blocks[b].name, off, r[0],
			    r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
		}
	}
}

/* mode 2: the DECON sends 1080 x 1920 and the panel enlarges it; the picture, not scaled, in the middle */
static int
attach_panel_scaler(uint32_t pa, uint32_t width, uint32_t height, uint32_t rowbytes)
{
	const struct hakios_viewport full = { 0, 0, panel_w, panel_h };
	uint32_t expected[LCD_REGS], fhd[LCD_REGS];
	bool as_expected = true;

	if (width > 1080 || height > 1920 || (rowbytes & 3) || rowbytes / 4 < width || rowbytes / 4 > 0x1fff ||
	    panel_w != 1440 || panel_h != 2560 || !lcd_values(panel_w, panel_h, expected) || !lcd_values(1080, 1920, fhd)) {
		say("exynos8890_decon: the panel's scaler cannot take %ux%u (%u bytes a row) on %ux%u, or no panel driver "
		    "(panel=1)\n", width, height, rowbytes, panel_w, panel_h);
		return -1;
	}
	for (unsigned k = 0; k < LCD_REGS; k++) {
		/*
		 * (but for the decoder's initial delay, bytes 18-19, which an encoder does not use: S-Boot leaves 0x1b4, worked
		 * out from half a slice, where Samsung's kernel writes the table's 0x268)
		 */
		uint32_t mask = (lcd_reg[k] & 0xfff) == DSC_PPS(16) ? 0xffff0000u : ~0u;

		lcd_boot[k] = rd(decon, lcd_reg[k]);
		if ((lcd_boot[k] & mask) != (expected[k] & mask)) {
			say("exynos8890_decon: +%#x holds %#x, not Samsung's %#x for %ux%u%s\n", lcd_reg[k], lcd_boot[k], expected[k],
			    panel_w, panel_h, k < LCD_REGS_SURE ? "" : ": left as it is");
			as_expected = as_expected && k >= LCD_REGS_SURE;
			fhd[k] = lcd_boot[k];
		}
	}
	dump_vpp(type_name[boot_type], vpp);
	if (rd(decon, WIN_PIXEL_COUNT(win)) != panel_w * panel_h) {
		say("exynos8890_decon: window %u's pixel count is %#x, not its size's\n", win, rd(decon, WIN_PIXEL_COUNT(win)));
		as_expected = false;
	}
	if (!as_expected) {
		say("exynos8890_decon: the DECON's sizes are not as worked out: left alone\n");
		return -1;
	}
	window = (struct hakios_viewport){ (1080 - width) / 2, (1920 - height) / 2, width, height };
	if (!lcd_switch(1080, 1920, fhd, &window, height << 16 | rowbytes / 4, height << 16 | width, pa)) {
		dump("stopped");
		(void)lcd_switch(panel_w, panel_h, lcd_boot, &full, boot_src, boot_img, boot_base);
		dump("back");
		return -1;
	}
	mode = 2;
	return 0;
}

int
hakios_scanout_attach(int want, const uint64_t *phys, unsigned count, uint32_t width, uint32_t height, uint32_t rowbytes,
    unsigned shown)
{
	say("exynos8890_decon: attach: %s asked for, %u buffers of %ux%u (%u bytes a row), %u drawn\n",
	    want == 1 ? "flips (fb_flip)" : want == 2 ? "the panel's scaler (fb_dpp)" : "nothing", count, width, height, rowbytes,
	    shown);
	if (probed <= 0 || mode != 0 || count < 2 || count > 4 || shown >= count || (want != 1 && want != 2) ||
	    (want == 1 && (width != (rd(vpp, VG_IMG_SIZE) & 0x1fff) || rowbytes != (rd(vpp, VG_SRC_SIZE) & 0x1fff) * 4))) {
		say("exynos8890_decon: attach: refused (probe %d, mode %d, buffers %u, drawn %u, %u wide, %u a row)\n", probed,
		    mode, count, shown, width, rowbytes);
		return -1;
	}
	for (unsigned k = 0; k < count; k++) {
		if (phys[k] >> 32 || (phys[k] & 0xff)) {
			say("exynos8890_decon: buffer %u at %#llx: the VPP takes aligned 32-bit addresses\n", k, phys[k]);
			return -1;
		}
		buffers[k] = phys[k];
	}
	nbuffers = count;
	if (want == 2) {
		if (attach_panel_scaler((uint32_t)phys[shown], width, height, rowbytes) != 0) {
			nbuffers = 0;
			say("exynos8890_decon: attach: the panel's scaler not taken: the boot window and buffer stay\n");
			return -1;
		}
		say("exynos8890_decon: the panel enlarges 1080x1920, %ux%u in its middle; flips between %u buffers\n", width, height,
		    count);
		return 0;
	}
	mode = 1;
	say("exynos8890_decon: flips on %s between %u buffers (%08llx ...), each taken at a refresh\n", type_name[boot_type],
	    count, phys[0]);
	if (buffers[shown] != boot_base) {
		hakios_scanout_flip(shown);
	}
	return 0;
}

int
hakios_scanout_flip(unsigned index)
{
	if (mode == 0 || index >= nbuffers) {
		return -1;
	}
	point((uint32_t)buffers[index]);
	return 0;
}

int
hakios_scanout_latched(void)
{
	return probed > 0 && (rd(vpp, VG_PINGPONG) & PINGPONG_UPDATE) == 0 && rd(vpp, VG_SHA_BASE_Y) == flipped;
}

uint32_t
hakios_scanout_refreshes(void)
{
	return refreshes;
}

void
hakios_scanout_detach(const char *why)
{
	if (probed <= 0 || mode == 0) {
		return;
	}
	say("exynos8890_decon: back to the boot window and buffer (%s)\n", why);
	if (mode == 2) {
		const struct hakios_viewport full = { 0, 0, panel_w, panel_h };

		(void)lcd_switch(panel_w, panel_h, lcd_boot, &full, boot_src, boot_img, boot_base);
	}
	point(boot_base);
	wr(decon, SHADOW_REQ, rd(decon, SHADOW_REQ) | SHADOW_GLOBAL | SHADOW_WIN(win));
	for (unsigned ms = 0; ms < 100 && (!hakios_scanout_latched() || (rd(decon, SHADOW_REQ) & (SHADOW_GLOBAL |
	    SHADOW_WIN(win)))); ms++) {
		IOSleep(1);
	}
	mode = 0;
}

int
hakios_scanout_viewport(struct hakios_viewport *v, uint32_t *pw, uint32_t *ph)
{
	if (mode != 2) {
		return 0;
	}
	*v = window;
	*pw = lcd_w;
	*ph = lcd_h;
	return 1;
}

/* fb_zc's parts: not on this SoC yet */
int hakios_scanout_carve(__unused uint64_t bytes, __unused uint64_t *phys) { return -1; }
void hakios_scanout_uncarve(__unused uint64_t phys) {}
int hakios_scanout_carved(__unused uint64_t phys) { return 0; }
int hakios_scanout_show(__unused uint64_t phys, __unused uint32_t rowbytes) { return -1; }
