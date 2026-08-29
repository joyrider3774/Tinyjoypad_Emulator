#include "tj_browser.h"
#include "tj_ui.h"

#include <stdlib.h>
#include <string.h>

/* Bounds for the recursive scan, so a wrong turn into C:/ cannot hang the UI. */
#define SCAN_MAX_DEPTH   12
#define SCAN_MAX_ENTRIES 4000
#define SCAN_MAX_DIRS    40000

#define ROW_H 12

/* ------------------------------------------------------------------ */
/* Path helpers                                                        */
/* ------------------------------------------------------------------ */

static void
normalise(char *p)
{
	for (char *s = p; *s; s++)
		if (*s == '\\')
			*s = '/';
}

static bool
is_drive_root(const char *p)
{
	return p[0] && p[1] == ':' && p[2] == '/' && p[3] == 0;
}

static bool
has_hex_suffix(const char *name)
{
	const char *dot = SDL_strrchr(name, '.');
	return dot && SDL_strcasecmp(dot, ".hex") == 0;
}

static void
join(char *out, size_t out_size, const char *dir, const char *name)
{
	size_t n = SDL_strlen(dir);

	/* SDL_EnumerateDirectory hands back the directory with a trailing
	 * separator, which on Windows is a backslash - accept either. */
	if (n && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
		SDL_snprintf(out, out_size, "%s%s", dir, name);
	else
		SDL_snprintf(out, out_size, "%s/%s", dir, name);
}

void
tj_path_dirname(char *out, size_t out_size, const char *path)
{
	char tmp[1024];
	SDL_snprintf(tmp, sizeof(tmp), "%s", path);
	normalise(tmp);

	char *slash = SDL_strrchr(tmp, '/');
	if (!slash) {
		out[0] = 0;
		return;
	}
	/* Keep the trailing slash of a drive/filesystem root. */
	if (slash == tmp || (slash == tmp + 2 && tmp[1] == ':'))
		slash[1] = 0;
	else
		*slash = 0;

	SDL_snprintf(out, out_size, "%s", tmp);
}

/* ------------------------------------------------------------------ */
/* Entry list                                                          */
/* ------------------------------------------------------------------ */

static tj_entry_t *
push_entry(tj_browser_t *b)
{
	if (b->count == b->capacity) {
		int cap = b->capacity ? b->capacity * 2 : 128;
		tj_entry_t *items = (tj_entry_t *)SDL_realloc(b->items,
				(size_t)cap * sizeof(tj_entry_t));
		if (!items)
			return NULL;
		b->items = items;
		b->capacity = cap;

		int *view = (int *)SDL_realloc(b->view, (size_t)cap * sizeof(int));
		if (!view)
			return NULL;
		b->view = view;
	}
	tj_entry_t *e = &b->items[b->count++];
	SDL_memset(e, 0, sizeof(*e));
	return e;
}

static int
compare_entries(const void *pa, const void *pb)
{
	const tj_entry_t *a = (const tj_entry_t *)pa;
	const tj_entry_t *b = (const tj_entry_t *)pb;

	if (a->is_parent != b->is_parent)
		return a->is_parent ? -1 : 1;
	if (a->is_dir != b->is_dir)
		return a->is_dir ? -1 : 1;
	return SDL_strcasecmp(a->label, b->label);
}

static void
rebuild_view(tj_browser_t *b)
{
	b->view_count = 0;
	for (int i = 0; i < b->count; i++) {
		if (b->filter[0] && !b->items[i].is_parent) {
			if (!SDL_strcasestr(b->items[i].label, b->filter))
				continue;
		}
		b->view[b->view_count++] = i;
	}
	if (b->sel >= b->view_count)
		b->sel = b->view_count ? b->view_count - 1 : 0;
	if (b->sel < 0)
		b->sel = 0;
}

/* ------------------------------------------------------------------ */
/* Scanning                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
	tj_browser_t *b;
	const char   *root;       /* for relative labels in recursive mode */
	int           depth;
	int           dirs_seen;
} scan_ctx_t;

static SDL_EnumerationResult scan_flat_cb(void *userdata, const char *dirname,
										  const char *fname);
static void scan_recursive(scan_ctx_t *ctx, const char *dir);

static SDL_EnumerationResult
scan_flat_cb(void *userdata, const char *dirname, const char *fname)
{
	scan_ctx_t *ctx = (scan_ctx_t *)userdata;
	tj_browser_t *b = ctx->b;
	char full[1024];
	SDL_PathInfo info;

	join(full, sizeof(full), b->dir, fname);
	if (!SDL_GetPathInfo(full, &info))
		return SDL_ENUM_CONTINUE;

	bool is_dir = (info.type == SDL_PATHTYPE_DIRECTORY);
	if (!is_dir && !has_hex_suffix(fname))
		return SDL_ENUM_CONTINUE;

	tj_entry_t *e = push_entry(b);
	if (!e)
		return SDL_ENUM_FAILURE;

	SDL_snprintf(e->name, sizeof(e->name), "%s", fname);
	SDL_snprintf(e->label, sizeof(e->label), "%s", fname);
	SDL_snprintf(e->path, sizeof(e->path), "%s", full);
	e->is_dir = is_dir;
	e->size = is_dir ? 0 : (uint64_t)info.size;

	(void)dirname;
	return SDL_ENUM_CONTINUE;
}

static SDL_EnumerationResult
scan_recursive_cb(void *userdata, const char *dirname, const char *fname)
{
	scan_ctx_t *ctx = (scan_ctx_t *)userdata;
	tj_browser_t *b = ctx->b;
	char full[1024];
	SDL_PathInfo info;

	if (b->count >= SCAN_MAX_ENTRIES || ctx->dirs_seen >= SCAN_MAX_DIRS) {
		b->truncated = true;
		return SDL_ENUM_SUCCESS;
	}

	join(full, sizeof(full), dirname, fname);
	normalise(full);
	if (!SDL_GetPathInfo(full, &info))
		return SDL_ENUM_CONTINUE;

	if (info.type == SDL_PATHTYPE_DIRECTORY) {
		if (ctx->depth < SCAN_MAX_DEPTH) {
			ctx->depth++;
			ctx->dirs_seen++;
			scan_recursive(ctx, full);
			ctx->depth--;
		}
		return SDL_ENUM_CONTINUE;
	}

	if (!has_hex_suffix(fname))
		return SDL_ENUM_CONTINUE;

	tj_entry_t *e = push_entry(b);
	if (!e)
		return SDL_ENUM_FAILURE;

	SDL_snprintf(e->name, sizeof(e->name), "%s", fname);
	SDL_snprintf(e->path, sizeof(e->path), "%s", full);
	e->is_dir = false;
	e->size = (uint64_t)info.size;

	/* Label with the path relative to the scan root - that is what tells
	 * "tiny-pacman-v1.2" apart from the half dozen other pacman builds. */
	size_t rootlen = SDL_strlen(ctx->root);
	const char *rel = full;
	if (rootlen && SDL_strncmp(full, ctx->root, rootlen) == 0) {
		rel = full + rootlen;
		while (*rel == '/')
			rel++;
	}
	SDL_snprintf(e->label, sizeof(e->label), "%s", rel);

	return SDL_ENUM_CONTINUE;
}

static void
scan_recursive(scan_ctx_t *ctx, const char *dir)
{
	SDL_EnumerateDirectory(dir, scan_recursive_cb, ctx);
}

#ifdef SDL_PLATFORM_WINDOWS
static void
scan_drives(tj_browser_t *b)
{
	for (char letter = 'A'; letter <= 'Z'; letter++) {
		char root[8];
		SDL_snprintf(root, sizeof(root), "%c:/", letter);

		SDL_PathInfo info;
		if (!SDL_GetPathInfo(root, &info) || info.type != SDL_PATHTYPE_DIRECTORY)
			continue;

		tj_entry_t *e = push_entry(b);
		if (!e)
			return;
		SDL_snprintf(e->name, sizeof(e->name), "%c:", letter);
		SDL_snprintf(e->label, sizeof(e->label), "%c:/", letter);
		SDL_snprintf(e->path, sizeof(e->path), "%s", root);
		e->is_dir = true;
	}
	SDL_snprintf(b->status, sizeof(b->status), "Drives");
}
#endif

void
tj_browser_refresh(tj_browser_t *b)
{
	char keep[1024] = { 0 };
	if (b->view_count && b->sel < b->view_count)
		SDL_snprintf(keep, sizeof(keep), "%s", b->items[b->view[b->sel]].path);

	b->count = 0;
	b->truncated = false;
	b->status[0] = 0;

	if (!b->dir[0]) {
#ifdef SDL_PLATFORM_WINDOWS
		scan_drives(b);
#endif
		rebuild_view(b);
		return;
	}

	if (b->recursive) {
		scan_ctx_t ctx = { b, b->dir, 0, 0 };
		scan_recursive(&ctx, b->dir);
		SDL_snprintf(b->status, sizeof(b->status), "%d .hex found%s",
					 b->count, b->truncated ? " (limit reached)" : "");
	} else {
		/*
		 * Always offer "..": from a drive root it leads to the drive list,
		 * which is the only way to reach another drive from the keyboard.
		 */
		tj_entry_t *e = push_entry(b);
		if (e) {
			SDL_snprintf(e->name, sizeof(e->name), "..");
			SDL_snprintf(e->label, sizeof(e->label), "..");
			e->is_dir = true;
			e->is_parent = true;
		}
		scan_ctx_t ctx = { b, b->dir, 0, 0 };
		SDL_EnumerateDirectory(b->dir, scan_flat_cb, &ctx);
	}

	SDL_qsort(b->items, (size_t)b->count, sizeof(tj_entry_t), compare_entries);
	rebuild_view(b);

	if (keep[0])
		tj_browser_select_path(b, keep);
}

void
tj_browser_open(tj_browser_t *b, const char *dir)
{
	char want[1024];

	SDL_snprintf(want, sizeof(want), "%s", dir ? dir : "");
	normalise(want);

	/* Drop a trailing slash except on a root. */
	size_t n = SDL_strlen(want);
	while (n > 1 && want[n - 1] == '/' && !is_drive_root(want)) {
		want[--n] = 0;
	}

	if (want[0]) {
		SDL_PathInfo info;
		if (!SDL_GetPathInfo(want, &info) || info.type != SDL_PATHTYPE_DIRECTORY)
			want[0] = 0;
	}

	if (!want[0]) {
		/* Fall back to the working directory. */
		char *cwd = SDL_GetCurrentDirectory();
		if (cwd) {
			SDL_snprintf(want, sizeof(want), "%s", cwd);
			SDL_free(cwd);
			normalise(want);
			n = SDL_strlen(want);
			while (n > 1 && want[n - 1] == '/' && !is_drive_root(want))
				want[--n] = 0;
		}
	}

#ifndef SDL_PLATFORM_WINDOWS
	/* Only Windows has a drive list to fall back to; elsewhere use the root. */
	if (!want[0])
		SDL_snprintf(want, sizeof(want), "/");
#endif

	SDL_snprintf(b->dir, sizeof(b->dir), "%s", want);
	b->sel = 0;
	b->scroll = 0;
	b->filter[0] = 0;
	tj_browser_refresh(b);
}

void
tj_browser_init(tj_browser_t *b)
{
	SDL_memset(b, 0, sizeof(*b));
}

void
tj_browser_free(tj_browser_t *b)
{
	SDL_free(b->items);
	SDL_free(b->view);
	SDL_memset(b, 0, sizeof(*b));
}

void
tj_browser_set_recursive(tj_browser_t *b, bool recursive)
{
	if (b->recursive == recursive)
		return;
	b->recursive = recursive;
	b->sel = 0;
	b->scroll = 0;
	tj_browser_refresh(b);
}

/* ------------------------------------------------------------------ */
/* Navigation                                                          */
/* ------------------------------------------------------------------ */

int
tj_browser_visible_rows(void)
{
	/* One row is reserved at the top of the body for the path line. */
	return (TJ_BODY_H - ROW_H - 4) / ROW_H;
}

static void
clamp_scroll(tj_browser_t *b)
{
	int rows = tj_browser_visible_rows();

	if (b->sel < b->scroll)
		b->scroll = b->sel;
	if (b->sel >= b->scroll + rows)
		b->scroll = b->sel - rows + 1;
	if (b->scroll > b->view_count - rows)
		b->scroll = b->view_count - rows;
	if (b->scroll < 0)
		b->scroll = 0;
}

void
tj_browser_move_to(tj_browser_t *b, int index)
{
	if (b->view_count <= 0) {
		b->sel = 0;
		b->scroll = 0;
		return;
	}
	if (index < 0)
		index = 0;
	if (index >= b->view_count)
		index = b->view_count - 1;
	b->sel = index;
	clamp_scroll(b);
}

void
tj_browser_move(tj_browser_t *b, int delta)
{
	tj_browser_move_to(b, b->sel + delta);
}

void
tj_browser_up(tj_browser_t *b)
{
	if (!b->dir[0])
		return;

	char parent[1024];
	if (is_drive_root(b->dir)) {
#ifdef SDL_PLATFORM_WINDOWS
		parent[0] = 0;                 /* up from C:/ is the drive list */
#else
		return;
#endif
	} else {
		tj_path_dirname(parent, sizeof(parent), b->dir);
		if (!parent[0])
			return;
	}

	char was[1024];
	SDL_snprintf(was, sizeof(was), "%s", b->dir);

	tj_browser_open(b, parent);
	tj_browser_select_path(b, was);   /* land on the directory we came from */
}

const char *
tj_browser_activate(tj_browser_t *b)
{
	if (b->view_count <= 0 || b->sel >= b->view_count)
		return NULL;

	tj_entry_t *e = &b->items[b->view[b->sel]];

	if (e->is_parent) {
		tj_browser_up(b);
		return NULL;
	}
	if (e->is_dir) {
		char path[1024];
		SDL_snprintf(path, sizeof(path), "%s", e->path);
		tj_browser_open(b, path);
		return NULL;
	}

	/* Entry pointers survive: nothing rescans on a plain file activation. */
	return e->path;
}

void
tj_browser_select_path(tj_browser_t *b, const char *path)
{
	char want[1024];
	SDL_snprintf(want, sizeof(want), "%s", path);
	normalise(want);

	size_t n = SDL_strlen(want);
	while (n > 1 && want[n - 1] == '/')
		want[--n] = 0;

	for (int i = 0; i < b->view_count; i++) {
		if (SDL_strcasecmp(b->items[b->view[i]].path, want) == 0) {
			tj_browser_move_to(b, i);
			return;
		}
	}
}

void
tj_browser_filter_append(tj_browser_t *b, const char *text)
{
	size_t len = SDL_strlen(b->filter);
	SDL_snprintf(b->filter + len, sizeof(b->filter) - len, "%s", text);
	b->sel = 0;
	b->scroll = 0;
	rebuild_view(b);
	clamp_scroll(b);
}

void
tj_browser_filter_backspace(tj_browser_t *b)
{
	size_t len = SDL_strlen(b->filter);
	if (!len)
		return;
	b->filter[len - 1] = 0;
	rebuild_view(b);
	clamp_scroll(b);
}

void
tj_browser_filter_clear(tj_browser_t *b)
{
	if (!b->filter[0])
		return;
	b->filter[0] = 0;
	rebuild_view(b);
	clamp_scroll(b);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void
format_size(char *out, size_t out_size, uint64_t bytes)
{
	if (bytes < 1024)
		SDL_snprintf(out, out_size, "%uB", (unsigned)bytes);
	else
		SDL_snprintf(out, out_size, "%uK", (unsigned)((bytes + 1023) / 1024));
}

void
tj_browser_render(tj_browser_t *b, SDL_Renderer *r)
{
	char buf[600];
	int rows = tj_browser_visible_rows();
	float y = TJ_BODY_Y + 2;

	/* Current location, plus the live filter if one is being typed. */
	if (b->filter[0]) {
		tj_ui_textf(r, 6, y, TJ_COL_ACCENT, "find: %s_", b->filter);
	} else {
		tj_ui_elide_middle(buf, sizeof(buf), b->dir[0] ? b->dir : "(drives)", 62);
		tj_ui_text(r, 6, y, TJ_COL_DIM, buf);
	}
	y += ROW_H + 2;

	if (b->view_count == 0) {
		tj_ui_text(r, 6, y + 8, TJ_COL_DIM,
				   b->filter[0] ? "Nothing matches that filter."
								: "No .hex files or folders here.");
		return;
	}

	for (int i = 0; i < rows; i++) {
		int vi = b->scroll + i;
		if (vi >= b->view_count)
			break;

		tj_entry_t *e = &b->items[b->view[vi]];
		bool selected = (vi == b->sel);
		float row_y = y + i * ROW_H;

		if (selected)
			tj_ui_fill(r, 2, row_y - 2, TJ_LOGICAL_W - 4, ROW_H, TJ_COL_SELECT);

		tj_color_t col = (!selected && e->is_dir) ? TJ_COL_ACCENT : TJ_COL_TEXT;

		char size_txt[16] = { 0 };
		int name_chars = 60;
		if (!e->is_dir) {
			format_size(size_txt, sizeof(size_txt), e->size);
			name_chars = 54;
		}

		if (e->is_dir)
			SDL_snprintf(buf, sizeof(buf), "[%s]", e->label);
		else
			SDL_snprintf(buf, sizeof(buf), "%s", e->label);

		char shown[600];
		tj_ui_elide_middle(shown, sizeof(shown), buf, name_chars);
		tj_ui_text(r, 8, row_y, col, shown);

		if (size_txt[0]) {
			float x = TJ_LOGICAL_W - 10 - (float)SDL_strlen(size_txt) * TJ_CHAR_W;
			tj_ui_text(r, x, row_y, selected ? TJ_COL_TEXT : TJ_COL_DIM, size_txt);
		}
	}

	/* Scroll position indicator. */
	if (b->view_count > rows) {
		float track_h = (float)(rows * ROW_H);
		float thumb_h = track_h * rows / b->view_count;
		if (thumb_h < 8)
			thumb_h = 8;
		float t = (float)b->scroll / (float)(b->view_count - rows);
		tj_ui_fill(r, TJ_LOGICAL_W - 3, y - 2, 2, track_h, TJ_COL_BAR);
		tj_ui_fill(r, TJ_LOGICAL_W - 3, y - 2 + t * (track_h - thumb_h), 2,
				   thumb_h, TJ_COL_DIM);
	}
}
