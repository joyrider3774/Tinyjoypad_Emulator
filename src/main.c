/*
 * TinyJoypad emulator - SDL3 front end.
 *
 * The emulation core is in tj_board.c: simavr executes the real ATtiny85
 * machine code from a .hex file, and the board model around it provides the
 * SSD1306, the resistor-ladder joystick, the action switch and the piezo.
 * This file is only the shell: window, audio, input and the .hex browser.
 *
 * Audio is the master clock.  Each frame the emulator is run for exactly as
 * many CPU cycles as the audio device still needs samples for, which keeps
 * emulated time locked to real time without a separate timing loop, and keeps
 * the speaker free of gaps.  The panel is integrated several times inside that
 * run so mid-frame display writes are captured (see tj_ssd1306_step).
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SDL_PLATFORM_WINDOWS
#include <unistd.h>      /* isatty(), to decide whether a dialog is wanted */
#endif

#include "tj_board.h"
#include "tj_browser.h"
#include "tj_config.h"
#include "tj_ssd1306.h"
#include "tj_ui.h"

#define TJ_SAMPLE_RATE   48000
#define TJ_AUDIO_CHUNK   128        /* samples between panel integration steps */
#define TJ_AUDIO_TARGET  (TJ_SAMPLE_RATE / 20)   /* 50 ms of buffered audio  */
#define TJ_AUDIO_MAX     (TJ_SAMPLE_RATE / 10)   /* catch-up cap per frame   */
#define TJ_TURBO_SPEED   4
#define TJ_MAX_GAMEPADS  8

/* The panel fills the body area exactly: 128x64 at 4x. */
#define PANEL_X 0
#define PANEL_Y TJ_HEADER_H
#define PANEL_W (SSD1306_COLUMNS * 4)
#define PANEL_H (SSD1306_HEIGHT * 4)

typedef enum {
	APP_BROWSER,
	APP_RUNNING
} app_state_t;

typedef struct {
	SDL_Window      *window;
	SDL_Renderer    *renderer;
	SDL_Texture     *panel;
	SDL_AudioStream *audio;
	SDL_Gamepad     *pads[TJ_MAX_GAMEPADS];

	tj_config_t      cfg;
	tj_board_t       board;
	tj_browser_t     browser;

	app_state_t      state;
	bool             running;
	bool             paused;
	bool             turbo;
	bool             show_help;
	bool             show_debug;
	bool             show_error;
	bool             dialog_open;
	bool             immersive;      /* fullscreen, panel only, no chrome */

	char             game_name[128];
	char             failed_name[128];
	char             load_error[256];

	char             status[256];
	Uint64           status_until;

	Uint64           last_pump_ns;   /* only used when there is no audio device */

	/*
	 * The settings file as it was on disk.  Window size and fullscreen given
	 * on the command line apply to the current run only, so those fields are
	 * restored from here before saving.
	 */
	tj_config_t      cfg_on_disk;
	bool             size_from_cli;
	bool             fullscreen_from_cli;
	bool             rotate_from_cli;

	/* Repeat state for gamepad menu navigation. */
	bool             nav_down[4];
	Uint64           nav_next[4];
} app_t;

static app_t app;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static void
set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	SDL_vsnprintf(app.status, sizeof(app.status), fmt, ap);
	va_end(ap);
	app.status_until = SDL_GetTicks() + 4000;
}

static void
basename_of(char *out, size_t out_size, const char *path)
{
	const char *slash = SDL_strrchr(path, '/');
	const char *back  = SDL_strrchr(path, '\\');
	if (back > slash)
		slash = back;
	SDL_snprintf(out, out_size, "%s", slash ? slash + 1 : path);
}

static const tj_panel_theme_t *
current_theme(void)
{
	int i = app.cfg.panel_theme % TJ_PANEL_THEME_COUNT;
	return &TJ_PANEL_THEMES[i];
}

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

static bool
load_rom(const char *path)
{
	char name[128];
	basename_of(name, sizeof(name), path);

	if (!tj_board_load(&app.board, path, app.cfg.frequency)) {
		/*
		 * A rejected file leaves any game already running untouched, so only
		 * the message changes - the player is put back in the list with an
		 * explanation rather than losing what they were playing.
		 */
		SDL_snprintf(app.failed_name, sizeof(app.failed_name), "%s", name);
		SDL_snprintf(app.load_error, sizeof(app.load_error), "%s",
					 app.board.error);
		app.show_error = true;
		app.state = APP_BROWSER;
		return false;
	}

	app.board.oled.persistence_enabled = app.cfg.persistence;
	app.load_error[0] = 0;
	app.show_error = false;
	SDL_snprintf(app.game_name, sizeof(app.game_name), "%s", name);
	app.state = APP_RUNNING;
	app.paused = false;

	SDL_snprintf(app.cfg.last_rom, sizeof(app.cfg.last_rom), "%s", path);
	tj_path_dirname(app.cfg.last_dir, sizeof(app.cfg.last_dir), path);

	/* Drop whatever the previous game had queued so it does not play on. */
	if (app.audio)
		SDL_ClearAudioStream(app.audio);

	set_status("%s - %u bytes @ %u MHz", app.game_name,
			   app.board.firmware_size, app.board.frequency / 1000000u);
	return true;
}

static void
open_browser_view(void)
{
	app.state = APP_BROWSER;
	if (app.cfg.last_rom[0])
		tj_browser_select_path(&app.browser, app.cfg.last_rom);
}

static void SDLCALL
dialog_callback(void *userdata, const char * const *filelist, int filter)
{
	(void)userdata;
	(void)filter;
	app.dialog_open = false;

	if (!filelist || !filelist[0])
		return;                        /* cancelled, or an error */

	char path[1024];
	SDL_snprintf(path, sizeof(path), "%s", filelist[0]);
	for (char *s = path; *s; s++)
		if (*s == '\\')
			*s = '/';

	if (load_rom(path)) {
		char dir[1024];
		tj_path_dirname(dir, sizeof(dir), path);
		tj_browser_open(&app.browser, dir);
	}
}

static void
open_native_dialog(void)
{
	static const SDL_DialogFileFilter filters[] = {
		{ "Intel HEX firmware", "hex" },
		{ "All files", "*" },
	};

	if (app.dialog_open)
		return;
	app.dialog_open = true;
	SDL_ShowOpenFileDialog(dialog_callback, NULL, app.window, filters, 2,
						   app.browser.dir[0] ? app.browser.dir : NULL, false);
}

/* ------------------------------------------------------------------ */
/* Emulation pump                                                      */
/* ------------------------------------------------------------------ */

static void
push_samples(const float *buf, int count)
{
	if (!app.audio || count <= 0)
		return;

	float scaled[TJ_AUDIO_MAX];
	float vol = app.cfg.volume / 100.0f;
	if (count > TJ_AUDIO_MAX)
		count = TJ_AUDIO_MAX;

	for (int i = 0; i < count; i++) {
		float v = buf[i] * vol;
		scaled[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
	}
	SDL_PutAudioStreamData(app.audio, scaled, count * (int)sizeof(float));
}

static void
pump_emulation(void)
{
	static float out[TJ_AUDIO_MAX];
	static float tmp[TJ_AUDIO_CHUNK];

	Uint64 now = SDL_GetTicksNS();

	if (!app.board.loaded || app.paused || app.state != APP_RUNNING ||
		app.board.run_state != TJ_RUN_OK) {
		app.last_pump_ns = now;
		return;
	}

	int need;
	if (app.audio) {
		int queued = (int)(SDL_GetAudioStreamQueued(app.audio) / (int)sizeof(float));
		need = TJ_AUDIO_TARGET - queued;
	} else {
		/* No audio device: fall back to pacing on the wall clock. */
		Uint64 dt = now - app.last_pump_ns;
		need = (int)((dt * TJ_SAMPLE_RATE) / 1000000000ull);
	}
	app.last_pump_ns = now;

	if (need <= 0)
		return;
	if (need > TJ_AUDIO_MAX)
		need = TJ_AUDIO_MAX;

	int speed = app.turbo ? TJ_TURBO_SPEED : 1;
	int written = 0;

	while (written < need) {
		int n = need - written;
		if (n > TJ_AUDIO_CHUNK)
			n = TJ_AUDIO_CHUNK;

		/*
		 * Turbo runs `speed` chunks of emulated time and keeps one chunk of
		 * audio, which is the usual fast-forward sound: correct duration,
		 * sped-up content.
		 */
		for (int k = 0; k < speed; k++) {
			tj_board_run(&app.board, tmp, n, TJ_SAMPLE_RATE);
			tj_ssd1306_step(&app.board.oled, (float)n / TJ_SAMPLE_RATE);
			if (k == 0)
				SDL_memcpy(out + written, tmp, (size_t)n * sizeof(float));
			if (app.board.run_state != TJ_RUN_OK)
				break;
		}
		written += n;

		if (app.board.run_state != TJ_RUN_OK) {
			SDL_memset(out + written, 0, (size_t)(need - written) * sizeof(float));
			written = need;
			set_status("%s", app.board.error);
			break;
		}
	}

	push_samples(out, written);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	bool up, down, left, right, fire;
} pad_state_t;

/*
 * Keyboard and gamepad are read separately.
 *
 * They are merged for the emulated joypad, but the browser must only ever see
 * the gamepad here: keyboard menu movement comes from SDL_EVENT_KEY_DOWN, so
 * feeding held keys to the repeat logic as well would move the selection twice
 * per press.
 */
static pad_state_t
read_keyboard(void)
{
	pad_state_t s = { 0 };
	const bool *k = SDL_GetKeyboardState(NULL);

	s.up    = k[SDL_SCANCODE_UP]    || k[SDL_SCANCODE_W] || k[SDL_SCANCODE_KP_8];
	s.down  = k[SDL_SCANCODE_DOWN]  || k[SDL_SCANCODE_S] || k[SDL_SCANCODE_KP_2];
	s.left  = k[SDL_SCANCODE_LEFT]  || k[SDL_SCANCODE_A] || k[SDL_SCANCODE_KP_4];
	s.right = k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D] || k[SDL_SCANCODE_KP_6];
	s.fire  = k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_Z] || k[SDL_SCANCODE_X] ||
			  k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_LCTRL] ||
			  k[SDL_SCANCODE_KP_5];
	return s;
}

static pad_state_t
read_gamepads(void)
{
	pad_state_t s = { 0 };
	const int DEADZONE = 12000;

	for (int i = 0; i < TJ_MAX_GAMEPADS; i++) {
		SDL_Gamepad *g = app.pads[i];
		if (!g)
			continue;

		s.up    |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP);
		s.down  |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
		s.left  |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
		s.right |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);

		/*
		 * The TinyJoypad has exactly one action switch, and A and B are it.
		 * X, Y, the shoulders and Select are deliberately not fire: this is a
		 * one-button console, so those are worth far more as emulator
		 * controls than as a fifth and sixth way to press the same switch.
		 */
		s.fire |= SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH) ||
				  SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST);

		int ax = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
		int ay = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
		if (ax < -DEADZONE) s.left  = true;
		if (ax >  DEADZONE) s.right = true;
		if (ay < -DEADZONE) s.up    = true;
		if (ay >  DEADZONE) s.down  = true;
	}
	return s;
}

static pad_state_t
pad_merge(pad_state_t a, pad_state_t b)
{
	pad_state_t s;

	s.up    = a.up    || b.up;
	s.down  = a.down  || b.down;
	s.left  = a.left  || b.left;
	s.right = a.right || b.right;
	s.fire  = a.fire  || b.fire;
	return s;
}

static void
apply_inputs_to_board(const pad_state_t *s)
{
	bool active = (app.state == APP_RUNNING) && !app.paused &&
				  !app.show_help && !app.show_error;

	tj_board_set_button(&app.board, TJ_BTN_UP,    active && s->up);
	tj_board_set_button(&app.board, TJ_BTN_DOWN,  active && s->down);
	tj_board_set_button(&app.board, TJ_BTN_LEFT,  active && s->left);
	tj_board_set_button(&app.board, TJ_BTN_RIGHT, active && s->right);
	tj_board_set_button(&app.board, TJ_BTN_FIRE,  active && s->fire);
}

/*
 * Edge + auto-repeat for driving the menu from a gamepad.  The keyboard gets
 * this for free from SDL's own key repeat.
 */
static bool
nav_repeat(int index, bool held)
{
	Uint64 now = SDL_GetTicks();

	if (!held) {
		app.nav_down[index] = false;
		return false;
	}
	if (!app.nav_down[index]) {
		app.nav_down[index] = true;
		app.nav_next[index] = now + 380;
		return true;
	}
	if (now >= app.nav_next[index]) {
		app.nav_next[index] = now + 60;
		return true;
	}
	return false;
}

static void
gamepad_menu_nav(const pad_state_t *s)
{
	int rows = tj_browser_visible_rows();

	if (nav_repeat(0, s->up))
		tj_browser_move(&app.browser, -1);
	if (nav_repeat(1, s->down))
		tj_browser_move(&app.browser, 1);
	if (nav_repeat(2, s->left))
		tj_browser_move(&app.browser, -rows);
	if (nav_repeat(3, s->right))
		tj_browser_move(&app.browser, rows);
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

static void
cycle_theme(void)
{
	app.cfg.panel_theme = (app.cfg.panel_theme + 1) % TJ_PANEL_THEME_COUNT;
	set_status("Panel: %s", current_theme()->name);
}

static void
toggle_clock(void)
{
	uint32_t next = (app.cfg.frequency == 16000000) ? 8000000 : 16000000;
	app.cfg.frequency = next;
	tj_board_set_frequency(&app.board, next);
	set_status("Clock: %u MHz (restarted)", next / 1000000u);
}

/*
 * Not every TinyJoypad game is drawn for the same way up - some builds assume
 * the board mounted the other way round, and come out inverted here (TinyMania
 * is one).  Rather than guess per game, let the player flip the panel.
 */
static void
toggle_rotation(void)
{
	app.cfg.rotate180 = !app.cfg.rotate180;
	set_status(app.cfg.rotate180 ? "Screen flipped 180 degrees"
								 : "Screen back to normal");
}

static void
toggle_persistence(void)
{
	app.cfg.persistence = !app.cfg.persistence;
	app.board.oled.persistence_enabled = app.cfg.persistence;
	set_status("Panel persistence: %s", app.cfg.persistence ? "on" : "off");
}

static void
adjust_volume(int delta)
{
	int v = app.cfg.volume + delta;
	if (v < 0)   v = 0;
	if (v > 100) v = 100;
	app.cfg.volume = v;
	set_status("Volume: %d%%", v);
}

static void
toggle_fullscreen(void)
{
	app.cfg.fullscreen = !app.cfg.fullscreen;
	SDL_SetWindowFullscreen(app.window, app.cfg.fullscreen);
	set_status(app.cfg.fullscreen ? "Fullscreen - Alt+Enter or F11 to return"
								  : "Windowed");
}

/*
 * Turn the panel's 0..255 intensities into XRGB pixels, applying the colour
 * theme and, when it is on, the 180 degree rotation.
 *
 * Rotating by 180 is a horizontal flip plus a vertical one, which for a linear
 * buffer is just reading it backwards.  Doing it here rather than at draw time
 * means the screenshot comes out the same way up as the screen for free.
 */
static void
blit_panel(const uint8_t *img, void *pixels, int pitch)
{
	const tj_panel_theme_t *th = current_theme();
	bool rotate = app.cfg.rotate180;

	for (int y = 0; y < SSD1306_HEIGHT; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)pixels + y * pitch);
		for (int x = 0; x < SSD1306_COLUMNS; x++) {
			int i = y * SSD1306_COLUMNS + x;
			int v = img[rotate ? (SSD1306_PIXELS - 1 - i) : i];
			Uint8 r = (Uint8)(th->unlit.r + (th->lit.r - th->unlit.r) * v / 255);
			Uint8 g = (Uint8)(th->unlit.g + (th->lit.g - th->unlit.g) * v / 255);
			Uint8 b = (Uint8)(th->unlit.b + (th->lit.b - th->unlit.b) * v / 255);
			row[x] = ((Uint32)r << 16) | ((Uint32)g << 8) | b;
		}
	}
}

/* Save the panel at its native 128x64, which is the useful thing to keep. */
static void
save_screenshot(void)
{
	uint8_t img[SSD1306_PIXELS];

	if (!app.board.loaded) {
		set_status("Nothing to capture.");
		return;
	}
	tj_ssd1306_present(&app.board.oled, img);

	SDL_Surface *surf = SDL_CreateSurface(SSD1306_COLUMNS, SSD1306_HEIGHT,
										  SDL_PIXELFORMAT_XRGB8888);
	if (!surf) {
		set_status("Could not create the screenshot surface.");
		return;
	}

	blit_panel(img, surf->pixels, surf->pitch);

	char *pref = SDL_GetPrefPath("tinyjoypad", "emulator");
	char path[1200];
	char stem[128];
	SDL_snprintf(stem, sizeof(stem), "%s", app.game_name);
	char *dot = SDL_strrchr(stem, '.');
	if (dot)
		*dot = 0;
	SDL_snprintf(path, sizeof(path), "%s%s-%u.bmp", pref ? pref : "",
				 stem, (unsigned)(SDL_GetTicks() & 0xffffff));
	SDL_free(pref);

	if (SDL_SaveBMP(surf, path))
		set_status("Saved %s", path);
	else
		set_status("Screenshot failed: %s", SDL_GetError());
	SDL_DestroySurface(surf);
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void
add_gamepad(SDL_JoystickID id)
{
	for (int i = 0; i < TJ_MAX_GAMEPADS; i++) {
		if (app.pads[i] && SDL_GetGamepadID(app.pads[i]) == id)
			return;
	}
	for (int i = 0; i < TJ_MAX_GAMEPADS; i++) {
		if (!app.pads[i]) {
			app.pads[i] = SDL_OpenGamepad(id);
			if (app.pads[i])
				set_status("Gamepad: %s", SDL_GetGamepadName(app.pads[i]));
			return;
		}
	}
}

static void
remove_gamepad(SDL_JoystickID id)
{
	for (int i = 0; i < TJ_MAX_GAMEPADS; i++) {
		if (app.pads[i] && SDL_GetGamepadID(app.pads[i]) == id) {
			SDL_CloseGamepad(app.pads[i]);
			app.pads[i] = NULL;
			return;
		}
	}
}

static void
handle_browser_key(SDL_Scancode sc, SDL_Keymod mod)
{
	int rows = tj_browser_visible_rows();

	switch (sc) {
		case SDL_SCANCODE_UP:       tj_browser_move(&app.browser, -1); break;
		case SDL_SCANCODE_DOWN:     tj_browser_move(&app.browser, 1); break;
		case SDL_SCANCODE_PAGEUP:
		case SDL_SCANCODE_LEFT:     tj_browser_move(&app.browser, -rows); break;
		case SDL_SCANCODE_PAGEDOWN:
		case SDL_SCANCODE_RIGHT:    tj_browser_move(&app.browser, rows); break;
		case SDL_SCANCODE_HOME:     tj_browser_move_to(&app.browser, 0); break;
		case SDL_SCANCODE_END:
			tj_browser_move_to(&app.browser, app.browser.view_count - 1);
			break;

		case SDL_SCANCODE_RETURN:
		case SDL_SCANCODE_KP_ENTER: {
			const char *path = tj_browser_activate(&app.browser);
			if (path) {
				char copy[1024];
				SDL_snprintf(copy, sizeof(copy), "%s", path);
				load_rom(copy);
			}
			break;
		}

		case SDL_SCANCODE_BACKSPACE:
			if (app.browser.filter[0])
				tj_browser_filter_backspace(&app.browser);
			else
				tj_browser_up(&app.browser);
			break;

		case SDL_SCANCODE_ESCAPE:
			if (app.browser.filter[0])
				tj_browser_filter_clear(&app.browser);
			else if (app.board.loaded)
				app.state = APP_RUNNING;      /* back to the running game */
			else
				app.running = false;
			break;

		case SDL_SCANCODE_F5:
			tj_browser_refresh(&app.browser);
			set_status("Refreshed");
			break;

		case SDL_SCANCODE_F4:
		case SDL_SCANCODE_R:
			/* Ctrl+R is the file-manager habit; F4 is what the footer
			 * advertises, since it needs no modifier. */
			if (sc == SDL_SCANCODE_F4 || (mod & SDL_KMOD_CTRL)) {
				tj_browser_set_recursive(&app.browser, !app.browser.recursive);
				app.cfg.recursive = app.browser.recursive;
				set_status(app.browser.recursive
						   ? "Listing every .hex below this folder"
						   : "Listing this folder only");
			}
			break;

		default:
			break;
	}
}

static void
handle_key(SDL_Scancode sc, SDL_Keymod mod, bool repeat)
{
	/*
	 * Alt+Enter, the toggle every other fullscreen application uses, checked
	 * ahead of the switch so that a plain Enter still reaches the browser.
	 */
	if (!repeat && (mod & SDL_KMOD_ALT) &&
		(sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)) {
		toggle_fullscreen();
		return;
	}

	/* Global keys, available in both views. */
	switch (sc) {
		case SDL_SCANCODE_F1:
			if (!repeat) app.show_help = !app.show_help;
			return;
		case SDL_SCANCODE_F3:
			if (!repeat) open_native_dialog();
			return;
		case SDL_SCANCODE_F6:
			if (!repeat) cycle_theme();
			return;
		case SDL_SCANCODE_F7:
			if (!repeat) toggle_persistence();
			return;
		case SDL_SCANCODE_F8:
			if (!repeat) toggle_rotation();
			return;
		case SDL_SCANCODE_F11:
			if (!repeat) toggle_fullscreen();
			return;
		case SDL_SCANCODE_EQUALS:
		case SDL_SCANCODE_KP_PLUS:
			adjust_volume(5);
			return;
		case SDL_SCANCODE_MINUS:
		case SDL_SCANCODE_KP_MINUS:
			adjust_volume(-5);
			return;
		default:
			break;
	}

	if (app.show_help) {
		if (!repeat && (sc == SDL_SCANCODE_ESCAPE || sc == SDL_SCANCODE_RETURN))
			app.show_help = false;
		return;
	}

	if (app.show_error) {
		if (!repeat)
			app.show_error = false;
		return;
	}

	if (app.state == APP_BROWSER) {
		handle_browser_key(sc, mod);
		return;
	}

	/* Running-game keys. */
	switch (sc) {
		case SDL_SCANCODE_ESCAPE:
			if (!repeat) open_browser_view();
			break;
		case SDL_SCANCODE_F2:
			if (!repeat) {
				tj_board_reset(&app.board);
				if (app.audio)
					SDL_ClearAudioStream(app.audio);
				set_status("Reset");
			}
			break;
		case SDL_SCANCODE_F5:
			if (!repeat) toggle_clock();
			break;
		case SDL_SCANCODE_F10:
			if (!repeat) save_screenshot();
			break;
		case SDL_SCANCODE_F12:
			if (!repeat) app.show_debug = !app.show_debug;
			break;
		case SDL_SCANCODE_P:
			if (!repeat) {
				app.paused = !app.paused;
				set_status(app.paused ? "Paused" : "Resumed");
			}
			break;
		case SDL_SCANCODE_TAB:
			app.turbo = true;
			break;
		default:
			break;
	}
}

static void
handle_events(void)
{
	SDL_Event e;

	while (SDL_PollEvent(&e)) {
		switch (e.type) {
			case SDL_EVENT_QUIT:
				app.running = false;
				break;

			case SDL_EVENT_KEY_DOWN:
				handle_key(e.key.scancode, e.key.mod, e.key.repeat);
				break;

			case SDL_EVENT_KEY_UP:
				if (e.key.scancode == SDL_SCANCODE_TAB)
					app.turbo = false;
				break;

			case SDL_EVENT_TEXT_INPUT:
				/* Typing in the browser filters the list. */
				if (app.state == APP_BROWSER && !app.show_help &&
					!app.show_error && e.text.text[0] >= ' ')
					tj_browser_filter_append(&app.browser, e.text.text);
				break;

			case SDL_EVENT_DROP_FILE: {
				char path[1024];
				SDL_snprintf(path, sizeof(path), "%s", e.drop.data);
				for (char *s = path; *s; s++)
					if (*s == '\\')
						*s = '/';
				if (load_rom(path)) {
					char dir[1024];
					tj_path_dirname(dir, sizeof(dir), path);
					tj_browser_open(&app.browser, dir);
				}
				break;
			}

			case SDL_EVENT_GAMEPAD_ADDED:
				add_gamepad(e.gdevice.which);
				break;
			case SDL_EVENT_GAMEPAD_REMOVED:
				remove_gamepad(e.gdevice.which);
				break;

			case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
				if (app.show_help || app.show_error) {
					app.show_help = false;
					app.show_error = false;
					break;
				}
				/*
				 * Y flips the screen.  X, the shoulders and Select are left
				 * unbound, ready for whatever wants them next.  Start covers
				 * the .hex list on its own, both ways.
				 */
				if (e.gbutton.button == SDL_GAMEPAD_BUTTON_NORTH) {
					toggle_rotation();
					break;
				}
				if (app.state == APP_BROWSER) {
					if (e.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH) {
						const char *path = tj_browser_activate(&app.browser);
						if (path) {
							char copy[1024];
							SDL_snprintf(copy, sizeof(copy), "%s", path);
							load_rom(copy);
						}
					} else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
						tj_browser_up(&app.browser);
					} else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
						if (app.board.loaded)
							app.state = APP_RUNNING;
					}
				} else {
					if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START)
						open_browser_view();
				}
				break;

			case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
				app.cfg.fullscreen = true;
				break;
			case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
				app.cfg.fullscreen = false;
				break;

			case SDL_EVENT_WINDOW_RESIZED:
				if (!app.cfg.fullscreen) {
					app.cfg.window_w = e.window.data1;
					app.cfg.window_h = e.window.data2;
				}
				break;

			default:
				break;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void
upload_panel(void)
{
	uint8_t img[SSD1306_PIXELS];
	void *pixels = NULL;
	int pitch = 0;

	tj_ssd1306_present(&app.board.oled, img);

	if (!SDL_LockTexture(app.panel, NULL, &pixels, &pitch))
		return;

	blit_panel(img, pixels, pitch);
	SDL_UnlockTexture(app.panel);
}

/*
 * In real fullscreen a running game gets the whole display to itself: the
 * title and hint bars are windowed-mode furniture and the point of going
 * fullscreen is to see the panel and nothing else.  The browser still needs
 * its chrome, and so does an error that has to be read and dismissed.
 */
static bool
want_immersive(void)
{
	return app.cfg.fullscreen && app.state == APP_RUNNING && !app.show_error;
}

/* Logical height for the current mode; the width never changes. */
static float
logical_height(void)
{
	return app.immersive ? (float)PANEL_H : (float)TJ_LOGICAL_H;
}

/*
 * Dropping the bars means changing the logical size, so that the panel is
 * still letterboxed to its own 2:1 shape rather than stretched over the
 * display.  Only touch the renderer when the mode actually changes.
 */
static void
apply_presentation(void)
{
	bool immersive = want_immersive();

	if (immersive == app.immersive)
		return;

	app.immersive = immersive;
	SDL_SetRenderLogicalPresentation(app.renderer, TJ_LOGICAL_W,
									 (int)logical_height(),
									 SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

static void
render_header(void)
{
	tj_ui_fill(app.renderer, 0, 0, TJ_LOGICAL_W, TJ_HEADER_H, TJ_COL_BAR);

	char left[128];
	if (app.state == APP_BROWSER) {
		SDL_snprintf(left, sizeof(left), "TinyJoypad  -  select a .hex");
	} else {
		tj_ui_elide_end(left, sizeof(left), app.game_name, 34);
	}
	tj_ui_text(app.renderer, 8, 6, TJ_COL_TEXT, left);

	char right[64];
	SDL_snprintf(right, sizeof(right), "%uMHz  vol %d%%  %s%s",
				 app.cfg.frequency / 1000000u, app.cfg.volume,
				 current_theme()->name, app.cfg.rotate180 ? "  180" : "");
	float x = TJ_LOGICAL_W - 8 - (float)SDL_strlen(right) * TJ_CHAR_W;
	tj_ui_text(app.renderer, x, 6, TJ_COL_DIM, right);

	/* Second line: state flags, or the browser's scan summary. */
	char sub[128] = { 0 };
	if (app.state == APP_RUNNING) {
		SDL_snprintf(sub, sizeof(sub), "ATtiny85 + SSD1306%s%s%s",
					 app.paused ? "  [PAUSED]" : "",
					 app.turbo ? "  [FAST]" : "",
					 app.board.run_state != TJ_RUN_OK ? "  [HALTED]" : "");
	} else if (app.browser.status[0]) {
		SDL_snprintf(sub, sizeof(sub), "%s", app.browser.status);
	} else {
		SDL_snprintf(sub, sizeof(sub), "%s",
					 app.browser.recursive ? "recursive listing"
										   : "folder listing");
	}
	tj_ui_text(app.renderer, 8, 18, TJ_COL_DIM, sub);
}

static void
render_footer(void)
{
	float y = TJ_LOGICAL_H - TJ_FOOTER_H;
	tj_ui_fill(app.renderer, 0, y, TJ_LOGICAL_W, TJ_FOOTER_H, TJ_COL_BAR);

	const char *hint;
	if (app.state == APP_BROWSER)
		hint = "Enter open  Bksp up  type=find  F4 recurse  F3 dialog  F1 help";
	else
		hint = "Arrows+Space play  Esc list  F2 reset  Tab fast  F1 help";
	tj_ui_text(app.renderer, 8, y + 6, TJ_COL_DIM, hint);

	if (app.status[0] && SDL_GetTicks() < app.status_until) {
		char shown[80];
		tj_ui_elide_end(shown, sizeof(shown), app.status, 62);
		tj_ui_text(app.renderer, 8, y + 18, TJ_COL_WARN, shown);
	}
}

/* Wrap `text` to `cols` characters and draw it, returning the next y. */
static float
draw_wrapped(float x, float y, int cols, tj_color_t col, const char *text)
{
	char line[128];
	const char *p = text;

	while (*p) {
		int n = 0, brk = 0;
		while (p[n] && n < cols) {
			if (p[n] == ' ')
				brk = n;
			n++;
		}
		if (p[n] && brk > 0)
			n = brk;

		if (n > (int)sizeof(line) - 1)
			n = (int)sizeof(line) - 1;
		SDL_memcpy(line, p, (size_t)n);
		line[n] = 0;
		tj_ui_text(app.renderer, x, y, col, line);
		y += 10;

		p += n;
		while (*p == ' ')
			p++;
	}
	return y;
}

static void
render_error_panel(void)
{
	const float w = 460, h = 132;
	const float x = (TJ_LOGICAL_W - w) / 2;
	const float y = (logical_height() - h) / 2;
	char title[160];

	tj_ui_fill(app.renderer, x, y, w, h, TJ_COL_BAR);
	tj_ui_frame(app.renderer, x, y, w, h, TJ_COL_ERROR);

	tj_ui_elide_middle(title, sizeof(title), app.failed_name, 54);
	tj_ui_text(app.renderer, x + 12, y + 12, TJ_COL_ERROR, "Cannot run this file");
	tj_ui_text(app.renderer, x + 12, y + 26, TJ_COL_DIM, title);

	draw_wrapped(x + 12, y + 46, 55, TJ_COL_TEXT, app.load_error);

	tj_ui_text(app.renderer, x + 12, y + h - 18, TJ_COL_DIM,
			   "Press any key to pick another .hex.");
}

static void
render_debug(void)
{
	float top = app.immersive ? 0.0f : (float)PANEL_Y;
	float x = 8, y = top + 6;

	tj_ui_fill(app.renderer, 4, top + 2, 244, 92,
			   (tj_color_t){ 0, 0, 0, 190 });

	tj_ui_textf(app.renderer, x, y, TJ_COL_ACCENT, "PC   %04x  SP %04x",
				app.board.avr ? app.board.avr->pc : 0,
				app.board.avr ? (app.board.avr->data[0x5d] |
								 (app.board.avr->data[0x5e] << 8)) : 0);
	y += 10;
	tj_ui_textf(app.renderer, x, y, TJ_COL_TEXT, "PORTB %02x DDRB %02x PINB %02x",
				app.board.portb, app.board.ddrb,
				app.board.avr ? app.board.avr->data[0x36] : 0);
	y += 10;
	tj_ui_textf(app.renderer, x, y, TJ_COL_TEXT, "ADC0 %4d  ADC3 %4d",
				tj_board_adc_value(&app.board, 0),
				tj_board_adc_value(&app.board, 3));
	y += 10;
	tj_ui_textf(app.renderer, x, y, TJ_COL_TEXT, "I2C start %u byte %u nak %u",
				app.board.i2c.start_count, app.board.i2c.byte_count,
				app.board.i2c.nak_count);
	y += 10;
	tj_ui_textf(app.renderer, x, y, TJ_COL_TEXT, "OLED cmd %u data %u %s",
				app.board.oled.command_bytes, app.board.oled.data_bytes,
				app.board.oled.display_on ? "on" : "off");
	y += 10;
	tj_ui_textf(app.renderer, x, y, TJ_COL_TEXT, "contrast %d  mode %d  ADC rd %u",
				app.board.oled.contrast, app.board.oled.addr_mode,
				app.board.adc_reads);
	y += 10;
	tj_ui_textf(app.renderer, x, y, TJ_COL_DIM, "cycles %llu",
				(unsigned long long)app.board.cycles_run);
}

static void
render_help(void)
{
	const float w = 480, h = 240;
	const float x = (TJ_LOGICAL_W - w) / 2;
	const float y = (logical_height() - h) / 2;
	float ty = y + 12;

	tj_ui_fill(app.renderer, x, y, w, h, TJ_COL_BAR);
	tj_ui_frame(app.renderer, x, y, w, h, TJ_COL_ACCENT);

	tj_ui_text(app.renderer, x + 12, ty, TJ_COL_ACCENT, "TinyJoypad emulator");
	ty += 16;

	static const char *lines[] = {
		"Play     Arrows / WASD, and Space Z X Enter to fire",
		"         Pad: d-pad/stick, A or B fire, Start = .hex list",
		"",
		"Esc      Back to the .hex list (Esc again resumes)",
		"Enter    Open a folder, or load the selected .hex",
		"Bksp     Up one folder, or delete a filter character",
		"a-z      Type to filter the list, Esc clears it",
		"F4       List every .hex below this folder (or Ctrl+R)",
		"F3       Use the system file dialog instead",
		"",
		"F2 reset   F5 8/16 MHz   F6 colour   F7 persistence",
		"F8 flip 180 (pad Y)   F10 screenshot   F12 hardware view",
		"Alt+Enter fullscreen   Tab fast-forward   P pause   +/- vol",
		"",
		"You can also drag a .hex file onto the window.",
	};

	for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		tj_ui_text(app.renderer, x + 12, ty, lines[i][0] ? TJ_COL_TEXT : TJ_COL_DIM,
				   lines[i]);
		ty += 12;
	}

	tj_ui_text(app.renderer, x + 12, y + h - 16, TJ_COL_DIM,
			   "F1 or Esc closes this.");
}

/*
 * With the footer gone there is nowhere for "Clock: 8 MHz" and friends to
 * appear, so in fullscreen the status line is drawn over the bottom of the
 * panel instead.  It is transient - it fades out with the same timeout - so it
 * does not become the very chrome fullscreen is meant to remove.
 */
static void
render_immersive_status(void)
{
	char shown[80];
	float h = logical_height();

	if (!app.status[0] || SDL_GetTicks() >= app.status_until)
		return;
	if (app.show_help)
		return;              /* the overlay is already covering this corner */

	tj_ui_elide_end(shown, sizeof(shown), app.status, 62);
	tj_ui_fill(app.renderer, 0, h - 14, TJ_LOGICAL_W, 14,
			   (tj_color_t){ 0, 0, 0, 190 });
	tj_ui_text(app.renderer, 6, h - 12, TJ_COL_WARN, shown);
}

static void
render(void)
{
	apply_presentation();

	SDL_SetRenderDrawColor(app.renderer, TJ_COL_BG.r, TJ_COL_BG.g, TJ_COL_BG.b, 255);
	SDL_RenderClear(app.renderer);

	if (!app.immersive)
		render_header();

	if (app.state == APP_BROWSER) {
		tj_browser_render(&app.browser, app.renderer);
	} else {
		SDL_FRect dst = { PANEL_X, app.immersive ? 0.0f : (float)PANEL_Y,
						  PANEL_W, PANEL_H };
		upload_panel();
		SDL_RenderTexture(app.renderer, app.panel, NULL, &dst);
		if (app.show_debug)
			render_debug();
	}

	if (app.immersive)
		render_immersive_status();
	else
		render_footer();

	if (app.show_error)
		render_error_panel();
	if (app.show_help)
		render_help();

	SDL_RenderPresent(app.renderer);
}

/* ------------------------------------------------------------------ */
/* Command line                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
	const char *rom;
	bool        set_fullscreen;
	bool        fullscreen;
	bool        set_rotate;
	bool        rotate;
	bool        set_size;
	int         width, height;
	bool        help;
	char        error[256];
} cli_args_t;

static const char USAGE[] =
	"TinyJoypad emulator\n"
	"\n"
	"Usage: Tinyjoypad_Emulator [options] [game.hex]\n"
	"\n"
	"  -f, --fullscreen   Start fullscreen\n"
	"  -w, --windowed     Start windowed, ignoring the saved setting\n"
	"  -s, --size WxH     Window size in pixels, e.g. 1280x800 (minimum 256x160)\n"
	"      --scale N      Window size as N times the 512x320 logical size (1-16)\n"
	"  -r, --rotate       Start with the screen flipped 180 degrees\n"
	"      --no-rotate    Start unflipped, ignoring the saved setting\n"
	"  -h, --help         Show this message\n"
	"\n"
	"Without a game.hex the .hex browser opens instead.\n"
	"Size, fullscreen and rotation given here apply to this run only; they are\n"
	"not saved.";

/* Parse "1280x800". */
static bool
parse_size(const char *text, int *w, int *h)
{
	char *end = NULL;
	long a = SDL_strtol(text, &end, 10);

	if (end == text || (*end != 'x' && *end != 'X'))
		return false;

	const char *second = end + 1;
	long b = SDL_strtol(second, &end, 10);
	if (end == second || *end)
		return false;

	/* The window has a minimum of half the logical size; beyond that, trust
	 * the user - they may know something about their display that we do not. */
	if (a < TJ_LOGICAL_W / 2 || b < TJ_LOGICAL_H / 2 || a > 32767 || b > 32767)
		return false;

	*w = (int)a;
	*h = (int)b;
	return true;
}

/*
 * Take the value of an option written either as "--size 1280x800" or
 * "--size=1280x800". Returns NULL if the option is not this one; sets
 * *missing when the option matched but its value is absent.
 */
static const char *
option_value(const char *arg, const char *shortopt, const char *longopt,
			 int *index, int argc, char *argv[], bool *missing)
{
	size_t longlen = SDL_strlen(longopt);

	if (!SDL_strncmp(arg, longopt, longlen) && arg[longlen] == '=')
		return arg + longlen + 1;

	if (SDL_strcmp(arg, longopt) && (!shortopt || SDL_strcmp(arg, shortopt)))
		return NULL;

	if (*index + 1 >= argc) {
		*missing = true;
		return NULL;
	}
	return argv[++(*index)];
}

static bool
parse_args(int argc, char *argv[], cli_args_t *a)
{
	bool options_done = false;

	SDL_memset(a, 0, sizeof(*a));

	for (int i = 1; i < argc; i++) {
		const char *arg = argv[i];

		if (!options_done && arg[0] == '-' && arg[1]) {
			bool missing = false;
			const char *value;

			if (!SDL_strcmp(arg, "--")) {
				options_done = true;
				continue;
			}
			if (!SDL_strcmp(arg, "-h") || !SDL_strcmp(arg, "--help")) {
				a->help = true;
				return true;
			}
			if (!SDL_strcmp(arg, "-f") || !SDL_strcmp(arg, "--fullscreen")) {
				a->set_fullscreen = true;
				a->fullscreen = true;
				continue;
			}
			if (!SDL_strcmp(arg, "-w") || !SDL_strcmp(arg, "--windowed")) {
				a->set_fullscreen = true;
				a->fullscreen = false;
				continue;
			}
			if (!SDL_strcmp(arg, "-r") || !SDL_strcmp(arg, "--rotate")) {
				a->set_rotate = true;
				a->rotate = true;
				continue;
			}
			if (!SDL_strcmp(arg, "--no-rotate")) {
				a->set_rotate = true;
				a->rotate = false;
				continue;
			}

			value = option_value(arg, "-s", "--size", &i, argc, argv, &missing);
			if (missing) {
				SDL_snprintf(a->error, sizeof(a->error),
							 "%s needs a size, for example --size 1280x800.", arg);
				return false;
			}
			if (value) {
				if (!parse_size(value, &a->width, &a->height)) {
					SDL_snprintf(a->error, sizeof(a->error),
								 "\"%s\" is not a usable window size. Expected "
								 "WIDTHxHEIGHT, at least %dx%d.",
								 value, TJ_LOGICAL_W / 2, TJ_LOGICAL_H / 2);
					return false;
				}
				a->set_size = true;
				continue;
			}

			value = option_value(arg, NULL, "--scale", &i, argc, argv, &missing);
			if (missing) {
				SDL_snprintf(a->error, sizeof(a->error),
							 "--scale needs a number, for example --scale 3.");
				return false;
			}
			if (value) {
				int scale = SDL_atoi(value);
				if (scale < 1 || scale > 16) {
					SDL_snprintf(a->error, sizeof(a->error),
								 "--scale takes a number from 1 to 16, not \"%s\".",
								 value);
					return false;
				}
				a->width = TJ_LOGICAL_W * scale;
				a->height = TJ_LOGICAL_H * scale;
				a->set_size = true;
				continue;
			}

			SDL_snprintf(a->error, sizeof(a->error), "Unknown option \"%s\".", arg);
			return false;
		}

		if (a->rom) {
			SDL_snprintf(a->error, sizeof(a->error),
						 "Only one .hex file can be given.");
			return false;
		}
		a->rom = arg;
	}
	return true;
}

/*
 * Report to whoever launched us.
 *
 * The text always goes to the standard streams.  The dialog is a fallback for
 * when there is nowhere for that text to land, which differs by platform: on
 * Windows this is a windowed binary with no console at all, so the dialog is
 * the only thing the user will ever see, whereas on Linux and macOS a shell
 * gives us a perfectly good terminal and popping a window over it for --help
 * would be obnoxious.  There, the dialog appears only when the program was
 * started from a file manager or the dock.
 *
 * SDL_ShowSimpleMessageBox may be called before SDL_Init - it looks for a
 * usable video driver itself - and it fails harmlessly on a machine with no
 * display, which the printed text already covers.
 */
static void
show_message(const char *title, const char *text, bool is_error)
{
	FILE *stream = is_error ? stderr : stdout;
	bool  on_terminal;

	fputs(text, stream);
	fputc('\n', stream);
	fflush(stream);

#ifdef SDL_PLATFORM_WINDOWS
	on_terminal = false;
#else
	on_terminal = isatty(fileno(stream)) != 0;
#endif

	if (!on_terminal) {
		SDL_ShowSimpleMessageBox(is_error ? SDL_MESSAGEBOX_ERROR
										  : SDL_MESSAGEBOX_INFORMATION,
								 title, text, NULL);
	}
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static bool
init_sdl(void)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
		show_message("TinyJoypad emulator", SDL_GetError(), true);
		return false;
	}

	app.window = SDL_CreateWindow("TinyJoypad emulator",
								  app.cfg.window_w, app.cfg.window_h,
								  SDL_WINDOW_RESIZABLE);
	if (!app.window) {
		show_message("TinyJoypad emulator", SDL_GetError(), true);
		return false;
	}
	SDL_SetWindowMinimumSize(app.window, TJ_LOGICAL_W / 2, TJ_LOGICAL_H / 2);
	if (app.cfg.fullscreen)
		SDL_SetWindowFullscreen(app.window, true);

	app.renderer = SDL_CreateRenderer(app.window, NULL);
	if (!app.renderer) {
		show_message("TinyJoypad emulator", SDL_GetError(), true);
		return false;
	}
	SDL_SetRenderVSync(app.renderer, 1);

	/*
	 * Everything is drawn at a fixed 512x320 and letterboxed into whatever
	 * the window happens to be, so the layout never has to care about the
	 * window size and the aspect ratio of the panel is always preserved.
	 */
	SDL_SetRenderLogicalPresentation(app.renderer, TJ_LOGICAL_W, TJ_LOGICAL_H,
									 SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetDefaultTextureScaleMode(app.renderer, SDL_SCALEMODE_NEAREST);

	app.panel = SDL_CreateTexture(app.renderer, SDL_PIXELFORMAT_XRGB8888,
								  SDL_TEXTUREACCESS_STREAMING,
								  SSD1306_COLUMNS, SSD1306_HEIGHT);
	if (!app.panel) {
		show_message("TinyJoypad emulator", SDL_GetError(), true);
		return false;
	}
	SDL_SetTextureScaleMode(app.panel, SDL_SCALEMODE_NEAREST);

	SDL_AudioSpec spec = { SDL_AUDIO_F32, 1, TJ_SAMPLE_RATE };
	app.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
										  &spec, NULL, NULL);
	if (app.audio)
		SDL_ResumeAudioStreamDevice(app.audio);
	/* No audio device is not fatal: pump_emulation() falls back to the clock. */

	SDL_StartTextInput(app.window);
	return true;
}

static void
shutdown_sdl(void)
{
	for (int i = 0; i < TJ_MAX_GAMEPADS; i++)
		if (app.pads[i])
			SDL_CloseGamepad(app.pads[i]);
	if (app.audio)
		SDL_DestroyAudioStream(app.audio);
	if (app.panel)
		SDL_DestroyTexture(app.panel);
	if (app.renderer)
		SDL_DestroyRenderer(app.renderer);
	if (app.window)
		SDL_DestroyWindow(app.window);
	SDL_Quit();
}

int
main(int argc, char *argv[])
{
	cli_args_t args;

	if (!parse_args(argc, argv, &args)) {
		char text[sizeof(args.error) + sizeof(USAGE) + 4];
		SDL_snprintf(text, sizeof(text), "%s\n\n%s", args.error, USAGE);
		show_message("TinyJoypad emulator", text, true);
		return 2;
	}
	if (args.help) {
		show_message("TinyJoypad emulator", USAGE, false);
		return 0;
	}

	tj_config_load(&app.cfg);
	app.cfg_on_disk = app.cfg;

	if (args.set_size) {
		app.cfg.window_w = args.width;
		app.cfg.window_h = args.height;
		app.size_from_cli = true;
	}
	if (args.set_fullscreen) {
		app.cfg.fullscreen = args.fullscreen;
		app.fullscreen_from_cli = true;
	}
	if (args.set_rotate) {
		app.cfg.rotate180 = args.rotate;
		app.rotate_from_cli = true;
	}

	tj_browser_init(&app.browser);

	if (!init_sdl()) {
		shutdown_sdl();
		return 1;
	}

	app.running = true;
	app.state = APP_BROWSER;
	app.board.oled.persistence_enabled = app.cfg.persistence;
	app.browser.recursive = app.cfg.recursive;

	/* A .hex on the command line starts straight into the game. */
	const char *initial = args.rom;

	char start_dir[1024];
	if (initial) {
		char path[1024];
		SDL_snprintf(path, sizeof(path), "%s", initial);
		for (char *s = path; *s; s++)
			if (*s == '\\')
				*s = '/';
		tj_path_dirname(start_dir, sizeof(start_dir), path);
		tj_browser_open(&app.browser, start_dir);
		load_rom(path);
	} else {
		tj_browser_open(&app.browser, app.cfg.last_dir);
		if (app.cfg.last_rom[0])
			tj_browser_select_path(&app.browser, app.cfg.last_rom);
	}

	app.last_pump_ns = SDL_GetTicksNS();

	while (app.running) {
		handle_events();

		pad_state_t keys = read_keyboard();
		pad_state_t pads = read_gamepads();
		pad_state_t in = pad_merge(keys, pads);

		/* Menu repeat runs off the gamepad only; see read_keyboard(). */
		if (app.state == APP_BROWSER && !app.show_help && !app.show_error)
			gamepad_menu_nav(&pads);
		apply_inputs_to_board(&in);

		pump_emulation();
		render();
	}

	SDL_snprintf(app.cfg.last_dir, sizeof(app.cfg.last_dir), "%s",
				 app.browser.dir);
	app.cfg.recursive = app.browser.recursive;

	/* A size, fullscreen or rotation asked for on the command line was for this
	 * run; keep the user's saved preferences intact. */
	if (app.size_from_cli) {
		app.cfg.window_w = app.cfg_on_disk.window_w;
		app.cfg.window_h = app.cfg_on_disk.window_h;
	}
	if (app.fullscreen_from_cli)
		app.cfg.fullscreen = app.cfg_on_disk.fullscreen;
	if (app.rotate_from_cli)
		app.cfg.rotate180 = app.cfg_on_disk.rotate180;

	tj_config_save(&app.cfg);

	tj_browser_free(&app.browser);
	tj_board_free(&app.board);
	shutdown_sdl();
	return 0;
}
