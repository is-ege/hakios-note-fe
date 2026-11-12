/*
 * A phone's real-time clock, read once at start-up so that the kernel's calendar starts at the phone's own time instead
 * of 1970 or the time the image was built (epoch=). The chip's side is a glue (hakios.pmic_rtc.h): it reads the clock's
 * registers as S-Boot last latched them, writes nothing, and says whether the clock was set up.
 *
 * S-Boot reads the clock on every boot (the S9's log: "RTC TIME: 2018-01-01 14:54:18(0x02)PM" at 1.26 s of its timer;
 * the Note FE's S-Boot has the same pmic_rtc_read_time), so the registers hold the time of that read, a little after the
 * timer started. When the kernel's uptime at this read is between 2 s and 10 minutes (the timer counted from S-Boot's
 * start, not from an earlier boot) that uptime less one second is added; otherwise nothing is.
 *
 * Which wins. The clock's time when the chip says it was set up, every field is in range, the year is 2024 to 2099, and
 * it is not earlier than epoch= (a clock behind the image's time is wrong). Otherwise epoch= (or the loader's
 * boot-epoch), as before. Setting the clock from iOS's time would be a write, and is not here.
 *
 * Why it matters on the Note FE (12 November 2026): /private/var is kept between boots, and iOS's power log (aggregated)
 * held a core at 100% for the whole run once the calendar had gone from 1970 to 2026 (Wi-Fi's time) and back at the
 * next boot.
 *
 * Boot-args:
 *   rtc_pmic=1   read the clock at start-up (the kernel's first ask for the time of day), log it, use it when valid
 * Log (loader log), with rtc_pmic=1: the chip's registers, then
 *   rtc: the clock says 2026-11-02 08:49:03 (1793609343); uptime 9 s: +8 s since S-Boot read it: 1793609351
 *   rtc: using the clock: 2026-11-02 08:49:11 UTC (epoch= 1793600000 was 9351 s behind)
 * or "rtc: not used (<why>); the calendar starts at epoch= ...".
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "hakios.pmic_rtc.h"

extern void loader_log_line(const char *line);

static int rtc_say(const char *format, ...) __printflike(1, 2);

static int
rtc_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
	return printf("%s", line);
}

/* seconds since 1970 of a UTC date (year 1970 and on) */
static uint64_t
rtc_days_secs(uint32_t year, uint32_t month, uint32_t day, uint32_t hour, uint32_t min, uint32_t sec)
{
	static const uint16_t before[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	uint64_t days = (uint64_t)(year - 1970) * 365 + (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400 +
	    before[month - 1] + (day - 1);
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	if (month > 2 && leap) {
		days++;
	}
	return days * 86400 + (uint64_t)hour * 3600 + (uint64_t)min * 60 + sec;
}

/* the date of seconds since 1970, for the log */
static void
rtc_date(uint64_t t, uint32_t *y, uint32_t *mo, uint32_t *d, uint32_t *h, uint32_t *mi, uint32_t *s)
{
	uint64_t days = t / 86400, rest = t % 86400;
	uint32_t year = 1970;

	for (;;) {
		uint32_t len = ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) ? 366 : 365;

		if (days < len) {
			break;
		}
		days -= len;
		year++;
	}
	*y = year;
	*mo = 1;
	for (; *mo < 12; (*mo)++) {
		uint64_t next = rtc_days_secs(year, *mo + 1, 1, 0, 0, 0) - rtc_days_secs(year, *mo, 1, 0, 0, 0);

		if (days < next / 86400) {
			break;
		}
		days -= next / 86400;
	}
	*d = (uint32_t)days + 1;
	*h = (uint32_t)(rest / 3600);
	*mi = (uint32_t)(rest / 60 % 60);
	*s = (uint32_t)(rest % 60);
}

/*
 * The kernel's time of day at start-up (GenericARMPE.cpp, findBootEpoch): `epoch` is what it has otherwise (epoch=, or
 * the loader's boot-epoch; 0: none). The answer is the clock's time when rtc_pmic=1 and it is valid, else `epoch`.
 */
uint64_t hakios_rtc_boot_time(uint64_t epoch);

uint64_t
hakios_rtc_boot_time(uint64_t epoch)
{
	struct hakios_rtc_clock c;
	uint32_t on = 0, y, mo, d, h, mi, s;
	uint64_t clock, now, up_ns;
	const char *why;

	if (!PE_parse_boot_argn("rtc_pmic", &on, sizeof(on)) || on == 0) {
		return epoch;
	}
	why = hakios_rtc_chip_read(rtc_say, &c);
	absolutetime_to_nanoseconds(mach_absolute_time(), &up_ns);
	if (why == NULL && (c.sec > 59 || c.min > 59 || c.hour > 23 || c.day < 1 || c.day > 31 || c.month < 1 || c.month > 12)) {
		why = "a field out of range";
	}
	if (why != NULL) {
		rtc_say("rtc: not used (%s); the calendar starts at epoch= %llu\n", why, epoch);
		return epoch;
	}
	clock = rtc_days_secs(c.year, c.month, c.day, c.hour, c.min, c.sec);
	now = clock;
	if (up_ns >= 2 * NSEC_PER_SEC && up_ns <= 600 * NSEC_PER_SEC) {
		now += up_ns / NSEC_PER_SEC - 1;
	}
	rtc_say("rtc: the clock says %u-%02u-%02u %02u:%02u:%02u (%llu); uptime %llu s: +%llu s since S-Boot read it: %llu\n",
	    c.year, c.month, c.day, c.hour, c.min, c.sec, clock, up_ns / NSEC_PER_SEC, now - clock, now);
	if (c.year < 2024 || c.year > 2099) {
		why = "the year is not 2024-2099: the clock was never set";
	} else if (epoch != 0 && now < epoch) {
		why = "it is behind epoch=, the time the image was made";
	}
	if (why != NULL) {
		rtc_say("rtc: not used (%s); the calendar starts at epoch= %llu\n", why, epoch);
		return epoch;
	}
	rtc_date(now, &y, &mo, &d, &h, &mi, &s);
	rtc_say("rtc: using the clock: %u-%02u-%02u %02u:%02u:%02u UTC (epoch= %llu was %llu s behind)\n", y, mo, d, h, mi, s,
	    epoch, epoch != 0 ? now - epoch : 0);
	return now;
}
