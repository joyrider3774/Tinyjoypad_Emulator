/*
 * tj_board.h - the TinyJoypad rev2 board: an ATtiny85 emulated by simavr with
 * the hardware around it wired up.
 *
 *                     3V3
 *                      |
 *      PB5/ADC0 <-- 22k+-- [1: 88k] [3: 33k] --> GND     left / right ladder
 *      PB3/ADC3 <-- 22k+-- [2: 33k] [4: 88k] --> GND     up / down ladder
 *      PB1      <-- 10k pull-up, action switch to GND
 *      PB4      --> piezo speaker
 *      PB0      <-> SSD1306 SDA   (bit-banged)
 *      PB2      --> SSD1306 SCL   (bit-banged)
 *
 * Nothing here interprets the firmware: simavr executes the ATtiny85 machine
 * code out of the .hex verbatim, and this file only models what the pins are
 * connected to.
 */
#ifndef TJ_BOARD_H
#define TJ_BOARD_H

#include <stdint.h>
#include <stdbool.h>

#include "sim_avr.h"

#include "tj_i2c.h"
#include "tj_ssd1306.h"

/* ATtiny85 flash size - anything larger is not a TinyJoypad binary. */
#define TJ_FLASH_SIZE 8192

typedef enum {
	TJ_BTN_UP = 0,
	TJ_BTN_DOWN,
	TJ_BTN_LEFT,
	TJ_BTN_RIGHT,
	TJ_BTN_FIRE,
	TJ_BTN_COUNT
} tj_button_t;

typedef enum {
	TJ_RUN_OK = 0,      /* running normally */
	TJ_RUN_HALTED,      /* firmware executed a halt/invalid opcode */
	TJ_RUN_EMPTY        /* nothing loaded */
} tj_run_state_t;

typedef struct tj_board_t {
	avr_t         *avr;
	tj_ssd1306_t   oled;
	tj_i2c_t       i2c;

	uint32_t       frequency;         /* Hz the ATtiny85 is clocked at */
	uint32_t       firmware_size;     /* bytes of flash actually occupied */
	char           firmware_path[1024];
	char           error[256];
	bool           loaded;
	tj_run_state_t run_state;

	bool           buttons[TJ_BTN_COUNT];

	/* Cached IRQs, see release_usi_scl_hold() in tj_board.c. */
	struct avr_irq_t *scl_irq;
	struct avr_irq_t *usi_usck_irq;

	/* Mirrors of PORTB / DDRB, kept current from the ioport IRQs. */
	uint8_t        portb, ddrb;
	uint8_t        pull_mask, pull_value;

	/* Speaker: exact integration of the PB4 square wave between samples. */
	int            spk_level;
	uint64_t       spk_mark_cycle;
	double         spk_high_cycles;
	double         next_sample_cycle;
	float          hp_prev_in, hp_prev_out;

	/* Diagnostics */
	uint64_t       cycles_run;
	uint32_t       adc_reads;
} tj_board_t;

/*
 * Load an Intel HEX file into a fresh ATtiny85.  Returns false and fills in
 * b->error on failure (unreadable file, bad HEX, or a binary too large to be
 * an ATtiny85 image - the usual sign of an Arduboy or ESP build).
 */
bool tj_board_load(tj_board_t *b, const char *hex_path, uint32_t frequency);

/* Restart the loaded firmware from reset without re-reading the file. */
void tj_board_reset(tj_board_t *b);

/* Change the clock. Keeps the firmware loaded but restarts it. */
void tj_board_set_frequency(tj_board_t *b, uint32_t frequency);

void tj_board_free(tj_board_t *b);

void tj_board_set_button(tj_board_t *b, tj_button_t btn, bool down);

/*
 * Run the CPU for `count` audio samples' worth of cycles, writing the speaker
 * output to out[] as floats in -1..1.  This is the emulator's master clock:
 * everything else (video, input polling) hangs off how much audio the device
 * still needs.
 */
void tj_board_run(tj_board_t *b, float *out, int count, int sample_rate);

/* Current ADC reading of a ladder, for the debug overlay. 0..1023. */
int tj_board_adc_value(const tj_board_t *b, int channel);

/*
 * EEPROM access, so the front end can keep it in a file between runs.  Games
 * use it for high scores and progress, and it survives reset on real hardware
 * exactly as it does here.  512 bytes on an ATtiny85.
 */
uint32_t tj_board_eeprom_size(tj_board_t *b);
bool tj_board_eeprom_get(tj_board_t *b, uint8_t *out, uint32_t size);
bool tj_board_eeprom_set(tj_board_t *b, const uint8_t *data, uint32_t size);

#endif /* TJ_BOARD_H */
