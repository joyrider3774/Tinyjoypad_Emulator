#include "tj_board.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avr_adc.h"
#include "avr_ioport.h"
#include "avr_usi.h"
#include "sim_hex.h"
#include "sim_io.h"
#include "sim_irq.h"

/* Pin assignments on PORTB, from the TinyJoypad rev2 schematic. */
#define PIN_SDA        0
#define PIN_FIRE       1
#define PIN_SCL        2
#define PIN_UPDOWN     3   /* ADC3 */
#define PIN_SOUND      4
#define PIN_LEFTRIGHT  5   /* ADC0 */

#define ADC_CH_LEFTRIGHT 0
#define ADC_CH_UPDOWN    3

#define SSD1306_I2C_ADDRESS 0x3c

/* Board voltage, and the joystick resistor ladder values. */
#define TJ_VCC_MV   3300
#define R_SERIES    22000.0
#define R_BRANCH_A  88000.0   /* buttons 1 (left) and 4 (down) */
#define R_BRANCH_B  33000.0   /* buttons 3 (right) and 2 (up)  */

/* PLLCSR lives at I/O 0x27, i.e. data space 0x47 on the ATtiny85. */
#define REG_PLLCSR  0x47
#define BIT_PLOCK   0
#define BIT_PLLE    1
#define BIT_PCKE    2

/* ------------------------------------------------------------------ */
/* Joystick ladders                                                    */
/* ------------------------------------------------------------------ */

/*
 * Both direction inputs are a 22k pull-up to VCC with two switched resistors
 * to ground.  Solving the divider rather than returning canned numbers means
 * pressing both directions at once produces the same out-of-range value the
 * hardware would, instead of a direction the game never sees on a real unit.
 */
static uint32_t
ladder_millivolts(bool branch_a, bool branch_b)
{
	double g = 0.0;

	if (branch_a)
		g += 1.0 / R_BRANCH_A;
	if (branch_b)
		g += 1.0 / R_BRANCH_B;

	if (g == 0.0)
		return TJ_VCC_MV;               /* idle: pulled up to VCC */

	double r_par = 1.0 / g;
	return (uint32_t)(TJ_VCC_MV * (r_par / (r_par + R_SERIES)) + 0.5);
}

static uint32_t
left_right_mv(const tj_board_t *b)
{
	return ladder_millivolts(b->buttons[TJ_BTN_LEFT], b->buttons[TJ_BTN_RIGHT]);
}

static uint32_t
up_down_mv(const tj_board_t *b)
{
	return ladder_millivolts(b->buttons[TJ_BTN_DOWN], b->buttons[TJ_BTN_UP]);
}

int
tj_board_adc_value(const tj_board_t *b, int channel)
{
	uint32_t mv = (channel == ADC_CH_UPDOWN) ? up_down_mv(b) : left_right_mv(b);
	return (int)((mv * 1023u) / TJ_VCC_MV);
}

/* simavr asks for analog inputs just before each conversion. */
static void
adc_trigger_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	(void)value;
	tj_board_t *b = (tj_board_t *)param;
	avr_irq_t *adc = avr_io_getirq(b->avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_ADC0);

	if (!adc)
		return;
	b->adc_reads++;
	avr_raise_irq(adc + ADC_CH_LEFTRIGHT, left_right_mv(b));
	avr_raise_irq(adc + ADC_CH_UPDOWN, up_down_mv(b));
}

/* ------------------------------------------------------------------ */
/* Pin level plumbing                                                  */
/* ------------------------------------------------------------------ */

/* What the AVR drives onto a pin; released pins read high (pull-ups). */
static bool
avr_drives_high(const tj_board_t *b, int pin)
{
	if (b->ddrb & (1u << pin))
		return (b->portb & (1u << pin)) != 0;
	return true;
}

/*
 * Push the board's own drive of PORTB inputs back into simavr: the action
 * switch external pull-up, and SDA while the OLED is acknowledging.
 */
static void
apply_external_pulls(tj_board_t *b)
{
	uint8_t mask = (uint8_t)(1u << PIN_FIRE);
	uint8_t value = b->buttons[TJ_BTN_FIRE] ? 0u : (uint8_t)(1u << PIN_FIRE);

	if (b->i2c.ack_drive) {
		mask |= (uint8_t)(1u << PIN_SDA);
		/* the value bit stays 0: the slave is pulling SDA down */
	}

	if (mask == b->pull_mask && value == b->pull_value)
		return;

	b->pull_mask = mask;
	b->pull_value = value;

	avr_ioport_external_t ext = { .name = 'B', .mask = mask, .value = value };
	avr_ioctl(b->avr, AVR_IOCTL_IOPORT_SET_EXTERNAL('B'), &ext);

	/*
	 * The ioctl only records the levels; poke the pin IRQs so PINB reflects
	 * them immediately rather than at the next PORTB/DDRB write.
	 */
	avr_irq_t *port = avr_io_getirq(b->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0);
	if (port) {
		for (int pin = 0; pin < 8; pin++)
			if (mask & (1u << pin))
				avr_raise_irq(port + pin, (value >> pin) & 1);
	}
}

/* Integrate the speaker level up to `cycle`, then switch to `level`. */
static void
speaker_set(tj_board_t *b, uint64_t cycle, int level)
{
	if (b->spk_level && cycle > b->spk_mark_cycle)
		b->spk_high_cycles += (double)(cycle - b->spk_mark_cycle);
	b->spk_mark_cycle = cycle;
	b->spk_level = level;
}

/*
 * Recompute the two I2C line levels and the speaker pin from the mirrored
 * PORTB/DDRB, and feed them onwards.
 */
static void
update_lines(tj_board_t *b)
{
	bool scl = avr_drives_high(b, PIN_SCL);
	bool sda = avr_drives_high(b, PIN_SDA);

	/* SDA behaves as open drain here: whoever pulls low wins. */
	if (b->i2c.ack_drive)
		sda = false;

	if (tj_i2c_set_lines(&b->i2c, scl, sda)) {
		/*
		 * The ACK state changed, so the level the AVR sees changed too.
		 * Tell simavr about it.
		 */
		apply_external_pulls(b);
	}

	int spk = ((b->ddrb & (1u << PIN_SOUND)) &&
			   (b->portb & (1u << PIN_SOUND))) ? 1 : 0;
	if (spk != b->spk_level)
		speaker_set(b, b->avr->cycle, spk);
}

static void
portb_write_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	tj_board_t *b = (tj_board_t *)param;
	b->portb = (uint8_t)value;
	update_lines(b);
}

static void
ddrb_write_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
	(void)irq;
	tj_board_t *b = (tj_board_t *)param;
	b->ddrb = (uint8_t)value;
	update_lines(b);
}

/*
 * simavr's tinyx5 core has no PLL, but the ATtiny85 runs from it at 16 MHz and
 * firmware that enables it spins on PLOCK.  Mirror PLLE into PLOCK so that
 * wait terminates, as it does on silicon once the PLL settles.
 */
static void
pllcsr_write_hook(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
	(void)param;
	if (v & (1u << BIT_PLLE))
		v |= (uint8_t)(1u << BIT_PLOCK);
	else
		v &= (uint8_t)~(1u << BIT_PLOCK);
	avr_core_watch_write(avr, addr, v);
}

/*
 * Keep the USI from wedging the I2C clock.
 *
 * Not every TinyJoypad game bit-bangs the display: some (Tiny Invaders v4.2,
 * the older TinyDungeon builds) drive it through the ATtiny85's USI in
 * two-wire mode, which on this board still means SDA on PB0 and SCL on PB2.
 *
 * simavr models the USI's *slave* side clock stretching - hold SCL low while
 * USISIF or USIOIF is set - by putting IRQ_FLAG_STRONG on the SCL pin so that
 * nothing can drive it.  It engages that hold spuriously: simavr assigns an
 * IRQ's new value only after its handlers have run, so when the USI shifts the
 * next data bit onto SDA from inside the SCL falling-edge handler, its own
 * start/stop detector still reads SCL as high and books a START or STOP that
 * never happened on the wire.  The firmware's next clock toggle is then
 * swallowed and it spins forever on `sbis PINB,2`.
 *
 * A TinyJoypad is always the bus master - the OLED is the only device on it,
 * and it is write-only - so slave-mode clock stretching is not part of this
 * board's behaviour at all, and the hold can simply be lifted whenever it
 * appears.  Doing it here rather than patching the simavr submodule keeps the
 * vendored checkout pristine.
 */
static void
release_usi_scl_hold(tj_board_t *b)
{
	if (b->scl_irq)
		b->scl_irq->flags &= ~IRQ_FLAG_STRONG;
	if (b->usi_usck_irq)
		b->usi_usck_irq->flags &= ~IRQ_FLAG_STRONG;
}

/*
 * simavr's stock sleep callback calls usleep(), which would stall the host
 * while the emulator is meant to be producing audio.  Sleeping only has to
 * burn emulated cycles here, and avr_run() advances the cycle counter itself.
 */
static void
board_sleep(avr_t *avr, avr_cycle_count_t how_long)
{
	(void)avr;
	(void)how_long;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void
board_wire(tj_board_t *b)
{
	avr_irq_t *port = avr_io_getirq(b->avr, AVR_IOCTL_IOPORT_GETIRQ('B'), 0);
	if (port) {
		avr_irq_register_notify(port + IOPORT_IRQ_REG_PORT, portb_write_hook, b);
		avr_irq_register_notify(port + IOPORT_IRQ_DIRECTION_ALL, ddrb_write_hook, b);
	}

	b->scl_irq = port ? (port + PIN_SCL) : NULL;
	b->usi_usck_irq = avr_io_getirq(b->avr, AVR_IOCTL_USI_GETIRQ(), USI_IRQ_USCK);

	avr_irq_t *adc = avr_io_getirq(b->avr, AVR_IOCTL_ADC_GETIRQ, ADC_IRQ_OUT_TRIGGER);
	if (adc)
		avr_irq_register_notify(adc, adc_trigger_hook, b);

	avr_register_io_write(b->avr, REG_PLLCSR, pllcsr_write_hook, b);
}

static void
board_post_reset(tj_board_t *b)
{
	b->portb = 0;
	b->ddrb = 0;
	b->pull_mask = 0xff;      /* force apply_external_pulls() to push state */
	b->pull_value = 0xff;

	b->spk_level = 0;
	b->spk_mark_cycle = b->avr->cycle;
	b->spk_high_cycles = 0.0;
	b->next_sample_cycle = (double)b->avr->cycle;
	b->hp_prev_in = 0.0f;
	b->hp_prev_out = 0.0f;
	b->cycles_run = 0;
	b->adc_reads = 0;
	b->run_state = TJ_RUN_OK;

	tj_ssd1306_reset(&b->oled);
	tj_i2c_reset(&b->i2c, &b->oled, SSD1306_I2C_ADDRESS);

	/*
	 * At 16 MHz the part is clocked from the PLL, which the fuses start and
	 * which has locked long before the first instruction runs.
	 */
	if (b->frequency > 8000000)
		b->avr->data[REG_PLLCSR] =
				(uint8_t)((1u << BIT_PLLE) | (1u << BIT_PLOCK) | (1u << BIT_PCKE));

	apply_external_pulls(b);
	update_lines(b);
}

void
tj_board_free(tj_board_t *b)
{
	if (b->avr) {
		avr_terminate(b->avr);
		free(b->avr);
		b->avr = NULL;
	}
	b->loaded = false;
	b->run_state = TJ_RUN_EMPTY;
}

bool
tj_board_load(tj_board_t *b, const char *hex_path, uint32_t frequency)
{
	uint32_t size = 0, start = 0;
	uint8_t *code = read_ihex_file(hex_path, &size, &start);

	if (!code) {
		snprintf(b->error, sizeof(b->error),
				 "Could not read Intel HEX data from this file.");
		return false;
	}

	if (size == 0) {
		free(code);
		snprintf(b->error, sizeof(b->error), "The HEX file contains no code.");
		return false;
	}

	if (start + size > TJ_FLASH_SIZE) {
		snprintf(b->error, sizeof(b->error),
				 "Firmware is %u bytes but an ATtiny85 has only %d. This is a "
				 "build for another board (Arduboy, ESP8266, ...), not a "
				 "TinyJoypad game.",
				 (unsigned)(start + size), TJ_FLASH_SIZE);
		free(code);
		return false;
	}

	tj_board_free(b);

	b->avr = avr_make_mcu_by_name("attiny85");
	if (!b->avr) {
		free(code);
		snprintf(b->error, sizeof(b->error), "simavr has no ATtiny85 core.");
		return false;
	}

	b->frequency = frequency ? frequency : 16000000;
	avr_init(b->avr);

	b->avr->frequency = b->frequency;
	b->avr->vcc  = TJ_VCC_MV;
	b->avr->avcc = TJ_VCC_MV;
	b->avr->aref = TJ_VCC_MV;
	b->avr->log = LOG_ERROR;
	b->avr->sleep = board_sleep;

	avr_loadcode(b->avr, code, size, start);
	b->avr->codeend = start + size;
	free(code);

	b->firmware_size = start + size;
	snprintf(b->firmware_path, sizeof(b->firmware_path), "%s", hex_path);
	b->error[0] = 0;
	b->loaded = true;

	board_wire(b);
	avr_reset(b->avr);
	board_post_reset(b);

	return true;
}

void
tj_board_reset(tj_board_t *b)
{
	if (!b->loaded)
		return;
	avr_reset(b->avr);
	board_post_reset(b);
}

void
tj_board_set_frequency(tj_board_t *b, uint32_t frequency)
{
	if (frequency == 0 || frequency == b->frequency)
		return;
	b->frequency = frequency;
	if (!b->loaded)
		return;
	b->avr->frequency = frequency;
	tj_board_reset(b);
}

void
tj_board_set_button(tj_board_t *b, tj_button_t btn, bool down)
{
	if (btn >= TJ_BTN_COUNT || b->buttons[btn] == down)
		return;
	b->buttons[btn] = down;
	if (b->loaded && btn == TJ_BTN_FIRE)
		apply_external_pulls(b);
	/* The ladders are sampled on demand by adc_trigger_hook(). */
}

/* ------------------------------------------------------------------ */
/* Running                                                             */
/* ------------------------------------------------------------------ */

void
tj_board_run(tj_board_t *b, float *out, int count, int sample_rate)
{
	if (!b->loaded || b->run_state != TJ_RUN_OK || sample_rate <= 0) {
		for (int i = 0; i < count; i++)
			out[i] = 0.0f;
		return;
	}

	double cycles_per_sample = (double)b->frequency / (double)sample_rate;

	for (int i = 0; i < count; i++) {
		uint64_t slice_start = b->avr->cycle;

		b->next_sample_cycle += cycles_per_sample;

		while (b->avr->cycle < (uint64_t)b->next_sample_cycle) {
			int state = avr_run(b->avr);

			/* See release_usi_scl_hold(). */
			if (b->scl_irq && (b->scl_irq->flags & IRQ_FLAG_STRONG))
				release_usi_scl_hold(b);

			if (state == cpu_Done || state == cpu_Crashed) {
				b->run_state = TJ_RUN_HALTED;
				snprintf(b->error, sizeof(b->error),
						 "The firmware stopped at PC 0x%04x (%s).",
						 b->avr->pc,
						 state == cpu_Crashed ? "crashed" : "done");
				break;
			}
		}

		uint64_t slice_end = b->avr->cycle;
		b->cycles_run += slice_end - slice_start;

		/* Close out the speaker integration for this sample. */
		if (b->spk_level && slice_end > b->spk_mark_cycle)
			b->spk_high_cycles += (double)(slice_end - b->spk_mark_cycle);
		b->spk_mark_cycle = slice_end;

		double span = (double)(slice_end - slice_start);
		float duty = (span > 0.0)
				? (float)(b->spk_high_cycles / span)
				: (float)b->spk_level;
		b->spk_high_cycles = 0.0;

		/*
		 * The piezo is AC coupled, so strip the DC component with a one-pole
		 * high pass (~8 Hz).  Without it a pin left high reads as a constant
		 * offset and every tone starts with a thump.
		 */
		float hp = duty - b->hp_prev_in + 0.999f * b->hp_prev_out;
		b->hp_prev_in = duty;
		b->hp_prev_out = hp;

		out[i] = hp;

		if (b->run_state != TJ_RUN_OK) {
			for (int j = i + 1; j < count; j++)
				out[j] = 0.0f;
			return;
		}
	}
}
