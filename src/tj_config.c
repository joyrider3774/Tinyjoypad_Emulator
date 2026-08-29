#include "tj_config.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <string.h>

#define CONFIG_ORG  "tinyjoypad"
#define CONFIG_APP  "emulator"
#define CONFIG_FILE "settings.cfg"

void
tj_config_defaults(tj_config_t *c)
{
	SDL_memset(c, 0, sizeof(*c));
	c->panel_theme = 0;
	c->rotate180   = false;
	c->volume      = 60;
	c->frequency   = 16000000;   /* TinyJoypad rev2 runs the ATtiny85 at 16 MHz */
	c->recursive   = false;
	c->persistence = true;
	c->fullscreen  = false;
	c->window_w    = 1024;
	c->window_h    = 640;
	c->last_dir[0] = 0;
	c->last_rom[0] = 0;
}

/* Build "<prefpath>settings.cfg". Caller frees with SDL_free. */
static char *
config_path(void)
{
	char *pref = SDL_GetPrefPath(CONFIG_ORG, CONFIG_APP);
	if (!pref)
		return NULL;

	size_t len = SDL_strlen(pref) + sizeof(CONFIG_FILE) + 1;
	char *path = (char *)SDL_malloc(len);
	if (path)
		SDL_snprintf(path, len, "%s%s", pref, CONFIG_FILE);
	SDL_free(pref);
	return path;
}

static void
trim(char *s)
{
	size_t n = SDL_strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
				 s[n - 1] == ' '  || s[n - 1] == '\t'))
		s[--n] = 0;
}

void
tj_config_load(tj_config_t *c)
{
	tj_config_defaults(c);

	char *path = config_path();
	if (!path)
		return;

	size_t size = 0;
	char *data = (char *)SDL_LoadFile(path, &size);
	SDL_free(path);
	if (!data)
		return;

	char *save = NULL;
	for (char *line = SDL_strtok_r(data, "\n", &save); line;
		 line = SDL_strtok_r(NULL, "\n", &save)) {
		char *eq = SDL_strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *key = line;
		char *val = eq + 1;
		trim(key);
		trim(val);

		if (!SDL_strcmp(key, "last_dir"))
			SDL_snprintf(c->last_dir, sizeof(c->last_dir), "%s", val);
		else if (!SDL_strcmp(key, "last_rom"))
			SDL_snprintf(c->last_rom, sizeof(c->last_rom), "%s", val);
		else if (!SDL_strcmp(key, "panel_theme"))
			c->panel_theme = SDL_atoi(val);
		else if (!SDL_strcmp(key, "rotate180"))
			c->rotate180 = SDL_atoi(val) != 0;
		else if (!SDL_strcmp(key, "volume"))
			c->volume = SDL_atoi(val);
		else if (!SDL_strcmp(key, "frequency"))
			c->frequency = (uint32_t)SDL_strtoul(val, NULL, 10);
		else if (!SDL_strcmp(key, "recursive"))
			c->recursive = SDL_atoi(val) != 0;
		else if (!SDL_strcmp(key, "persistence"))
			c->persistence = SDL_atoi(val) != 0;
		else if (!SDL_strcmp(key, "fullscreen"))
			c->fullscreen = SDL_atoi(val) != 0;
		else if (!SDL_strcmp(key, "window_w"))
			c->window_w = SDL_atoi(val);
		else if (!SDL_strcmp(key, "window_h"))
			c->window_h = SDL_atoi(val);
	}
	SDL_free(data);

	/* Guard against a hand-edited or stale file. */
	if (c->volume < 0)   c->volume = 0;
	if (c->volume > 100) c->volume = 100;
	if (c->frequency < 1000000 || c->frequency > 32000000)
		c->frequency = 16000000;
	if (c->window_w < 320) c->window_w = 1024;
	if (c->window_h < 200) c->window_h = 640;
	if (c->panel_theme < 0) c->panel_theme = 0;
}

void
tj_config_save(const tj_config_t *c)
{
	char *path = config_path();
	if (!path)
		return;

	SDL_IOStream *io = SDL_IOFromFile(path, "w");
	SDL_free(path);
	if (!io)
		return;

	char buf[2600];
	int n = SDL_snprintf(buf, sizeof(buf),
			"# TinyJoypad emulator settings\n"
			"last_dir=%s\n"
			"last_rom=%s\n"
			"panel_theme=%d\n"
			"rotate180=%d\n"
			"volume=%d\n"
			"frequency=%u\n"
			"recursive=%d\n"
			"persistence=%d\n"
			"fullscreen=%d\n"
			"window_w=%d\n"
			"window_h=%d\n",
			c->last_dir, c->last_rom, c->panel_theme, c->rotate180 ? 1 : 0,
			c->volume,
			(unsigned)c->frequency, c->recursive ? 1 : 0,
			c->persistence ? 1 : 0, c->fullscreen ? 1 : 0,
			c->window_w, c->window_h);

	if (n > 0)
		SDL_WriteIO(io, buf, (size_t)n);
	SDL_CloseIO(io);
}
