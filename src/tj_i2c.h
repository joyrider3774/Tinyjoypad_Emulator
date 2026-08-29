/*
 * tj_i2c.h - decoder for the bit-banged I2C bus between the ATtiny85 and the
 * SSD1306.
 *
 * The TinyJoypad has no hardware TWI in use: every display driver used by
 * these games (FastTinyDriver's inline asm, ssd1306xled, ...) toggles PB0 as
 * SDA and PB2 as SCL by hand.  So rather than hooking a TWI peripheral, this
 * watches the two pin levels and recovers START/STOP/bits/bytes from them,
 * exactly as a logic analyser on the real bus would.
 *
 * It also drives the ACK bit, pulling SDA low for the ninth clock of every
 * byte it accepts.  FastTinyDriver never reads the ACK back, but other
 * drivers do, and a NAK is how a firmware written for a different display
 * address discovers there is nothing on the bus.
 */
#ifndef TJ_I2C_H
#define TJ_I2C_H

#include <stdint.h>
#include <stdbool.h>

#include "tj_ssd1306.h"

typedef struct tj_i2c_t {
	tj_ssd1306_t *oled;
	uint8_t  address;            /* 7-bit slave address the OLED answers on */

	/* Previous line levels, to spot edges */
	bool     scl, sda;

	bool     in_transfer;        /* between START and STOP */
	bool     selected;           /* addressed slave is ours */
	bool     expect_control;     /* next byte is a control byte */
	bool     data_mode;          /* control byte said D/C# = 1 */
	bool     continuation;       /* control byte had Co = 1 */
	bool     first_byte;         /* next byte is the address byte */

	uint8_t  shift;
	uint8_t  bit_pos;            /* 0..7 data bits, 8 = ACK slot */
	bool     byte_consumed;      /* the byte in `shift` was already handled */

	/* Set while the slave holds SDA low for an ACK. */
	bool     ack_drive;

	/* Diagnostics */
	uint32_t start_count;
	uint32_t byte_count;
	uint32_t nak_count;
} tj_i2c_t;

void tj_i2c_reset(tj_i2c_t *b, tj_ssd1306_t *oled, uint8_t address);

/*
 * Feed the current bus levels.  Call on every change of either line; passing
 * unchanged levels is harmless.  Returns true if b->ack_drive changed, which
 * means the caller must push the new SDA level back to the AVR.
 */
bool tj_i2c_set_lines(tj_i2c_t *b, bool scl, bool sda);

#endif /* TJ_I2C_H */
