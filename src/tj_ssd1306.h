/*
 * tj_ssd1306.h - SSD1306 128x64 OLED controller emulation.
 *
 * Models the controller's GDDRAM and the subset of its command set that the
 * TinyJoypad display drivers use (FastTinyDriver, ssd1306xled, Adafruit-style
 * init sequences).  It is fed a byte stream by the I2C decoder in tj_i2c.c and
 * is otherwise self-contained.
 *
 * Panel persistence
 * -----------------
 * Reading GDDRAM once per host frame is not enough.  TinyJoypad games render
 * straight into the panel one page at a time with no back buffer, and several
 * of them flicker sprites on alternate frames to fake extra shades.  Sampling
 * that at 60 Hz produces tearing and strobing that the real device does not
 * show.  So, like Ardens does for the Arduboy, this models the panel as a
 * surface with rise/fall time constants: the emulator integrates the
 * instantaneous image into a persistence buffer many times per frame (see
 * tj_ssd1306_step) and only that buffer is displayed.
 */
#ifndef TJ_SSD1306_H
#define TJ_SSD1306_H

#include <stdint.h>
#include <stdbool.h>

#define SSD1306_COLUMNS 128
#define SSD1306_PAGES   8
#define SSD1306_HEIGHT  (SSD1306_PAGES * 8)
#define SSD1306_PIXELS  (SSD1306_COLUMNS * SSD1306_HEIGHT)

enum {
	SSD1306_ADDR_MODE_HORIZONTAL = 0,
	SSD1306_ADDR_MODE_VERTICAL   = 1,
	SSD1306_ADDR_MODE_PAGE       = 2,
};

typedef struct tj_ssd1306_t {
	uint8_t  vram[SSD1306_PAGES][SSD1306_COLUMNS];

	/* Addressing */
	uint8_t  addr_mode;
	uint8_t  col_start, col_end;     /* horizontal/vertical mode window */
	uint8_t  page_start, page_end;
	uint8_t  cur_col, cur_page;

	/* Display setup */
	bool     display_on;
	bool     inverted;               /* 0xA6 / 0xA7 */
	bool     entire_on;              /* 0xA4 / 0xA5 */
	bool     segment_remap;          /* 0xA0 / 0xA1 */
	bool     com_scan_dec;           /* 0xC0 / 0xC8 */
	uint8_t  contrast;
	uint8_t  start_line;
	uint8_t  display_offset;
	uint8_t  mux_ratio;              /* 1..64 */

	/* Command argument state machine */
	uint8_t  pending_cmd;
	uint8_t  pending_args;           /* how many more argument bytes to eat */
	uint8_t  arg[8];
	uint8_t  arg_count;

	/* Panel persistence, 0..1 per pixel */
	float    persist[SSD1306_PIXELS];
	float    rise_ms;                /* time constant for a pixel lighting up */
	float    fall_ms;                /* ...and for going dark */
	bool     persistence_enabled;

	/* Diagnostics, shown in the emulator's debug overlay */
	uint32_t data_bytes;
	uint32_t command_bytes;
} tj_ssd1306_t;

/* Power-on reset state, per the SSD1306 datasheet defaults. */
void tj_ssd1306_reset(tj_ssd1306_t *s);

/* Byte stream from the I2C layer. */
void tj_ssd1306_command(tj_ssd1306_t *s, uint8_t byte);
void tj_ssd1306_data(tj_ssd1306_t *s, uint8_t byte);

/*
 * Instantaneous panel image as 0..255 intensities, applying start line,
 * remap, COM scan direction, multiplex ratio, inversion and contrast.
 */
void tj_ssd1306_render(const tj_ssd1306_t *s, uint8_t *out /* SSD1306_PIXELS */);

/*
 * Integrate the instantaneous image into the persistence buffer over
 * dt_seconds of emulated time.  Call this repeatedly while stepping the CPU -
 * several times per displayed frame - so that mid-frame GDDRAM writes and
 * flicker effects are captured the way the panel would show them.
 */
void tj_ssd1306_step(tj_ssd1306_t *s, float dt_seconds);

/* The image to actually display: the persistence buffer as 0..255. */
void tj_ssd1306_present(const tj_ssd1306_t *s, uint8_t *out /* SSD1306_PIXELS */);

#endif /* TJ_SSD1306_H */
