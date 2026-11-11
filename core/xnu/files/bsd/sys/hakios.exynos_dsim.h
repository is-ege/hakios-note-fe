/*
 * The Exynos DSIM's command FIFO: DCS commands sent to the panel, and its registers read, while the DECON keeps showing
 * what the boot loader set up. The same block in the Exynos 9810 and 8890, its registers at other offsets; the bits are
 * the same. For a panel's brightness driver, which includes this once: hakios.s6e3ha8.c (the Galaxy S9),
 * devices/gracerlte/drivers/gpu/hakios.s6e3ha5.c. The includer has say() declared before, and defines dsim_soc after.
 */
#ifndef _HAKIOS_EXYNOS_DSIM_H_
#define _HAKIOS_EXYNOS_DSIM_H_

struct hakios_dsim {
	const char *who;                /* the log's prefix: "s6e3ha8" */
	uint64_t pa;
	uint16_t intsrc, pkthdr, payload, rxfifo, fifoctrl;
};

static const struct hakios_dsim dsim_soc;
static vm_offset_t dsim;                /* dsim_soc.pa, mapped by the includer */
static uint32_t dsim_read_failures;

#define DSIM_INTSRC   (dsim_soc.intsrc)
#define  SFR_PH_EMPTY   (1u << 28)
#define  LPRX_TIMEOUT   (1u << 21)
#define  BTA_TIMEOUT    (1u << 20)
#define  RX_DATA_DONE   (1u << 18)
#define DSIM_PKTHDR   (dsim_soc.pkthdr)
#define  PKTHDR_BTA     (1u << 24)      /* turn the bus round after this packet: the panel answers */
#define DSIM_PAYLOAD  (dsim_soc.payload)
#define DSIM_RXFIFO   (dsim_soc.rxfifo)
#define DSIM_FIFOCTRL (dsim_soc.fifoctrl)
#define  EMPTY_RX       (1u << 12)
#define  FULL_PH        (1u << 11)
#define  EMPTY_PH       (1u << 10)      /* (the header FIFO empty: Samsung takes that for "sent" as well) */
#define  FULL_PL        (1u << 9)
#define RX_WORDS        64              /* Samsung's DSIM_RX_FIFO_MAX_DEPTH */

#define DCS_SHORT       0x05
#define DCS_READ        0x06
#define MAX_RETURN      0x37            /* set maximum return packet size */
#define DCS_LONG        0x39
#define RX_ACK_ERROR    0x02
#define RX_EOT          0x08
#define RX_GEN_SHORT_1  0x11
#define RX_GEN_SHORT_2  0x12
#define RX_GEN_LONG     0x1A
#define RX_DCS_LONG     0x1C
#define RX_DCS_SHORT_1  0x21
#define RX_DCS_SHORT_2  0x22
#define RX_ERRORS       0x3F3F          /* MIPI_DSI_ERR_BIT_MASK */

/* a moment `us` microseconds from now, by the clock: a loop bounded by it spins, where delay() would sleep a tick */
static uint64_t
deadline_us(uint64_t us)
{
	uint64_t interval;

	nanoseconds_to_absolutetime(us * NSEC_PER_USEC, &interval);
	return mach_absolute_time() + interval;
}

static inline uint32_t
rd(uint32_t off)
{
	return *(volatile uint32_t *)(dsim + off);
}

static inline void
wr(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(dsim + off) = v;
}

/* one packet, its payload (if any) and then its header: true once the DSIM says the header has gone out */
static bool
dsim_packet(uint32_t header, const uint8_t *payload, uint32_t len)
{
	uint64_t deadline = deadline_us(50000);

	while ((rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & (FULL_PH | FULL_PL)) {
		return false;
	}
	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	for (uint32_t i = 0; i < len; i += 4) {
		uint32_t word = 0;

		for (uint32_t j = 0; j < 4 && i + j < len; j++) {
			word |= (uint32_t)payload[i + j] << (8 * j);
		}
		wr(DSIM_PAYLOAD, word);
	}
	wr(DSIM_PKTHDR, header);
	for (deadline = deadline_us(50000); !(rd(DSIM_INTSRC) & SFR_PH_EMPTY) && !(rd(DSIM_FIFOCTRL) & EMPTY_PH) &&
	    mach_absolute_time() < deadline;) {
	}
	bool sent = (rd(DSIM_INTSRC) & SFR_PH_EMPTY) || (rd(DSIM_FIFOCTRL) & EMPTY_PH);

	wr(DSIM_INTSRC, SFR_PH_EMPTY);
	return sent;
}

/* one DCS command: a byte alone short, anything longer long */
static bool
dsim_send(const uint8_t *data, uint32_t len)
{
	if (len == 1) {
		return dsim_packet(DCS_SHORT | (uint32_t)data[0] << 8, NULL, 0);
	}
	return dsim_packet(DCS_LONG | (len & 0xff) << 8 | (len >> 8) << 16, data, len);
}

/* the next word of the RX FIFO, waiting at most `us' for one */
static bool
rx_pop(uint32_t *word, uint64_t us)
{
	uint64_t deadline = deadline_us(us);

	while ((rd(DSIM_FIFOCTRL) & EMPTY_RX) && mach_absolute_time() < deadline) {
	}
	if (rd(DSIM_FIFOCTRL) & EMPTY_RX) {
		return false;
	}
	*word = rd(DSIM_RXFIFO);
	return true;
}

/* a DCS read of `len' bytes (1-40) of register `reg' (dsim_read_data): true when exactly that many came */
static bool
dsim_read(uint8_t reg, uint8_t *buf, uint32_t len)
{
	uint32_t word = 0, first = 0, got = 0, words = 0, stale = 0, intsrc;
	const char *why = NULL;
	uint64_t deadline;

	while (!(rd(DSIM_FIFOCTRL) & EMPTY_RX) && stale < RX_WORDS) {
		(void)rd(DSIM_RXFIFO);
		stale++;
	}
	wr(DSIM_INTSRC, RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT);
	if (!dsim_packet(MAX_RETURN | len << 8, NULL, 0) ||
	    !dsim_packet(DCS_READ | (uint32_t)reg << 8 | PKTHDR_BTA, NULL, 0)) {
		why = "the request was not sent";
		goto out;
	}
	for (deadline = deadline_us(100000); !(rd(DSIM_INTSRC) & (RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT)) &&
	    mach_absolute_time() < deadline;) {
	}
	/* (an answer in the FIFO is read even without RX data done: whether INTSRC shows that bit while it is masked is
	 * for the phone to say) */
	while (why == NULL && words < RX_WORDS && rx_pop(&word, words == 0 ? 1000 : 0)) {
		uint32_t size;

		first = words++ == 0 ? word : first;
		switch (word & 0xff) {
		case RX_ACK_ERROR:
			if ((word >> 8) & RX_ERRORS) {
				why = "the panel reported an error";
			}
			break;
		case RX_EOT:
			break;
		case RX_DCS_SHORT_1:
		case RX_GEN_SHORT_1:
		case RX_DCS_SHORT_2:
		case RX_GEN_SHORT_2:
			size = ((word & 0xff) == RX_DCS_SHORT_1 || (word & 0xff) == RX_GEN_SHORT_1) ? 1 : 2;
			for (uint32_t i = 0; i < size && i < len; i++) {
				buf[i] = (uint8_t)(word >> (8 + 8 * i));
			}
			got = size;
			break;
		case RX_DCS_LONG:
		case RX_GEN_LONG:
			size = (word >> 8) & 0xffff;
			for (uint32_t i = 0; i < size && why == NULL; i += 4) {
				uint32_t data;

				if (words++ >= RX_WORDS || !rx_pop(&data, 1000)) {
					why = "a long answer cut short";
					break;
				}
				for (uint32_t j = 0; j < 4 && i + j < size && i + j < len; j++) {
					buf[i + j] = (uint8_t)(data >> (8 * j));
				}
			}
			got = size;
			break;
		default:
			why = "a packet of unknown type";
			break;
		}
	}
	if (why == NULL && got != len) {
		why = words == 0 ? "no answer" : "an answer of another length";
	}
out:
	intsrc = rd(DSIM_INTSRC);
	wr(DSIM_INTSRC, RX_DATA_DONE | BTA_TIMEOUT | LPRX_TIMEOUT);
	if (why != NULL && dsim_read_failures++ < 8) {
		say("%s: read %02X (%u bytes): %s (got %u in %u words, first %08x; INTSRC %x, FIFOCTRL %x%s)\n", dsim_soc.who, reg,
		    len, why, got, words, first, intsrc, rd(DSIM_FIFOCTRL), stale ? ", stale words read out first" : "");
	}
	return why == NULL;
}

#endif
