#ifndef MOTU_H
#define MOTU_H

#include <stdint.h>

/* The platform layer, one implementation per platform behind this header.
 *
 * Threads: calls marked [any] may come from any thread; every other call is
 * made on the main thread, where kaikai runs `main`. Callbacks into kaikai
 * (the motu_on_* functions) are always made on the main thread, from inside
 * motu_run.
 *
 * Windows are numbered from 1; 0 is "no window" or "the application".
 * Strings passed in are copied; strings passed to callbacks are valid only
 * for the duration of the callback. */

/* --- application --- */

/* `ud` is passed back to every application callback. */
void motu_init(void *ud, const char *app_name);
/* Runs the event loop until motu_quit. */
void motu_run(void);
/* [any] Ends motu_run. */
void motu_quit(void);
/* Files under `root` are served as motu://app/<path>. */
void motu_set_asset_root(const char *root);
/* [any] Files under `root` are served as motu://files/<path>; "" serves none. */
void motu_set_files_root(const char *root);

/* --- windows --- */

/* [any] */
int64_t motu_window_new(const char *title, int64_t width, int64_t height);
/* [any] */
void motu_window_load(int64_t win, const char *url);
/* [any] */
void motu_window_set_title(int64_t win, const char *title);
/* [any] */
void motu_window_set_min_size(int64_t win, int64_t width, int64_t height);
/* [any] */
void motu_window_set_fullscreen(int64_t win, int64_t on);
/* [any] */
void motu_window_focus(int64_t win);
/* [any] Closes without asking: the close request has already been decided.
 * Reports "closed"; closing the last window ends motu_run. */
void motu_window_close(int64_t win);
/* [any] Delivers one JSON value to the page: window.__motu_recv(json). */
void motu_post(int64_t win, const char *json);

/* --- menus ---
 * The menu bar is built depth-first and replaces the previous one on
 * motu_menu_install. `key` is the accelerator: one character, upper case
 * meaning shift; "" for none. The primary modifier (Command, Control) is
 * implied. flags: MOTU_MENU_DISABLED, MOTU_MENU_CHECKED. An item with the id
 * of a standard action (MOTU_ACTION_*) gets the platform's own behaviour. */

#define MOTU_MENU_DISABLED 1
#define MOTU_MENU_CHECKED 2

void motu_menu_begin(const char *title);
void motu_menu_item(const char *id, const char *title, const char *key, int64_t flags);
void motu_menu_separator(void);
void motu_menu_end(void);
void motu_menu_install(void);
/* [any] */
void motu_menu_set_state(const char *id, int64_t flags);

/* Standard actions: "undo", "redo", "cut", "copy", "paste", "select-all",
 * "quit", "close-window", "fullscreen", "minimize". */

/* --- dialogs ---
 * [any] Asynchronous: the answer arrives through motu_on_reply with the same
 * `reply`. Open and save answer a JSON array of paths, [] when cancelled;
 * message answers the index of the button pressed. */

#define MOTU_OPEN_DIRECTORIES 1
#define MOTU_OPEN_MULTIPLE 2

/* `extensions`: comma-separated, without dots; "" for any file. */
void motu_dialog_open(void *reply, int64_t win, const char *title, int64_t flags, const char *extensions);
void motu_dialog_save(void *reply, int64_t win, const char *title, const char *suggested_name);
/* `buttons`: newline-separated labels, the first is the default. */
void motu_dialog_message(void *reply, int64_t win, const char *title, const char *message, const char *buttons);

/* --- shell, clipboard, files --- */

/* [any] */
void motu_shell_open(const char *path);
/* [any] */
void motu_shell_reveal(const char *path);
/* [any] */
void motu_clipboard_set_text(const char *text);
/* [any] Answers a JSON string through motu_on_reply. */
void motu_clipboard_get_text(void *reply);
/* [any] Flushes a written file to the disk; 0 on success. */
int64_t motu_fsync_path(const char *path);
/* [any] The platform's per-user data directory, without the app name. */
const char *motu_data_dir(void);
/* [any] The directory holding the running executable. */
const char *motu_exe_dir(void);

/* --- printing ---
 * [any] Renders the page at `url` in an offscreen webview and writes it to
 * `out_path` as PDF; answers true or false through motu_on_reply. */
void motu_print_pdf(void *reply, const char *url, const char *out_path);

/* --- callbacks, implemented by the kaikai side --- */

extern void motu_on_message(void *ud, int64_t win, const char *json);
/* kind: "close-requested", "closed", "quit-requested", "focus", "blur",
 * "resize". */
extern void motu_on_window_event(void *ud, int64_t win, const char *kind);
extern void motu_on_menu(void *ud, const char *id);
/* `paths`: a JSON array of the dropped files. */
extern void motu_on_drop(void *ud, int64_t win, const char *paths);
extern void motu_on_reply(void *reply, const char *json);

#endif
