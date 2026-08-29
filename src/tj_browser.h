/*
 * tj_browser.h - the built-in .hex file browser.
 *
 * Deliberately not just a native file dialog: TinyJoypad games live scattered
 * in deep source trees (".../Tiny Pacman/tinypacman/tinypacman.ino.hex"), so
 * the browser has a recursive mode that collects every .hex under the current
 * directory into one flat, filterable list.  That turns a checkout of a game
 * collection into a playable menu.  F3 still opens the OS dialog for the times
 * that is quicker.
 */
#ifndef TJ_BROWSER_H
#define TJ_BROWSER_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct tj_entry_t {
	char     name[256];       /* leaf name */
	char     label[512];      /* what to show: name, or relative path */
	char     path[1024];      /* full path */
	bool     is_dir;
	bool     is_parent;       /* the ".." entry */
	uint64_t size;
} tj_entry_t;

typedef struct tj_browser_t {
	char        dir[1024];    /* current directory; "" means the drive list */
	tj_entry_t *items;
	int         count;
	int         capacity;

	int         sel;          /* index into the filtered view */
	int         scroll;

	/* Indices of items passing the filter, rebuilt on any change. */
	int        *view;
	int         view_count;

	bool        recursive;
	char        filter[64];
	char        status[256];
	bool        truncated;    /* the recursive scan hit its limit */
} tj_browser_t;

void tj_browser_init(tj_browser_t *b);
void tj_browser_free(tj_browser_t *b);

/* Scan `dir` ("" for the drive list). Falls back to a sensible place if bad. */
void tj_browser_open(tj_browser_t *b, const char *dir);

/* Re-scan the current directory, keeping the selection where possible. */
void tj_browser_refresh(tj_browser_t *b);

void tj_browser_set_recursive(tj_browser_t *b, bool recursive);

/* Navigation. */
void tj_browser_move(tj_browser_t *b, int delta);
void tj_browser_move_to(tj_browser_t *b, int index);
void tj_browser_up(tj_browser_t *b);

/*
 * Act on the selection: enters directories and returns NULL, or returns the
 * full path of a .hex file to load (valid until the next browser call).
 */
const char *tj_browser_activate(tj_browser_t *b);

/* Filter text, as typed. */
void tj_browser_filter_append(tj_browser_t *b, const char *text);
void tj_browser_filter_backspace(tj_browser_t *b);
void tj_browser_filter_clear(tj_browser_t *b);

/* Select the entry whose path matches, if it is in the current listing. */
void tj_browser_select_path(tj_browser_t *b, const char *path);

int tj_browser_visible_rows(void);
void tj_browser_render(tj_browser_t *b, SDL_Renderer *r);

/* Split a full file path into its directory part. */
void tj_path_dirname(char *out, size_t out_size, const char *path);

#endif /* TJ_BROWSER_H */
