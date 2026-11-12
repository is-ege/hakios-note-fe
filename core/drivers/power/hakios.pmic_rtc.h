/*
 * A phone's real-time clock, read once at start-up (hakios.pmic_rtc.c), with the chip's side a glue: the Galaxy S9's
 * S2MPS18 through ACPM (hakios.s2mps18_rtc.c), the Galaxy Note FE's S2MPS16 on its HSI2C (devices/gracerlte:
 * hakios.s2mps16.c). Each reads the clock as S-Boot last latched it (S-Boot reads it on every boot) and writes nothing.
 */
#ifndef _HAKIOS_PMIC_RTC_H_
#define _HAKIOS_PMIC_RTC_H_

#include <stdint.h>

struct hakios_rtc_clock {
	uint32_t year, month, day, hour, min, sec;      /* UTC, month 1-12 */
};

/* NULL and the clock's fields, or why it cannot be used; its registers said in the log */
const char *hakios_rtc_chip_read(int (*say)(const char *, ...), struct hakios_rtc_clock *clock);

#endif
