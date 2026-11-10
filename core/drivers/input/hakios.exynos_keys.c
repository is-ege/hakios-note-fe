/*
 * A Samsung phone's keys, pins of its SoC's ALIVE GPIO block, as a source of button records for /dev/hakios-touch
 * (sys/hakios.input.h). Only with boot-arg exynos_keys=1; the pins are the board's (boards[] below,
 * exynos_keys_board=, starlte by default):
 *   starlte (Galaxy S9, Exynos 9810, pinctrl@14050000): volume up gpa0-3, volume down gpa0-4, Bixby gpa0-6, power gpa2-4
 *   gracerlte (Galaxy Note FE, Exynos 8890, pinctrl@10580000): volume up gpa0-3, volume down gpa0-4, Home gpa0-5,
 *     power gpa2-4
 * Each is pulled up and pulled to ground by its key, so a pin reads 0 while its key is held. A bank of that block
 * is CON (+0x00, a nibble per pin: 0 input, 0xF its wakeup interrupt), DAT (+0x04, a bit per pin), PUD (+0x08),
 * DRV (+0x0C), banks 0x20 apart (the 9810's gpa0 at 0x20, the 8890's at 0x00). DAT says the pin's level whether
 * the pin is an input or an interrupt (Android's own driver reads it with the pins set to 0xF).
 * Sources: the phones' device trees (gpio_keys, the gpa banks, the key-* pin settings), Linux's
 * drivers/pinctrl/samsung/pinctrl-exynos.c (the ALIVE banks) and Project Silicium's starlte UEFI, which reads the
 * same four bits the same way (KeypadDeviceLib.c) and sets nothing up first.
 *
 * THIS DRIVER ONLY READS. The firmware has left the pins as Android wants them; a pin found set to anything but
 * an input or its interrupt is not taken for a key, and said so.
 *
 * What a press is: a thread reads the two DAT registers 100 times a second, and a key has changed when two looks
 * in a row say so (Android debounces for 5 ms). A key that reads as held before it has ever been seen released
 * is not believed until it has been.
 *
 * The mapping: the iPhone X, whose user space runs here, has a side button and two volume keys. Power is the
 * side button (HAKIOS_BUTTON_LOCK); the S9's Bixby, where the iPhone has nothing, and the Note FE's Home key are the
 * home button (HAKIOS_BUTTON_HOME).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

/* (every line to the loader's log as well, as the other drivers do: see hakios.buttons.c) */
extern void loader_log_line(const char *line);

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

#define BANK_CON        0x00
#define BANK_DAT        0x04
#define BANK_PUD        0x08

#define POLL_MS         10
#define MINUTE_TICKS    (60 * 1000 / POLL_MS)
#define NKEYS           4

struct key {
	const char *name;
	uint32_t bank, pin;             /* the bank's offset in the ALIVE block */
	uint32_t button;                /* HAKIOS_BUTTON_* */
	bool usable;                    /* the pin is an input or its interrupt */
	bool live;                      /* seen released: what it says is believed */
	bool held;                      /* as last reported */
	bool last;                      /* the previous look */
	uint32_t presses;
};

static struct board {
	const char *name;
	uint64_t alive_pa;
	struct key keys[NKEYS];
} boards[] = {
	{ "starlte", 0x14050000, {
		{ "power", 0x060, 4, HAKIOS_BUTTON_LOCK },
		{ "volume-up", 0x020, 3, HAKIOS_BUTTON_VOLUME_UP },
		{ "volume-down", 0x020, 4, HAKIOS_BUTTON_VOLUME_DOWN },
		{ "bixby", 0x020, 6, HAKIOS_BUTTON_HOME },
	} },
	{ "gracerlte", 0x10580000, {
		{ "power", 0x040, 4, HAKIOS_BUTTON_LOCK },
		{ "volume-up", 0x000, 3, HAKIOS_BUTTON_VOLUME_UP },
		{ "volume-down", 0x000, 4, HAKIOS_BUTTON_VOLUME_DOWN },
		{ "home", 0x000, 5, HAKIOS_BUTTON_HOME },
	} },
};
static struct board *board;
static struct key *keys;

static vm_offset_t alive;

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(alive + off);
}

/* held: the pin at 0 */
static bool
key_held(const struct key *k)
{
	return ((rd(k->bank + BANK_DAT) >> k->pin) & 1) == 0;
}

static void
keys_poll(void)
{
	for (unsigned i = 0; i < NKEYS; i++) {
		struct key *k = &keys[i];
		bool now = key_held(k);

		if (!k->usable) {
			continue;
		}
		if (now != k->last) {
			k->last = now;          /* one look: wait for a second that agrees */
			continue;
		}
		if (!k->live) {
			if (!now) {
				k->live = true;
				say("exynos_keys: %s seen released: watching it\n", k->name);
			}
			continue;
		}
		if (now != k->held) {
			k->held = now;
			if (now) {
				k->presses++;
			}
			hakios_touch_post(now ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, k->button, 0, 0);
			say("exynos_keys: %s %s\n", k->name, now ? "down" : "up");
		}
	}
}

static void
keys_thread(__unused void *arg, __unused wait_result_t wr)
{
	alive = ml_io_map((vm_offset_t)board->alive_pa, 0x1000);
	if (alive == 0) {
		say("exynos_keys: cannot map the ALIVE GPIO block at 0x%llx\n", board->alive_pa);
		thread_terminate(current_thread());
		return;
	}
	for (unsigned i = 0; i < NKEYS; i++) {
		struct key *k = &keys[i];
		uint32_t con = (rd(k->bank + BANK_CON) >> (4 * k->pin)) & 0xf;

		k->usable = con == 0x0 || con == 0xf;
		k->last = key_held(k);
		k->live = k->usable && !k->last;
		/* (the bank's PUD only said, raw -- its field width differs between SoCs: a key that relies on the SoC's
		 * pull-up and is left without it may read at random) */
		say("exynos_keys: %s: bank 0x%x pin %u, CON %x (%s), bank PUD %x, %s\n", k->name, k->bank, k->pin, con,
		    con == 0 ? "input" : con == 0xf ? "interrupt" : "SOMETHING ELSE: not a key here, left alone",
		    rd(k->bank + BANK_PUD),
		    k->last ? "reads as held: believed once it has been seen released" : "released");
	}
	say("exynos_keys: %s: watching every %u ms: power is the side button, %s the home button\n", board->name, POLL_MS,
	    keys[3].name);
	for (unsigned tick = 1;; tick++) {
		keys_poll();
		if (tick % MINUTE_TICKS == 0) {
			say("exynos_keys: a minute: presses %s %u, %s %u, %s %u, %s %u\n", keys[0].name, keys[0].presses,
			    keys[1].name, keys[1].presses, keys[2].name, keys[2].presses, keys[3].name, keys[3].presses);
		}
		delay_for_interval(POLL_MS, NSEC_PER_MSEC);
	}
}

void
hakios_exynos_keys_init(void)
{
	uint32_t enabled = 0;
	char name[16] = "starlte";
	thread_t thread;

	if (!PE_parse_boot_argn("exynos_keys", &enabled, sizeof(enabled)) || enabled == 0) {
		return;
	}
	/* exynos_keys_board=NAME: the board's pins (boards[]: starlte, gracerlte) */
	(void)PE_parse_boot_arg_str("exynos_keys_board", name, sizeof(name));
	for (unsigned i = 0; i < sizeof(boards) / sizeof(boards[0]); i++) {
		if (strcmp(name, boards[i].name) == 0) {
			board = &boards[i];
		}
	}
	if (board == NULL) {
		say("exynos_keys: exynos_keys_board=%s: no such board (starlte, gracerlte)\n", name);
		return;
	}
	keys = board->keys;
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
