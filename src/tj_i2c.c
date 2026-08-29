#include "tj_i2c.h"

#include <string.h>

void
tj_i2c_reset(tj_i2c_t *b, tj_ssd1306_t *oled, uint8_t address)
{
	memset(b, 0, sizeof(*b));
	b->oled = oled;
	b->address = address;
	/* Idle bus: both lines released high by the pull-ups. */
	b->scl = true;
	b->sda = true;
}

/*
 * Handle one complete byte clocked in from the master.
 * Returns true to ACK it, false to NAK.
 */
static bool
consume_byte(tj_i2c_t *b, uint8_t v)
{
	b->byte_count++;

	if (b->first_byte) {
		b->first_byte = false;
		b->selected = ((v >> 1) == b->address);
		if (!b->selected) {
			b->nak_count++;
			return false;
		}
		/* A read transfer makes no sense for these drivers; ignore the data. */
		b->expect_control = true;
		b->continuation = false;
		b->data_mode = false;
		return true;
	}

	if (!b->selected)
		return false;

	if (b->expect_control) {
		/*
		 * Control byte: bit 7 (Co) says whether a control byte precedes each
		 * following byte, bit 6 (D/C#) selects data or command.
		 */
		b->continuation = (v & 0x80) != 0;
		b->data_mode = (v & 0x40) != 0;
		b->expect_control = false;
		return true;
	}

	if (b->data_mode)
		tj_ssd1306_data(b->oled, v);
	else
		tj_ssd1306_command(b->oled, v);

	/* With Co set, every payload byte is preceded by its own control byte. */
	if (b->continuation)
		b->expect_control = true;

	return true;
}

static void
begin_transfer(tj_i2c_t *b)
{
	b->in_transfer = true;
	b->first_byte = true;
	b->selected = false;
	b->shift = 0;
	b->bit_pos = 0;
	b->byte_consumed = false;
	b->ack_drive = false;
	b->start_count++;
}

static void
end_transfer(tj_i2c_t *b)
{
	b->in_transfer = false;
	b->selected = false;
	b->bit_pos = 0;
	b->byte_consumed = false;
	b->ack_drive = false;
}

bool
tj_i2c_set_lines(tj_i2c_t *b, bool scl, bool sda)
{
	bool prev_scl = b->scl;
	bool prev_sda = b->sda;
	bool prev_ack = b->ack_drive;

	if (scl == prev_scl && sda == prev_sda)
		return false;

	b->scl = scl;
	b->sda = sda;

	/* START / STOP are SDA edges while SCL is held high. */
	if (prev_scl && scl && sda != prev_sda) {
		if (!sda)
			begin_transfer(b);       /* START, or a repeated START */
		else
			end_transfer(b);         /* STOP */
		return b->ack_drive != prev_ack;
	}

	if (!b->in_transfer)
		return b->ack_drive != prev_ack;

	if (!prev_scl && scl) {
		/* Rising edge: the master has presented a bit. */
		if (b->bit_pos < 8) {
			b->shift = (uint8_t)((b->shift << 1) | (sda ? 1 : 0));
			b->bit_pos++;
		} else {
			/* This was the ACK clock; the next byte starts now. */
			b->bit_pos = 0;
			b->byte_consumed = false;
		}
	} else if (prev_scl && !scl) {
		/*
		 * Falling edge.  After the eighth one the byte is complete and the
		 * slave owns SDA until the ninth falling edge.
		 */
		if (b->bit_pos == 8) {
			if (!b->byte_consumed) {
				b->byte_consumed = true;
				b->ack_drive = consume_byte(b, b->shift);
			}
		} else {
			b->ack_drive = false;
		}
	}

	return b->ack_drive != prev_ack;
}
