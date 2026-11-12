/*
 * The Galaxy S9's real-time clock, the RTC block of its S2MPS18 PMIC: the chip's side of hakios.pmic_rtc.c (which
 * decides whether the calendar starts at its time, rtc_pmic=1). READ ONLY: nothing is written to the PMIC. Its read seen
 * working on the phone, 2 November.
 *
 * The way. The S2MPS18 is on SPEEDY, which the APM owns: the AP reaches its registers only by asking the APM through
 * ACPM (drivers/core/hakios.exynos_acpm.c, channel 2, the same path the GPU's and the touch screen's supplies take, and
 * Samsung's kernel's own s2mps18 driver: every one of its reads and writes is an ACPM request). The RTC is the PMIC's
 * block 2 ("type" 2; Samsung's kernel calls it the i2c address 0x02 of the PMIC). Its registers, as Samsung's kernel
 * has them for the S9 (CONFIG_RTC_DRV_S2MP not set, so the layout with milliseconds):
 *   0x00 CTRL     bit 0 BCD (0: binary), bit 1 24-hour mode; Linux sets 0x02 (binary, 24 h) when it finds the block reset
 *   0x01 WTSR_SMPL                0x02 UPDATE (RUDR bit 0, the latch of the counters into the registers read here)
 *   0x03 CAPSEL   its top four bits are all set once Linux has set the clock up: "CTRL bit 1 and CAPSEL[7:4] = 0xF, or
 *                 the RTC was reset" is Linux's own test
 *   0x04 MSEC     0x05 SEC [6:0]  0x06 MIN [6:0]  0x07 HOUR [4:0] (bit 6: PM, said, not used in 24-hour mode)
 *   0x08 WEEK (one bit a day)     0x09 DAY [4:0]  0x0A MONTH [3:0] (1-12)  0x0B YEAR [6:0] (since 2000)
 * all binary.
 *
 * What is read is what was last latched: S-Boot's read at boot (hakios.pmic_rtc.c). Linux latches (writes RUDR to
 * UPDATE) before each read; that is a write, and this driver does not write. The clock is taken as set up when its CTRL
 * and CAPSEL are as Linux leaves them. On the S9's runs so far (runs/auto-210855, the phone's last_kmsg) the clock said
 * 2018-01-01: reset and never set, because nothing sets it from the network.
 *
 * Log (loader log), with rtc_pmic=1:
 *   rtc: S2MPS18 RTC (ACPM, block 2), read only
 *   rtc: CTRL 0x02 UPDATE 0x.. CAPSEL 0xf. MSEC 0x.. SEC 0x.. MIN 0x.. HOUR 0x.. WEEK 0x.. DAY 0x.. MONTH 0x.. YEAR 0x..
 * then hakios.pmic_rtc.c's lines, or "rtc: not used (ACPM not there) ...".
 *
 * Sources (facts only, nothing copied; GPL-2.0): Samsung's kernel for the S9 (lineage-24.0, android_kernel_samsung_exynos9810):
 * include/linux/mfd/samsung/rtc-s2mp.h (the register layout without CONFIG_RTC_DRV_S2MP, the CTRL bits),
 * drivers/rtc/rtc-s2mps18.c (s2m_data_to_tm: the fields' masks, the year since 2000, the month 1-12; s2m_rtc_init_reg:
 * the reset test; s2m_rtc_update: RUDR), drivers/mfd/s2mps18_core.c (I2C_ADDR_RTC 0x02, every access an ACPM request),
 * arch/arm64/configs/exynos9810-starlte_defconfig (CONFIG_RTC_DRV_S2MPS18=y, no CONFIG_RTC_DRV_S2MP); S-Boot's and
 * Linux's log on the phone (runs/auto-210855/last_kmsg).
 *
 * Status: compiled; NOT RUN on the phone. The ACPM path is the one the GPU and touch drivers have run on the phone; the
 * RTC block's registers have not been read by this kernel before.
 */
#include <libkern/libkern.h>
#include <sys/hakios.exynos_acpm.h>
#include "hakios.pmic_rtc.h"

#define RTC_CTRL        0x00
#define RTC_UPDATE      0x02
#define RTC_CAPSEL      0x03
#define RTC_MSEC        0x04
#define RTC_SEC         0x05
#define RTC_MIN         0x06
#define RTC_HOUR        0x07
#define RTC_WEEK        0x08
#define RTC_DAY         0x09
#define RTC_MONTH       0x0A
#define RTC_YEAR        0x0B
#define RTC_REGS        12              /* 0x00 to 0x0B */

#define CTRL_BCD        0x01
#define CTRL_24H        0x02

const char *
hakios_rtc_chip_read(int (*say)(const char *, ...), struct hakios_rtc_clock *clock)
{
	uint8_t r[RTC_REGS];

	say("rtc: S2MPS18 RTC (ACPM, block 2), read only\n");
	if (!hakios_acpm_init(say)) {
		return "ACPM not there";
	}
	for (uint32_t i = 0; i < RTC_REGS; i++) {
		if (hakios_acpm_pmic_read(HAKIOS_ACPM_PMIC_RTC, i, &r[i]) != 0) {
			return "a register could not be read";
		}
	}
	say("rtc: CTRL 0x%02x UPDATE 0x%02x CAPSEL 0x%02x MSEC 0x%02x SEC 0x%02x MIN 0x%02x HOUR 0x%02x WEEK 0x%02x "
	    "DAY 0x%02x MONTH 0x%02x YEAR 0x%02x\n", r[RTC_CTRL], r[RTC_UPDATE], r[RTC_CAPSEL], r[RTC_MSEC], r[RTC_SEC],
	    r[RTC_MIN], r[RTC_HOUR], r[RTC_WEEK], r[RTC_DAY], r[RTC_MONTH], r[RTC_YEAR]);
	if ((r[RTC_CTRL] & (CTRL_BCD | CTRL_24H)) != CTRL_24H) {
		return "CTRL is not binary, 24-hour: not set up";
	}
	if ((r[RTC_CAPSEL] & 0xf0) != 0xf0) {
		return "CAPSEL[7:4] is not 0xF: the clock was reset";
	}
	clock->sec = r[RTC_SEC] & 0x7f;
	clock->min = r[RTC_MIN] & 0x7f;
	clock->hour = r[RTC_HOUR] & 0x1f;
	clock->day = r[RTC_DAY] & 0x1f;
	clock->month = r[RTC_MONTH] & 0x0f;
	clock->year = 2000 + (r[RTC_YEAR] & 0x7f);
	return NULL;
}
