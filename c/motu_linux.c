/* Linux: GTK 4 and WebKitGTK 6.0, loaded with dlopen so the shim needs no GTK
 * headers or link flags (see motu_linux_api.h). */
#ifdef __linux__

#define _GNU_SOURCE

#include "motu.h"

#include <ctype.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "motu_linux.h"

#define DEFINE(ret, name, params) HIDDEN ret (*name) params;
MOTU_LIBS(DEFINE)
#undef DEFINE

static const char *LIBS[][2] = {
  {"libglib-2.0.so.0", "libglib2.0-0"},
  {"libgobject-2.0.so.0", "libglib2.0-0"},
  {"libgio-2.0.so.0", "libglib2.0-0"},
  {"libgtk-4.so.1", "libgtk-4-1"},
  {"libjavascriptcoregtk-6.0.so.1", "libjavascriptcoregtk-6.0-1"},
  {"libsoup-3.0.so.0", "libsoup-3.0-0"},
  {"libwebkitgtk-6.0.so.4", "libwebkitgtk-6.0-4"},
};

static void load_libraries(void) {
  for (size_t i = 0; i < sizeof LIBS / sizeof LIBS[0]; i++)
    if (!dlopen(LIBS[i][0], RTLD_NOW | RTLD_GLOBAL)) {
      fprintf(stderr, "motu: cannot load %s; install %s (Debian/Ubuntu package name)\n", LIBS[i][0], LIBS[i][1]);
      exit(1);
    }
  void *sym;
#define RESOLVE(ret, name, params) \
  sym = dlsym(RTLD_DEFAULT, #name); \
  if (!sym) { fprintf(stderr, "motu: %s not found; GTK 4.10 and WebKitGTK 6.0 or newer are required\n", #name); exit(1); } \
  memcpy(&name, &sym, sizeof sym);
  MOTU_LIBS(RESOLVE)
#undef RESOLVE
}

#define WIN_DATA(win) ((void *)(intptr_t)(win))
#define DATA_WIN(p) ((int64_t)(intptr_t)(p))

static void *g_ud;
static char *g_root, *g_files_root;
static void *g_loop;
static pthread_t g_ui_thread;

#define MOTU_MAX_WINDOWS 64

typedef struct {
  void *window, *view, *box, *bar, *shortcuts;
  void *group;
  int resize_pending;
} Win;

static Win g_win[MOTU_MAX_WINDOWS + 1];

static int valid(int64_t win) {
  return win >= 1 && win <= MOTU_MAX_WINDOWS && g_win[win].window;
}

static int on_ui_thread(void) {
  return pthread_equal(pthread_self(), g_ui_thread);
}

/* --- threads ---
 * GTK runs on a thread of its own: kaikai's main fiber may be on a stack that
 * JavaScriptCore does not recognise as the thread's, and aborts on. Calls are
 * queued to the GTK thread as jobs; callbacks are queued back to the kaikai
 * main thread and made from inside motu_run. */


static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static Job *g_jobs_head, *g_jobs_tail;

static char *dup_or_null(const char *s) { return s ? strdup(s) : 0; }

Job *lx_job(void (*fn)(Job *), void *p, int64_t a, int64_t b, const char *s0, const char *s1, const char *s2) {
  Job *j = calloc(1, sizeof *j);
  j->fn = fn;
  j->p = p;
  j->a = a;
  j->b = b;
  j->s[0] = dup_or_null(s0);
  j->s[1] = dup_or_null(s1);
  j->s[2] = dup_or_null(s2);
  return j;
}

static void run_job(Job *j) {
  j->fn(j);
  for (int i = 0; i < 3; i++) free(j->s[i]);
  free(j);
}

static int drain_jobs(void *unused) {
  (void)unused;
  pthread_mutex_lock(&g_jobs_lock);
  Job *j = g_jobs_head;
  g_jobs_head = g_jobs_tail = 0;
  pthread_mutex_unlock(&g_jobs_lock);
  while (j) {
    Job *next = j->next;
    run_job(j);
    j = next;
  }
  return 0;
}

/* Queued jobs run in the order they were queued. */
static void queue_job(Job *j) {
  pthread_mutex_lock(&g_jobs_lock);
  int wake = g_jobs_head == 0;
  if (g_jobs_tail) g_jobs_tail->next = j;
  else g_jobs_head = j;
  g_jobs_tail = j;
  pthread_mutex_unlock(&g_jobs_lock);
  if (wake) g_idle_add_full(PRIORITY_DEFAULT, drain_jobs, 0, 0);
}

void lx_on_ui(Job *j) {
  if (on_ui_thread()) run_job(j);
  else queue_job(j);
}

typedef struct { int64_t result; int done; } Sync;

static pthread_mutex_t g_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sync_cond = PTHREAD_COND_INITIALIZER;

static void sync_done(Sync *s, int64_t result) {
  pthread_mutex_lock(&g_sync_lock);
  s->result = result;
  s->done = 1;
  pthread_cond_broadcast(&g_sync_cond);
  pthread_mutex_unlock(&g_sync_lock);
}

static int64_t sync_wait(Sync *s) {
  pthread_mutex_lock(&g_sync_lock);
  while (!s->done) pthread_cond_wait(&g_sync_cond, &g_sync_lock);
  pthread_mutex_unlock(&g_sync_lock);
  return s->result;
}


typedef struct Callback Callback;
struct Callback { Callback *next; int kind; void *reply; int64_t win; char *text; };

static pthread_mutex_t g_cb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cb_cond = PTHREAD_COND_INITIALIZER;
static Callback *g_cb_head, *g_cb_tail;

void lx_deliver(int kind, void *reply, int64_t win, const char *text) {
  Callback *c = calloc(1, sizeof *c);
  c->kind = kind;
  c->reply = reply;
  c->win = win;
  c->text = strdup(text ? text : "");
  pthread_mutex_lock(&g_cb_lock);
  if (g_cb_tail) g_cb_tail->next = c;
  else g_cb_head = c;
  g_cb_tail = c;
  pthread_cond_signal(&g_cb_cond);
  pthread_mutex_unlock(&g_cb_lock);
}

static void call_back(Callback *c) {
  switch (c->kind) {
    case CB_MESSAGE: motu_on_message(g_ud, c->win, c->text); break;
    case CB_WINDOW: motu_on_window_event(g_ud, c->win, c->text); break;
    case CB_MENU: motu_on_menu(g_ud, c->text); break;
    case CB_DROP: motu_on_drop(g_ud, c->win, c->text); break;
    case CB_REPLY: motu_on_reply(c->reply, c->text); break;
    default: break;
  }
}

void motu_run(void) {
  int stop = 0;
  while (!stop) {
    pthread_mutex_lock(&g_cb_lock);
    while (!g_cb_head) pthread_cond_wait(&g_cb_cond, &g_cb_lock);
    Callback *c = g_cb_head;
    g_cb_head = g_cb_tail = 0;
    pthread_mutex_unlock(&g_cb_lock);
    while (c) {
      Callback *next = c->next;
      if (c->kind == CB_STOP) stop = 1;
      if (!stop) call_back(c);
      free(c->text);
      free(c);
      c = next;
    }
  }
}

/* --- JSON text for callbacks --- */


void lx_buf_put(Buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = '\0';
}

void lx_buf_json_string(Buf *b, const char *s) {
  lx_buf_put(b, "\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    char esc[8];
    if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; lx_buf_put(b, esc, 2); }
    else if (c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", c); lx_buf_put(b, esc, 6); }
    else lx_buf_put(b, (const char *)&c, 1);
  }
  lx_buf_put(b, "\"", 1);
}

static void buf_path_of(Buf *b, void *file) {
  char *path = file ? g_file_get_path(file) : 0;
  if (!path) return;
  if (b->n > 1) lx_buf_put(b, ",", 1);
  lx_buf_json_string(b, path);
  g_free(path);
}

/* A JSON array of the paths of `file`, or of every GFile in the list model `files`. */
char *lx_paths_json(void *file, void *files) {
  Buf b = {0};
  lx_buf_put(&b, "[", 1);
  buf_path_of(&b, file);
  unsigned n = files ? g_list_model_get_n_items(files) : 0;
  for (unsigned i = 0; i < n; i++) {
    void *f = g_list_model_get_item(files, i);
    buf_path_of(&b, f);
    g_object_unref(f);
  }
  lx_buf_put(&b, "]", 1);
  return b.p;
}

void lx_reply_owned(void *reply, char *json) {
  lx_deliver(CB_REPLY, reply, 0, json);
  free(json);
}

void lx_clear_error(GError **err) {
  if (*err) g_error_free(*err);
  *err = 0;
}

/* --- the page's client --- */

static const char BRIDGE_JS[] =
  "(() => {"
  " const pending = new Map(); const listeners = new Map(); let next = 1;"
  " const send = (m) => window.webkit.messageHandlers.motu.postMessage(JSON.stringify(m));"
  " window.__motu_recv = (m) => {"
  "  if (m.t === 'reply') { const p = pending.get(m.id); if (!p) return; pending.delete(m.id);"
  "   if ('err' in m) p.reject(new Error(m.err)); else p.resolve(m.ok); }"
  "  else if (m.t === 'event') { (listeners.get(m.name) || []).forEach((f) => f(m.data, m.id)); }"
  " };"
  " window.motu = {"
  "  invoke(cmd, args) { const id = next++; send({ t: 'call', id, cmd, args: args === undefined ? null : args });"
  "   const p = new Promise((resolve, reject) => pending.set(id, { resolve, reject })); p.id = id; return p; },"
  "  cancel(p) { send({ t: 'cancel', id: typeof p === 'number' ? p : p.id }); },"
  "  on(name, f) { if (!listeners.has(name)) listeners.set(name, []); listeners.get(name).push(f); },"
  " };"
  "})();";

static void on_script_message(void *ucm, void *value, void *data) {
  (void)ucm;
  char *text = jsc_value_to_string(value);
  lx_deliver(CB_MESSAGE, 0, DATA_WIN(data), text ? text : "");
  g_free(text);
}

/* --- motu:// --- */

static const char *mime_of(const char *path) {
  const char *dot = strrchr(path, '.');
  static const char *table[][2] = {
    {".html", "text/html"}, {".js", "text/javascript"}, {".mjs", "text/javascript"},
    {".css", "text/css"}, {".json", "application/json"}, {".wasm", "application/wasm"},
    {".svg", "image/svg+xml"}, {".png", "image/png"}, {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
    {".woff2", "font/woff2"}, {".pdf", "application/pdf"},
    {".md", "text/markdown"}, {".txt", "text/plain"},
  };
  if (dot)
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
      if (strcmp(dot, table[i][0]) == 0) return table[i][1];
  return "application/octet-stream";
}

/* Takes ownership of `data` when `release` is given. */
static void scheme_respond(void *req, unsigned code, const char *reason, const char *mime,
    char *data, size_t len, void (*release)(void *)) {
  void *stream = g_memory_input_stream_new_from_data(data, (long)len, release);
  void *resp = webkit_uri_scheme_response_new(stream, (int64_t)len);
  webkit_uri_scheme_response_set_status(resp, code, reason);
  webkit_uri_scheme_response_set_content_type(resp, mime);
  /* The page lives on motu://app; without this its fetches of motu://files fail CORS. */
  void *headers = soup_message_headers_new(SOUP_HEADERS_RESPONSE);
  soup_message_headers_append(headers, "Access-Control-Allow-Origin", "motu://app");
  webkit_uri_scheme_response_set_http_headers(resp, headers);
  webkit_uri_scheme_request_finish_with_response(req, resp);
  g_object_unref(resp);
  g_object_unref(stream);
}

static void scheme_fail(void *req, unsigned code, const char *reason) {
  static char empty[1];
  scheme_respond(req, code, reason, "text/plain", empty, 0, 0);
}

/* Host "files" serves the files root as is; any other host serves the asset
 * root, with index.html for "/". Both roots change only on the GTK thread. */
static void on_scheme_request(void *req, void *ud) {
  (void)ud;
  int app = strncmp(webkit_uri_scheme_request_get_uri(req), "motu://files/", 13) != 0;
  const char *root = app ? g_root : g_files_root;
  char *path = g_uri_unescape_string(webkit_uri_scheme_request_get_path(req), 0);
  if (!path) { scheme_fail(req, 400, "Bad Request"); return; }
  if (!root || !root[0] || strstr(path, "..")) { g_free(path); scheme_fail(req, 403, "Forbidden"); return; }
  const char *rel = app && (path[0] == '\0' || strcmp(path, "/") == 0) ? "/index.html" : path;
  size_t n = strlen(root) + strlen(rel) + 2;
  char *full = malloc(n);
  snprintf(full, n, "%s%s%s", root, rel[0] == '/' ? "" : "/", rel);
  char *data = 0;
  size_t len = 0;
  if (g_file_get_contents(full, &data, &len, 0)) scheme_respond(req, 200, "OK", mime_of(full), data, len, g_free);
  else scheme_fail(req, 404, "Not Found");
  free(full);
  g_free(path);
}

/* --- menus --- */

#define MOTU_MAX_ITEMS 256

typedef struct { char *id; char *accel; int64_t flags; } Item;

typedef struct {
  Item items[MOTU_MAX_ITEMS];
  int count;
  void *bar, *first;
} MenuBar;

static MenuBar g_building, g_menus;
static struct { void *menu, *section; } g_menu_stack[16];
static int g_menu_depth;

static void free_menubar(MenuBar *m) {
  for (int i = 0; i < m->count; i++) { free(m->items[i].id); free(m->items[i].accel); }
  if (m->bar) g_object_unref(m->bar);
  memset(m, 0, sizeof *m);
}

static int item_index(const MenuBar *m, const char *id_) {
  for (int i = 0; i < m->count; i++)
    if (strcmp(m->items[i].id, id_) == 0) return i;
  return -1;
}

static int checkable(int64_t flags) { return (flags & (MOTU_MENU_CHECKED | MOTU_MENU_CHECKABLE)) != 0; }

static void action_name(char *out, size_t n, int index, int qualified) {
  snprintf(out, n, "%sm%d", qualified ? "motu." : "", index);
}

/* "<Control><Shift>z" for "Z"; 0 for none. */
static char *accel_of(const char *key) {
  if (!key || !key[0]) return 0;
  unsigned char c = (unsigned char)key[0];
  int shift = isupper(c);
  const char *name = gdk_keyval_name(gdk_unicode_to_keyval((uint32_t)tolower(c)));
  if (!name) return 0;
  char buf[64];
  snprintf(buf, sizeof buf, "<Control>%s%s", shift ? "<Shift>" : "", name);
  return strdup(buf);
}

static const char *editing_command(const char *id_) {
  static const char *table[][2] = {
    {"undo", "Undo"}, {"redo", "Redo"}, {"cut", "Cut"}, {"copy", "Copy"},
    {"paste", "Paste"}, {"select-all", "SelectAll"},
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
    if (strcmp(id_, table[i][0]) == 0) return table[i][1];
  return 0;
}

static int standard_action(int64_t win, const char *id_) {
  const char *edit = editing_command(id_);
  if (edit) { if (valid(win)) webkit_web_view_execute_editing_command(g_win[win].view, edit); return 1; }
  if (strcmp(id_, "quit") == 0) { lx_deliver(CB_WINDOW, 0, 0, "quit-requested"); return 1; }
  if (strcmp(id_, "close-window") == 0) { if (valid(win)) lx_deliver(CB_WINDOW, 0, win, "close-requested"); return 1; }
  if (strcmp(id_, "fullscreen") == 0) {
    if (valid(win)) motu_window_set_fullscreen(win, !gtk_window_is_fullscreen(g_win[win].window));
    return 1;
  }
  if (strcmp(id_, "minimize") == 0) { if (valid(win)) gtk_window_minimize(g_win[win].window); return 1; }
  return 0;
}

static void on_action(void *action, void *param, void *data) {
  (void)action; (void)param;
  intptr_t code = (intptr_t)data;
  int index = (int)(code & 0xffff);
  int64_t win = code >> 16;
  if (index >= g_menus.count) return;
  const char *id_ = g_menus.items[index].id;
  if (!standard_action(win, id_)) lx_deliver(CB_MENU, 0, 0, id_);
}

static void *new_action(int64_t win, int index, int stateful) {
  char name[32];
  action_name(name, sizeof name, index, 0);
  int64_t flags = g_menus.items[index].flags;
  void *a = stateful
      ? g_simple_action_new_stateful(name, 0, g_variant_new_boolean((flags & MOTU_MENU_CHECKED) != 0))
      : g_simple_action_new(name, 0);
  g_simple_action_set_enabled(a, (flags & MOTU_MENU_DISABLED) == 0);
  CONNECT(a, "activate", on_action, (void *)(intptr_t)((win << 16) | index));
  return a;
}

static void attach_menus(int64_t win) {
  Win *w = &g_win[win];
  void *group = g_simple_action_group_new();
  void *shortcuts = gtk_shortcut_controller_new();
  gtk_event_controller_set_propagation_phase(shortcuts, PHASE_CAPTURE);
  for (int i = 0; i < g_menus.count; i++) {
    void *a = new_action(win, i, checkable(g_menus.items[i].flags));
    g_action_map_add_action(group, a);
    g_object_unref(a);
    if (g_menus.items[i].accel) {
      char name[32];
      action_name(name, sizeof name, i, 1);
      gtk_shortcut_controller_add_shortcut(shortcuts,
          gtk_shortcut_new(gtk_shortcut_trigger_parse_string(g_menus.items[i].accel), gtk_named_action_new(name)));
    }
  }
  gtk_widget_insert_action_group(w->window, "motu", group);
  w->group = group;
  if (w->shortcuts) gtk_widget_remove_controller(w->window, w->shortcuts);
  gtk_widget_add_controller(w->window, shortcuts);
  w->shortcuts = shortcuts;
  gtk_popover_menu_bar_set_menu_model(w->bar, g_menus.bar);
  g_object_unref(group);
}

static void *new_menu_with_section(void **section) {
  void *menu = g_menu_new();
  *section = g_menu_new();
  g_menu_append_section(menu, 0, *section);
  g_object_unref(*section);
  return menu;
}

static void menu_begin(const char *title) {
  if (g_menu_depth == 0) {
    free_menubar(&g_building);
    g_building.bar = g_menu_new();
    g_menu_stack[0].menu = g_building.bar;
    g_menu_stack[0].section = g_building.bar;
    g_menu_depth = 1;
  }
  if (g_menu_depth >= 16) return;
  void *section;
  void *sub = new_menu_with_section(&section);
  g_menu_append_submenu(g_menu_stack[g_menu_depth - 1].section, title, sub);
  g_object_unref(sub);
  if (!g_building.first) g_building.first = sub;
  g_menu_stack[g_menu_depth].menu = sub;
  g_menu_stack[g_menu_depth].section = section;
  g_menu_depth++;
}

static void menu_item(const char *id_, const char *title, const char *key, int64_t flags) {
  if (g_menu_depth <= 1 || g_building.count >= MOTU_MAX_ITEMS) return;
  int index = g_building.count++;
  Item *it = &g_building.items[index];
  it->id = strdup(id_);
  it->accel = accel_of(key);
  it->flags = flags;
  char name[32];
  action_name(name, sizeof name, index, 1);
  void *item = g_menu_item_new(title, name);
  if (it->accel) g_menu_item_set_attribute_value(item, "accel", g_variant_new_string(it->accel));
  g_menu_append_item(g_menu_stack[g_menu_depth - 1].section, item);
  g_object_unref(item);
}

static void menu_separator(void) {
  if (g_menu_depth <= 1) return;
  void *section = g_menu_new();
  g_menu_append_section(g_menu_stack[g_menu_depth - 1].menu, 0, section);
  g_object_unref(section);
  g_menu_stack[g_menu_depth - 1].section = section;
}

static void menu_end(void) {
  if (g_menu_depth > 1) g_menu_depth--;
}

/* Linux has no application menu: without a Quit item the bar gets one. */
static void add_quit(void) {
  if (!g_building.first || item_index(&g_building, "quit") >= 0) return;
  g_menu_stack[1].menu = g_menu_stack[1].section = g_building.first;
  g_menu_depth = 2;
  menu_separator();
  menu_item("quit", "Quit", "q", 0);
}

static void menu_install(void) {
  if (g_menu_depth < 1) return;
  add_quit();
  MenuBar old = g_menus;
  g_menus = g_building;
  memset(&g_building, 0, sizeof g_building);
  g_menu_depth = 0;
  for (int64_t i = 1; i <= MOTU_MAX_WINDOWS; i++)
    if (g_win[i].window) attach_menus(i);
  free_menubar(&old);
}

static void menu_begin_job(Job *j) { menu_begin(j->s[0]); }
static void menu_item_job(Job *j) { menu_item(j->s[0], j->s[1], j->s[2], j->a); }
static void menu_separator_job(Job *j) { (void)j; menu_separator(); }
static void menu_end_job(Job *j) { (void)j; menu_end(); }
static void menu_install_job(Job *j) { (void)j; menu_install(); }

void motu_menu_begin(const char *title) { lx_on_ui(lx_job(menu_begin_job, 0, 0, 0, title, 0, 0)); }
void motu_menu_item(const char *id_, const char *title, const char *key, int64_t flags) {
  lx_on_ui(lx_job(menu_item_job, 0, flags, 0, id_, title, key));
}
void motu_menu_separator(void) { lx_on_ui(lx_job(menu_separator_job, 0, 0, 0, 0, 0, 0)); }
void motu_menu_end(void) { lx_on_ui(lx_job(menu_end_job, 0, 0, 0, 0, 0, 0)); }
void motu_menu_install(void) { lx_on_ui(lx_job(menu_install_job, 0, 0, 0, 0, 0, 0)); }


static void menu_state_main(Job *j) {
  int index = item_index(&g_menus, j->s[0]);
  if (index < 0) return;
  g_menus.items[index].flags = j->a | (g_menus.items[index].flags & MOTU_MENU_CHECKABLE);
  char name[32];
  action_name(name, sizeof name, index, 0);
  for (int64_t win = 1; win <= MOTU_MAX_WINDOWS; win++) {
    if (!g_win[win].window || !g_win[win].group) continue;
    void *a = g_action_map_lookup_action(g_win[win].group, name);
    if (a && g_action_get_state_type(a)) {
      g_simple_action_set_enabled(a, (j->a & MOTU_MENU_DISABLED) == 0);
      g_simple_action_set_state(a, g_variant_new_boolean((j->a & MOTU_MENU_CHECKED) != 0));
    } else if (a && (j->a & MOTU_MENU_CHECKED)) {
      /* A checkmark needs a stateful action: replace the plain one. */
      g_action_map_remove_action(g_win[win].group, name);
      void *stateful = new_action(win, index, 1);
      g_action_map_add_action(g_win[win].group, stateful);
      g_object_unref(stateful);
    } else if (a) {
      g_simple_action_set_enabled(a, (j->a & MOTU_MENU_DISABLED) == 0);
    }
  }
}

void motu_menu_set_state(const char *id_, int64_t flags) {
  lx_on_ui(lx_job(menu_state_main, 0, flags, 0, id_, 0, 0));
}

/* --- windows --- */

static int64_t win_of(void *window, void *data) {
  int64_t win = DATA_WIN(data);
  return valid(win) && g_win[win].window == window ? win : 0;
}

static int on_close_request(void *window, void *data) {
  int64_t win = win_of(window, data);
  if (win) lx_deliver(CB_WINDOW, 0, win, "close-requested");
  return 1;
}

static void on_active(void *window, void *pspec, void *data) {
  (void)pspec;
  int64_t win = win_of(window, data);
  if (!win) return;
  int active = gtk_window_is_active(window);
  lx_deliver(CB_WINDOW, 0, win, active ? "focus" : "blur");
}

static int report_resize(void *data) {
  int64_t win = DATA_WIN(data);
  if (valid(win)) {
    g_win[win].resize_pending = 0;
    lx_deliver(CB_WINDOW, 0, win, "resize");
  }
  return 0;
}

/* Width and height change together: one "resize" for both. */
static void on_size(void *window, void *pspec, void *data) {
  (void)pspec;
  int64_t win = win_of(window, data);
  if (!win || g_win[win].resize_pending) return;
  g_win[win].resize_pending = 1;
  g_idle_add_full(PRIORITY_DEFAULT, report_resize, data, 0);
}

static int on_drop(void *target, const void *value, double x, double y, void *data) {
  (void)target; (void)x; (void)y;
  int64_t win = DATA_WIN(data);
  if (!valid(win)) return 0;
  Buf b = {0};
  lx_buf_put(&b, "[", 1);
  GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
  for (GSList *f = files; f; f = f->next) buf_path_of(&b, f->data);
  g_slist_free(files);
  lx_buf_put(&b, "]", 1);
  lx_deliver(CB_DROP, 0, win, b.p);
  free(b.p);
  return 1;
}

static void on_web_process_terminated(void *view, int reason, void *data) {
  (void)view; (void)data;
  fprintf(stderr, "motu: the web process ended (reason %d)\n", reason);
}

void *lx_new_web_view(void *ucm) {
  void *view = ucm
      ? g_object_new(webkit_web_view_get_type(), "user-content-manager", ucm, (char *)0)
      : g_object_new(webkit_web_view_get_type(), (char *)0);
  webkit_settings_set_enable_developer_extras(webkit_web_view_get_settings(view), 1);
  CONNECT(view, "web-process-terminated", on_web_process_terminated, 0);
  return view;
}

static void *page_content(int64_t win) {
  void *ucm = webkit_user_content_manager_new();
  webkit_user_content_manager_register_script_message_handler(ucm, "motu", 0);
  CONNECT(ucm, "script-message-received::motu", on_script_message, WIN_DATA(win));
  void *script = webkit_user_script_new(BRIDGE_JS, INJECT_TOP_FRAME, INJECT_AT_START, 0, 0);
  webkit_user_content_manager_add_script(ucm, script);
  webkit_user_script_unref(script);
  return ucm;
}

static int64_t window_new_main(const char *title, int64_t width, int64_t height) {
  int64_t win = 0;
  for (int64_t i = 1; i <= MOTU_MAX_WINDOWS; i++)
    if (!g_win[i].window) { win = i; break; }
  if (!win) return 0;
  Win *w = &g_win[win];
  memset(w, 0, sizeof *w);

  void *ucm = page_content(win);
  w->view = lx_new_web_view(ucm);
  g_object_unref(ucm);
  gtk_widget_set_vexpand(w->view, 1);
  gtk_widget_set_hexpand(w->view, 1);

  w->bar = gtk_popover_menu_bar_new_from_model(0);
  w->box = gtk_box_new(ORIENTATION_VERTICAL, 0);
  gtk_box_append(w->box, w->bar);
  gtk_box_append(w->box, w->view);

  /* Captured before the web view sees it, so only file lists stop here. */
  void *drop = gtk_drop_target_new(gdk_file_list_get_type(), ACTION_COPY);
  gtk_event_controller_set_propagation_phase(drop, PHASE_CAPTURE);
  CONNECT(drop, "drop", on_drop, WIN_DATA(win));
  gtk_widget_add_controller(w->box, drop);

  w->window = gtk_window_new();
  gtk_window_set_title(w->window, title);
  gtk_window_set_default_size(w->window, (int)width, (int)height);
  gtk_window_set_child(w->window, w->box);
  CONNECT(w->window, "close-request", on_close_request, WIN_DATA(win));
  CONNECT(w->window, "notify::is-active", on_active, WIN_DATA(win));
  CONNECT(w->window, "notify::default-width", on_size, WIN_DATA(win));
  CONNECT(w->window, "notify::default-height", on_size, WIN_DATA(win));
  attach_menus(win);
  gtk_window_present(w->window);
  return win;
}

static void window_new_job(Job *j) { sync_done(j->p, window_new_main(j->s[0], j->a, j->b)); }

int64_t motu_window_new(const char *title, int64_t width, int64_t height) {
  if (on_ui_thread()) return window_new_main(title, width, height);
  Sync s = {0, 0};
  queue_job(lx_job(window_new_job, &s, width, height, title, 0, 0));
  return sync_wait(&s);
}

static void load_main(Job *j) { if (valid(j->a)) webkit_web_view_load_uri(g_win[j->a].view, j->s[0]); }
void motu_window_load(int64_t win, const char *url) { lx_on_ui(lx_job(load_main, 0, win, 0, url, 0, 0)); }

static void title_main(Job *j) { if (valid(j->a)) gtk_window_set_title(g_win[j->a].window, j->s[0]); }
void motu_window_set_title(int64_t win, const char *title) { lx_on_ui(lx_job(title_main, 0, win, 0, title, 0, 0)); }

static void min_size_main(Job *j) {
  if (valid(j->a)) gtk_widget_set_size_request(g_win[j->a].view, (int)j->b, (int)j->c);
}
void motu_window_set_min_size(int64_t win, int64_t width, int64_t height) {
  Job *j = lx_job(min_size_main, 0, win, width, 0, 0, 0);
  j->c = height;
  lx_on_ui(j);
}

static void fullscreen_main(Job *j) {
  if (!valid(j->a)) return;
  if (j->b) gtk_window_fullscreen(g_win[j->a].window);
  else gtk_window_unfullscreen(g_win[j->a].window);
}
void motu_window_set_fullscreen(int64_t win, int64_t on) { lx_on_ui(lx_job(fullscreen_main, 0, win, on, 0, 0, 0)); }

static void focus_main(Job *j) { if (valid(j->a)) gtk_window_present(g_win[j->a].window); }
void motu_window_focus(int64_t win) { lx_on_ui(lx_job(focus_main, 0, win, 0, 0, 0, 0)); }

static void close_main(Job *j) {
  int64_t win = j->a;
  if (!valid(win)) return;
  void *window = g_win[win].window;
  memset(&g_win[win], 0, sizeof g_win[win]);
  gtk_window_destroy(window);
  lx_deliver(CB_WINDOW, 0, win, "closed");
  for (int64_t i = 1; i <= MOTU_MAX_WINDOWS; i++)
    if (g_win[i].window) return;
  motu_quit();
}
void motu_window_close(int64_t win) { lx_on_ui(lx_job(close_main, 0, win, 0, 0, 0, 0)); }

static void post_main(Job *j) {
  if (valid(j->a)) webkit_web_view_evaluate_javascript(g_win[j->a].view, j->s[0], -1, 0, 0, 0, 0, 0);
}

/* Invalid UTF-8 becomes U+FFFD: text read from any file may reach the page. */
void motu_post(int64_t win, const char *json) {
  char *text = g_utf8_make_valid(json, -1);
  size_t n = strlen(text) + 32;
  char *js = malloc(n);
  snprintf(js, n, "window.__motu_recv(%s)", text);
  g_free(text);
  Job *j = lx_job(post_main, 0, win, 0, 0, 0, 0);
  j->s[0] = js;
  queue_job(j);
}

void *lx_parent_of(int64_t win) { return valid(win) ? g_win[win].window : 0; }
void *lx_view_of(int64_t win) { return valid(win) ? g_win[win].view : 0; }

/* --- application --- */

static void *ui_main(void *arg) {
  g_ui_thread = pthread_self();
  if (!gtk_init_check()) { sync_done(arg, 0); return 0; }
  g_loop = g_main_loop_new(0, 0);
  void *ctx = webkit_web_context_get_default();
  webkit_web_context_register_uri_scheme(ctx, "motu", on_scheme_request, 0, 0);
  void *security = webkit_web_context_get_security_manager(ctx);
  webkit_security_manager_register_uri_scheme_as_secure(security, "motu");
  webkit_security_manager_register_uri_scheme_as_cors_enabled(security, "motu");
  sync_done(arg, 1);
  g_main_loop_run(g_loop);
  lx_deliver(CB_STOP, 0, 0, 0);
  return 0;
}

void motu_init(void *ud, const char *app_name) {
  /* The main fiber stays blocked in motu_run; commands need a second scheduler thread. */
  const char *threads = getenv("KAI_THREADS");
  if (threads && strcmp(threads, "1") == 0) { fprintf(stderr, "motu: KAI_THREADS=1 leaves no thread to run commands; unset it or raise it\n"); exit(2); }
  g_ud = ud;
  load_libraries();
  g_set_application_name(app_name);
  Sync ready = {0, 0};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 8 << 20);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t ui;
  if (pthread_create(&ui, &attr, ui_main, &ready) != 0) { fprintf(stderr, "motu: cannot start the GTK thread\n"); exit(1); }
  pthread_attr_destroy(&attr);
  if (!sync_wait(&ready)) {
    fprintf(stderr, "motu: cannot open a display (is DISPLAY or WAYLAND_DISPLAY set?)\n");
    exit(1);
  }

  /* A default menu bar until the application installs its own. */
  motu_menu_begin("Edit");
  motu_menu_item("undo", "Undo", "z", 0);
  motu_menu_item("redo", "Redo", "Z", 0);
  motu_menu_separator();
  motu_menu_item("cut", "Cut", "x", 0);
  motu_menu_item("copy", "Copy", "c", 0);
  motu_menu_item("paste", "Paste", "v", 0);
  motu_menu_item("select-all", "Select All", "a", 0);
  motu_menu_end();
  motu_menu_install();
}

static void asset_root_job(Job *j) {
  free(g_root);
  g_root = strdup(j->s[0]);
}
void motu_set_asset_root(const char *root) { lx_on_ui(lx_job(asset_root_job, 0, 0, 0, root, 0, 0)); }

static void files_root_job(Job *j) {
  free(g_files_root);
  g_files_root = strdup(j->s[0]);
}
void motu_set_files_root(const char *root) { lx_on_ui(lx_job(files_root_job, 0, 0, 0, root, 0, 0)); }

static void quit_job(Job *j) { (void)j; g_main_loop_quit(g_loop); }

void motu_quit(void) {
  queue_job(lx_job(quit_job, 0, 0, 0, 0, 0, 0));
}

#endif
