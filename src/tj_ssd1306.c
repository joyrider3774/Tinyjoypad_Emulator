#include "tj_ssd1306.h"

#include <math.h>
#include <string.h>

void
tj_ssd1306_reset(tj_ssd1306_t *s)
{
	float rise = s->rise_ms, fall = s->fall_ms;
	bool  persistence = s->persistence_enabled;
	bool  configured = (rise > 0.0f && fall > 0.0f);

	memset(s, 0, sizeof(*s));

	s->addr_mode  = SSD1306_ADDR_MODE_PAGE;
	s->col_start  = 0;
	s->col_end    = SSD1306_COLUMNS - 1;
	s->page_start = 0;
	s->page_end   = SSD1306_PAGES - 1;
	s->contrast   = 0x7f;
	s->mux_ratio  = 64;
	s->display_on = false;
	s->com_scan_dec = false;

	/*
	 * Defaults picked so that a sprite drawn on every other pass reads as a
	 * steady half-shade rather than a strobe, which is how these games look
	 * on the real panel.  Falling is slower than rising, as on the hardware.
	 */
	s->rise_ms = configured ? rise : 3.0f;
	s->fall_ms = configured ? fall : 14.0f;
	s->persistence_enabled = configured ? persistence : true;
}

/* Advance the RAM pointer after a data write, honouring the addressing mode. */
static void
advance_pointer(tj_ssd1306_t *s)
{
	switch (s->addr_mode) {
		case SSD1306_ADDR_MODE_HORIZONTAL:
			if (s->cur_col >= s->col_end) {
				s->cur_col = s->col_start;
				s->cur_page = (s->cur_page >= s->page_end)
						? s->page_start : (uint8_t)(s->cur_page + 1);
			} else {
				s->cur_col++;
			}
			break;
		case SSD1306_ADDR_MODE_VERTICAL:
			if (s->cur_page >= s->page_end) {
				s->cur_page = s->page_start;
				s->cur_col = (s->cur_col >= s->col_end)
						? s->col_start : (uint8_t)(s->cur_col + 1);
			} else {
				s->cur_page++;
			}
			break;
		case SSD1306_ADDR_MODE_PAGE:
		default:
			/* Page mode wraps within the page and never advances the page. */
			s->cur_col = (uint8_t)((s->cur_col + 1) & (SSD1306_COLUMNS - 1));
			break;
	}
}

void
tj_ssd1306_data(tj_ssd1306_t *s, uint8_t byte)
{
	s->data_bytes++;

	uint8_t page = s->cur_page & (SSD1306_PAGES - 1);
	uint8_t col  = s->cur_col;

	if (col < SSD1306_COLUMNS)
		s->vram[page][col] = byte;

	advance_pointer(s);
}

/* Apply a command once all of its argument bytes have arrived. */
static void
execute(tj_ssd1306_t *s, uint8_t cmd, const uint8_t *a, uint8_t n)
{
	switch (cmd) {
		case 0x20: /* Memory addressing mode */
			if (n >= 1 && (a[0] & 3) != 3)
				s->addr_mode = a[0] & 3;
			break;

		case 0x21: /* Column address: start, end */
			if (n >= 2) {
				s->col_start = a[0] & 0x7f;
				s->col_end   = a[1] & 0x7f;
				s->cur_col   = s->col_start;
			}
			break;

		case 0x22: /* Page address: start, end */
			if (n >= 2) {
				s->page_start = a[0] & 0x07;
				s->page_end   = a[1] & 0x07;
				s->cur_page   = s->page_start;
			}
			break;

		case 0x81: /* Contrast */
			if (n >= 1)
				s->contrast = a[0];
			break;

		case 0xa8: /* Multiplex ratio */
			if (n >= 1) {
				uint8_t mux = (uint8_t)((a[0] & 0x3f) + 1);
				if (mux >= 16)
					s->mux_ratio = mux;
			}
			break;

		case 0xd3: /* Display offset */
			if (n >= 1)
				s->display_offset = a[0] & 0x3f;
			break;

		/*
		 * Charge pump, clock divide, pre-charge, VCOMH, COM pin config and
		 * the scroll setup commands have no effect on the emulated image;
		 * their arguments are consumed by the table in arg_count_for() and
		 * dropped here.
		 */
		default:
			break;
	}
}

/* Number of argument bytes a command takes, 0 for single-byte commands. */
static uint8_t
arg_count_for(uint8_t cmd)
{
	switch (cmd) {
		case 0x20: /* addressing mode */
		case 0x81: /* contrast */
		case 0x8d: /* charge pump */
		case 0xa8: /* multiplex ratio */
		case 0xd3: /* display offset */
		case 0xd5: /* clock divide / osc freq */
		case 0xd9: /* pre-charge period */
		case 0xda: /* COM pins config */
		case 0xdb: /* VCOMH deselect */
			return 1;
		case 0x21: /* column address */
		case 0x22: /* page address */
		case 0xa3: /* vertical scroll area */
			return 2;
		case 0x29: /* continuous vertical+horizontal scroll */
		case 0x2a:
			return 5;
		case 0x26: /* continuous horizontal scroll */
		case 0x27:
			return 6;
		default:
			return 0;
	}
}

void
tj_ssd1306_command(tj_ssd1306_t *s, uint8_t byte)
{
	s->command_bytes++;

	if (s->pending_args) {
		if (s->arg_count < sizeof(s->arg))
			s->arg[s->arg_count++] = byte;
		if (--s->pending_args == 0)
			execute(s, s->pending_cmd, s->arg, s->arg_count);
		return;
	}

	uint8_t n = arg_count_for(byte);
	if (n) {
		s->pending_cmd = byte;
		s->pending_args = n;
		s->arg_count = 0;
		return;
	}

	/* Single-byte commands, including the ranged ones. */
	if (byte <= 0x0f) {                       /* lower column nibble, page mode */
		s->cur_col = (uint8_t)((s->cur_col & 0xf0) | (byte & 0x0f));
		return;
	}
	if (byte >= 0x10 && byte <= 0x1f) {       /* higher column nibble, page mode */
		s->cur_col = (uint8_t)((s->cur_col & 0x0f) | ((byte & 0x0f) << 4));
		return;
	}
	if (byte >= 0x40 && byte <= 0x7f) {       /* display start line */
		s->start_line = byte & 0x3f;
		return;
	}
	if (byte >= 0xb0 && byte <= 0xb7) {       /* page start address, page mode */
		s->cur_page = byte & 0x07;
		return;
	}

	switch (byte) {
		case 0x2e: case 0x2f:                 /* scroll off / on - not modelled */
			break;
		case 0xa0: case 0xa1:
			s->segment_remap = (byte == 0xa1);
			break;
		case 0xa4: case 0xa5:
			s->entire_on = (byte == 0xa5);
			break;
		case 0xa6: case 0xa7:
			s->inverted = (byte == 0xa7);
			break;
		case 0xae: case 0xaf:
			s->display_on = (byte == 0xaf);
			break;
		case 0xc0: case 0xc8:
			s->com_scan_dec = (byte == 0xc8);
			break;
		case 0xe3:                            /* NOP */
		default:
			break;
	}
}

void
tj_ssd1306_render(const tj_ssd1306_t *s, uint8_t *out)
{
	/*
	 * Lit pixels are drawn at full brightness whatever the contrast register
	 * says.  It is tempting to scale by it, but on the SSD1306 modules these
	 * games actually run on it makes very little perceptual difference: the
	 * collection ships 0x00, 0x3f, 0x7f and 0xcf interchangeably and every one
	 * of them looks white on real hardware - TinyMania sets 0x00, labels it
	 * "contraste minimal", and still displays white.  No game animates the
	 * register either, so scaling by it only ever made games wrongly dim.
	 * The value is still decoded and shown in the hardware view (F12).
	 */
	const uint8_t on = 255;
	int mux = s->mux_ratio ? s->mux_ratio : 64;

	for (int y = 0; y < SSD1306_HEIGHT; y++) {
		for (int x = 0; x < SSD1306_COLUMNS; x++) {
			int lit;

			if (!s->display_on) {
				lit = 0;
			} else if (s->entire_on) {
				lit = 1;
			} else if (y >= mux) {
				/* Rows beyond the multiplex ratio are never driven. */
				lit = 0;
			} else {
				int com = s->com_scan_dec ? (mux - 1 - y) : y;
				int row = (com + s->start_line + s->display_offset) & 63;
				int seg = s->segment_remap ? (SSD1306_COLUMNS - 1 - x) : x;

				lit = (s->vram[row >> 3][seg] >> (row & 7)) & 1;
				if (s->inverted)
					lit = !lit;
			}
			out[y * SSD1306_COLUMNS + x] = lit ? on : 0;
		}
	}
}

void
tj_ssd1306_step(tj_ssd1306_t *s, float dt_seconds)
{
	uint8_t frame[SSD1306_PIXELS];

	tj_ssd1306_render(s, frame);

	if (!s->persistence_enabled) {
		for (int i = 0; i < SSD1306_PIXELS; i++)
			s->persist[i] = frame[i] * (1.0f / 255.0f);
		return;
	}

	/*
	 * One-pole approach towards the instantaneous image, with separate time
	 * constants for lighting up and going dark.  exp() twice per step rather
	 * than per pixel keeps this cheap.
	 */
	float dt_ms = dt_seconds * 1000.0f;
	float k_rise = 1.0f - expf(-dt_ms / (s->rise_ms > 0.01f ? s->rise_ms : 0.01f));
	float k_fall = 1.0f - expf(-dt_ms / (s->fall_ms > 0.01f ? s->fall_ms : 0.01f));

	for (int i = 0; i < SSD1306_PIXELS; i++) {
		float target = frame[i] * (1.0f / 255.0f);
		float cur = s->persist[i];
		cur += (target - cur) * (target > cur ? k_rise : k_fall);
		s->persist[i] = cur;
	}
}

void
tj_ssd1306_present(const tj_ssd1306_t *s, uint8_t *out)
{
	for (int i = 0; i < SSD1306_PIXELS; i++) {
		float v = s->persist[i];
		if (v <= 0.0f) { out[i] = 0; continue; }
		if (v >= 1.0f) { out[i] = 255; continue; }
		out[i] = (uint8_t)(v * 255.0f + 0.5f);
	}
}
