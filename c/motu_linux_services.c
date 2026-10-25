/* Linux: dialogs, the shell, the clipboard, files and printing. */
#ifdef __linux__

#define _GNU_SOURCE

#include "motu.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "motu_linux.h"

/* --- dialogs --- */

enum { PICK_FILE, PICK_FILES, PICK_FOLDER, PICK_FOLDERS, PICK_SAVE };

typedef struct { void *reply; int kind; } Pick;

static void on_file_chosen(void *dialog, void *res, void *data) {
  Pick *p = data;
  GError *err = 0;
  void *file = 0, *files = 0;
  switch (p->kind) {
    case PICK_FILE: file = gtk_file_dialog_open_finish(dialog, res, &err); break;
    case PICK_FILES: files = gtk_file_dialog_open_multiple_finish(dialog, res, &err); break;
    case PICK_FOLDER: file = gtk_file_dialog_select_folder_finish(dialog, res, &err); break;
    case PICK_FOLDERS: files = gtk_file_dialog_select_multiple_folders_finish(dialog, res, &err); break;
    default: file = gtk_file_dialog_save_finish(dialog, res, &err); break;
  }
  lx_clear_error(&err);
  lx_reply_owned(p->reply, lx_paths_json(file, files));
  if (file) g_object_unref(file);
  if (files) g_object_unref(files);
  g_object_unref(dialog);
  free(p);
}

static void set_filters(void *dialog, const char *extensions) {
  if (!extensions || !extensions[0]) return;
  void *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, extensions);
  char **exts = g_strsplit(extensions, ",", -1);
  for (char **e = exts; *e; e++)
    if (**e) gtk_file_filter_add_suffix(filter, *e);
  g_strfreev(exts);
  void *store = g_list_store_new(gtk_file_filter_get_type());
  g_list_store_append(store, filter);
  gtk_file_dialog_set_filters(dialog, store);
  g_object_unref(store);
  g_object_unref(filter);
}

static void open_main(Job *j) {
  void *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, j->s[0]);
  Pick *p = calloc(1, sizeof *p);
  p->reply = j->p;
  int dirs = (j->b & MOTU_OPEN_DIRECTORIES) != 0, many = (j->b & MOTU_OPEN_MULTIPLE) != 0;
  void *parent = lx_parent_of(j->a);
  if (dirs) {
    p->kind = many ? PICK_FOLDERS : PICK_FOLDER;
    (many ? gtk_file_dialog_select_multiple_folders : gtk_file_dialog_select_folder)(dialog, parent, 0, on_file_chosen, p);
    return;
  }
  set_filters(dialog, j->s[1]);
  p->kind = many ? PICK_FILES : PICK_FILE;
  (many ? gtk_file_dialog_open_multiple : gtk_file_dialog_open)(dialog, parent, 0, on_file_chosen, p);
}

void motu_dialog_open(void *reply, int64_t win, const char *title, int64_t flags, const char *extensions) {
  lx_on_ui(lx_job(open_main, reply, win, flags, title, extensions, 0));
}

static void save_main(Job *j) {
  void *dialog = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dialog, j->s[0]);
  if (j->s[1] && j->s[1][0]) gtk_file_dialog_set_initial_name(dialog, j->s[1]);
  Pick *p = calloc(1, sizeof *p);
  p->reply = j->p;
  p->kind = PICK_SAVE;
  gtk_file_dialog_save(dialog, lx_parent_of(j->a), 0, on_file_chosen, p);
}

void motu_dialog_save(void *reply, int64_t win, const char *title, const char *suggested_name) {
  lx_on_ui(lx_job(save_main, reply, win, 0, title, suggested_name, 0));
}

static void on_alert_chosen(void *dialog, void *res, void *reply) {
  GError *err = 0;
  int index = gtk_alert_dialog_choose_finish(dialog, res, &err);
  lx_clear_error(&err);
  char out[32];
  snprintf(out, sizeof out, "%d", index);
  lx_deliver(CB_REPLY, reply, 0, out);
  g_object_unref(dialog);
}

/* A cancelled alert (Escape) answers -1. */
static void message_main(Job *j) {
  void *dialog = gtk_alert_dialog_new("%s", j->s[0]);
  gtk_alert_dialog_set_detail(dialog, j->s[1]);
  char **labels = g_strsplit(j->s[2], "\n", -1);
  static const char *const ok[] = {"OK", 0};
  gtk_alert_dialog_set_buttons(dialog, labels[0] ? (const char *const *)labels : ok);
  g_strfreev(labels);
  gtk_alert_dialog_set_default_button(dialog, 0);
  gtk_alert_dialog_choose(dialog, lx_parent_of(j->a), 0, on_alert_chosen, j->p);
}

void motu_dialog_message(void *reply, int64_t win, const char *title, const char *message, const char *buttons) {
  lx_on_ui(lx_job(message_main, reply, win, 0, title, message, buttons));
}

/* --- shell, clipboard, files --- */

static void launch_path(const char *path) {
  char *abs = g_canonicalize_filename(path, 0);
  char *uri = g_filename_to_uri(abs, 0, 0);
  GError *err = 0;
  if (uri && !g_app_info_launch_default_for_uri(uri, 0, &err))
    fprintf(stderr, "motu: cannot open %s: %s\n", abs, err ? err->message : "unknown error");
  lx_clear_error(&err);
  g_free(uri);
  g_free(abs);
}

static void open_job(Job *j) { launch_path(j->s[0]); }
void motu_shell_open(const char *path) { lx_on_ui(lx_job(open_job, 0, 0, 0, path, 0, 0)); }

static void on_shown(void *bus, void *res, void *data) {
  char *path = data;
  GError *err = 0;
  void *answer = g_dbus_connection_call_finish(bus, res, &err);
  if (answer) g_variant_unref(answer);
  else {
    char *dir = g_path_get_dirname(path);
    launch_path(dir);
    g_free(dir);
  }
  lx_clear_error(&err);
  g_free(path);
  g_object_unref(bus);
}

/* The file manager's ShowItems over D-Bus; else the parent directory. */
static void reveal_job(Job *j) {
  char *abs = g_canonicalize_filename(j->s[0], 0);
  GError *err = 0;
  void *bus = g_bus_get_sync(BUS_SESSION, 0, &err);
  lx_clear_error(&err);
  char *uri = bus ? g_filename_to_uri(abs, 0, 0) : 0;
  if (!uri) {
    char *dir = g_path_get_dirname(abs);
    launch_path(dir);
    g_free(dir);
    g_free(abs);
    if (bus) g_object_unref(bus);
    return;
  }
  char *uris[] = {uri, 0};
  g_dbus_connection_call(bus, "org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
      "org.freedesktop.FileManager1", "ShowItems", g_variant_new("(^ass)", uris, ""),
      0, 0, -1, 0, on_shown, abs);
  g_free(uri);
}
void motu_shell_reveal(const char *path) { lx_on_ui(lx_job(reveal_job, 0, 0, 0, path, 0, 0)); }

static void *clipboard(void) { return gdk_display_get_clipboard(gdk_display_get_default()); }

static void set_text_job(Job *j) { gdk_clipboard_set_text(clipboard(), j->s[0]); }
void motu_clipboard_set_text(const char *text) { lx_on_ui(lx_job(set_text_job, 0, 0, 0, text, 0, 0)); }

static void on_text_read(void *cb, void *res, void *reply) {
  GError *err = 0;
  char *text = gdk_clipboard_read_text_finish(cb, res, &err);
  lx_clear_error(&err);
  Buf b = {0};
  lx_buf_json_string(&b, text ? text : "");
  g_free(text);
  lx_reply_owned(reply, b.p);
}

static void get_text_job(Job *j) { gdk_clipboard_read_text_async(clipboard(), 0, on_text_read, j->p); }
void motu_clipboard_get_text(void *reply) { lx_on_ui(lx_job(get_text_job, reply, 0, 0, 0, 0, 0)); }

int64_t motu_fsync_path(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  int r = fsync(fd);
  close(fd);
  return r == 0 ? 0 : -1;
}

const char *motu_data_dir(void) {
  static char dir[1024];
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  if (xdg && xdg[0] == '/') snprintf(dir, sizeof dir, "%s", xdg);
  else snprintf(dir, sizeof dir, "%s/.local/share", home ? home : "");
  return dir;
}

const char *motu_exe_dir(void) {
  static char dir[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
  if (n <= 0) return ".";
  dir[n] = '\0';
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  return dir;
}

/* --- printing --- */

typedef struct { void *reply; char *out; void *window, *view, *op; int failed, started; } PrintJob;

static int find_file_printer(void *printer, void *data) {
  char **name = data;
  if (gtk_printer_is_virtual(printer) && gtk_printer_accepts_pdf(printer)) {
    *name = strdup(gtk_printer_get_name(printer));
    return 1;
  }
  return 0;
}

static int print_cleanup(void *data) {
  PrintJob *job = data;
  if (job->op) g_object_unref(job->op);
  gtk_window_destroy(job->window);
  free(job->out);
  free(job);
  return 0;
}

static void print_end(PrintJob *job, int ok) {
  lx_deliver(CB_REPLY, job->reply, 0, ok ? "true" : "false");
  g_idle_add_full(PRIORITY_DEFAULT, print_cleanup, job, 0);
}

static void on_print_failed(void *op, GError *err, void *data) {
  (void)op;
  PrintJob *job = data;
  job->failed = 1;
  fprintf(stderr, "motu: printing to %s failed: %s\n", job->out, err ? err->message : "unknown error");
}

static void on_print_finished(void *op, void *data) {
  (void)op;
  PrintJob *job = data;
  print_end(job, !job->failed && access(job->out, F_OK) == 0);
}

static void print_loaded(PrintJob *job) {
  char *printer = 0;
  gtk_enumerate_printers(find_file_printer, &printer, 0, 1);
  if (!printer) {
    fprintf(stderr, "motu: no print-to-file printer: GTK's file print backend is missing\n");
    print_end(job, 0);
    return;
  }
  char *abs = g_canonicalize_filename(job->out, 0);
  char *uri = g_filename_to_uri(abs, 0, 0);
  void *settings = gtk_print_settings_new();
  gtk_print_settings_set_printer(settings, printer);
  gtk_print_settings_set(settings, "output-file-format", "pdf");
  gtk_print_settings_set(settings, "output-uri", uri);
  job->op = webkit_print_operation_new(job->view);
  webkit_print_operation_set_print_settings(job->op, settings);
  CONNECT(job->op, "failed", on_print_failed, job);
  CONNECT(job->op, "finished", on_print_finished, job);
  webkit_print_operation_print(job->op);
  g_object_unref(settings);
  g_free(uri);
  g_free(abs);
  free(printer);
}

static void on_print_load_changed(void *view, int event, void *data) {
  (void)view;
  PrintJob *job = data;
  if (event != LOAD_FINISHED || job->started) return;
  job->started = 1;
  print_loaded(job);
}

static int on_print_load_failed(void *view, int event, const char *uri, GError *err, void *data) {
  (void)view; (void)event;
  PrintJob *job = data;
  fprintf(stderr, "motu: cannot load %s: %s\n", uri, err ? err->message : "unknown error");
  job->started = 1;
  print_end(job, 0);
  return 1;
}

/* The web view sits in a window that is never shown. */
static void print_main(Job *j) {
  PrintJob *job = calloc(1, sizeof *job);
  job->reply = j->p;
  job->out = strdup(j->s[2]);
  job->view = lx_new_web_view(0);
  job->window = gtk_window_new();
  gtk_window_set_default_size(job->window, 595, 842);
  gtk_window_set_child(job->window, job->view);
  CONNECT(job->view, "load-changed", on_print_load_changed, job);
  CONNECT(job->view, "load-failed", on_print_load_failed, job);
  webkit_web_view_load_html(job->view, j->s[0], j->s[1]);
}

void motu_print_pdf(void *reply, const char *html, const char *base_url, const char *out_path) {
  lx_on_ui(lx_job(print_main, reply, 0, 0, html, base_url, out_path));
}

#endif
