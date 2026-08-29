#include "tj_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const tj_color_t TJ_COL_BG     = { 0x10, 0x12, 0x18, 0xff };
const tj_color_t TJ_COL_BAR    = { 0x1a, 0x1e, 0x28, 0xff };
const tj_color_t TJ_COL_PANEL  = { 0x00, 0x00, 0x00, 0xff };
const tj_color_t TJ_COL_SELECT = { 0x2c, 0x3a, 0x55, 0xff };
const tj_color_t TJ_COL_TEXT   = { 0xcc, 0xd2, 0xde, 0xff };
const tj_color_t TJ_COL_DIM    = { 0x6c, 0x76, 0x8a, 0xff };
const tj_color_t TJ_COL_ACCENT = { 0x6f, 0xc3, 0xff, 0xff };
const tj_color_t TJ_COL_WARN   = { 0xf0, 0xc0, 0x60, 0xff };
const tj_color_t TJ_COL_ERROR  = { 0xf0, 0x7a, 0x7a, 0xff };

/*
 * SSD1306 panels come in a few colours and the TinyJoypad has been built with
 * most of them, so the emulator offers the same choice.  "unlit" is not pure
 * black: an OLED off-pixel still catches a little light.
 */
const tj_panel_theme_t TJ_PANEL_THEMES[] = {
	{ "White",  { 0xe8, 0xf2, 0xff, 0xff }, { 0x06, 0x07, 0x0a, 0xff } },
	{ "Blue",   { 0x64, 0xc8, 0xff, 0xff }, { 0x04, 0x08, 0x10, 0xff } },
	{ "Amber",  { 0xff, 0xb0, 0x40, 0xff }, { 0x0c, 0x07, 0x02, 0xff } },
	{ "Green",  { 0x70, 0xff, 0x90, 0xff }, { 0x03, 0x0c, 0x05, 0xff } },
};
const int TJ_PANEL_THEME_COUNT =
		(int)(sizeof(TJ_PANEL_THEMES) / sizeof(TJ_PANEL_THEMES[0]));

void
tj_ui_fill(SDL_Renderer *r, float x, float y, float w, float h, tj_color_t c)
{
	SDL_FRect rect = { x, y, w, h };
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
	SDL_RenderFillRect(r, &rect);
}

void
tj_ui_frame(SDL_Renderer *r, float x, float y, float w, float h, tj_color_t c)
{
	SDL_FRect rect = { x, y, w, h };
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
	SDL_RenderRect(r, &rect);
}

void
tj_ui_text(SDL_Renderer *r, float x, float y, tj_color_t c, const char *text)
{
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
	SDL_RenderDebugText(r, x, y, text);
}

void
tj_ui_textf(SDL_Renderer *r, float x, float y, tj_color_t c, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	SDL_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	tj_ui_text(r, x, y, c, buf);
}

void
tj_ui_elide_end(char *out, size_t out_size, const char *text, int max_chars)
{
	size_t len = SDL_strlen(text);

	if (max_chars < 4)
		max_chars = 4;
	if ((size_t)max_chars >= out_size)
		max_chars = (int)out_size - 1;

	if (len <= (size_t)max_chars) {
		SDL_snprintf(out, out_size, "%s", text);
		return;
	}
	SDL_memcpy(out, text, (size_t)max_chars - 3);
	SDL_memcpy(out + max_chars - 3, "...", 4);
}

void
tj_ui_elide_middle(char *out, size_t out_size, const char *text, int max_chars)
{
	size_t len = SDL_strlen(text);

	if (max_chars < 8)
		max_chars = 8;
	if ((size_t)max_chars >= out_size)
		max_chars = (int)out_size - 1;

	if (len <= (size_t)max_chars) {
		SDL_snprintf(out, out_size, "%s", text);
		return;
	}

	/* Keep a little more of the tail: the file name matters most. */
	int keep = max_chars - 3;
	int head = keep / 3;
	int tail = keep - head;

	SDL_memcpy(out, text, (size_t)head);
	SDL_memcpy(out + head, "...", 3);
	SDL_memcpy(out + head + 3, text + len - (size_t)tail, (size_t)tail);
	out[head + 3 + tail] = 0;
}
