/*
 * tj_ui.h - small drawing helpers shared by the browser and the emulator view.
 *
 * Everything is drawn in the renderer's logical coordinate space (see
 * TJ_LOGICAL_W/H), which is set up once with letterbox presentation, so these
 * helpers never have to know the window size.
 *
 * Text uses SDL_RenderDebugText's built-in 8x8 font: it needs no asset files,
 * which keeps the emulator a single self-contained executable.
 */
#ifndef TJ_UI_H
#define TJ_UI_H

#include <SDL3/SDL.h>
#include <stdbool.h>

/*
 * Logical resolution. 512x320 is four OLED pixels per logical pixel for the
 * 128x64 panel (512x256), plus a 32px bar above and below for the title and
 * the key hints - and 64 columns of 8px text, enough for readable file paths.
 */
#define TJ_LOGICAL_W  512
#define TJ_LOGICAL_H  320
#define TJ_HEADER_H    32
#define TJ_FOOTER_H    32
#define TJ_BODY_Y      (TJ_HEADER_H)
#define TJ_BODY_H      (TJ_LOGICAL_H - TJ_HEADER_H - TJ_FOOTER_H)

#define TJ_CHAR_W  8
#define TJ_CHAR_H  8

typedef struct { Uint8 r, g, b, a; } tj_color_t;

/* Interface palette. */
extern const tj_color_t TJ_COL_BG;
extern const tj_color_t TJ_COL_BAR;
extern const tj_color_t TJ_COL_PANEL;
extern const tj_color_t TJ_COL_SELECT;
extern const tj_color_t TJ_COL_TEXT;
extern const tj_color_t TJ_COL_DIM;
extern const tj_color_t TJ_COL_ACCENT;
extern const tj_color_t TJ_COL_WARN;
extern const tj_color_t TJ_COL_ERROR;

/* Panel colour themes, applied to the emulated OLED. */
typedef struct {
	const char *name;
	tj_color_t  lit;
	tj_color_t  unlit;
} tj_panel_theme_t;

extern const tj_panel_theme_t TJ_PANEL_THEMES[];
extern const int TJ_PANEL_THEME_COUNT;

void tj_ui_fill(SDL_Renderer *r, float x, float y, float w, float h, tj_color_t c);
void tj_ui_frame(SDL_Renderer *r, float x, float y, float w, float h, tj_color_t c);
void tj_ui_text(SDL_Renderer *r, float x, float y, tj_color_t c, const char *text);
void tj_ui_textf(SDL_Renderer *r, float x, float y, tj_color_t c,
				 SDL_PRINTF_FORMAT_STRING const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(5);

/*
 * Write `text` into `out` limited to `max_chars`, eliding the middle with "..."
 * when it does not fit.  Used for long paths, where both ends carry meaning.
 */
void tj_ui_elide_middle(char *out, size_t out_size, const char *text, int max_chars);

/* Same, but keeps the start and drops the tail. */
void tj_ui_elide_end(char *out, size_t out_size, const char *text, int max_chars);

#endif /* TJ_UI_H */
