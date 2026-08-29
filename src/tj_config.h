/*
 * tj_config.h - persisted settings, stored as key=value lines next to the
 * user's other SDL preferences (SDL_GetPrefPath).
 */
#ifndef TJ_CONFIG_H
#define TJ_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct tj_config_t {
	char     last_dir[1024];      /* directory the browser reopens in */
	char     last_rom[1024];      /* last successfully loaded .hex */
	int      panel_theme;         /* index into TJ_PANEL_THEMES */
	bool     rotate180;           /* flip the panel horizontally + vertically */
	int      volume;              /* 0..100 */
	uint32_t frequency;           /* ATtiny85 clock in Hz */
	bool     recursive;           /* browser lists .hex from subdirectories */
	bool     persistence;         /* emulate OLED pixel persistence */
	bool     fullscreen;
	int      window_w, window_h;
} tj_config_t;

void tj_config_defaults(tj_config_t *c);
void tj_config_load(tj_config_t *c);
void tj_config_save(const tj_config_t *c);

#endif /* TJ_CONFIG_H */
