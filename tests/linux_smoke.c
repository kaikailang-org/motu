/* Drives the Linux shim without kaikai: the parts of motu.h that need no user
 * input. Build: cc -std=c99 -Ic tests/linux_smoke.c c/motu_linux*.c -lpthread
 * and run under a display. Prints one line per check; exits 1 if any failed. */
#define _GNU_SOURCE

#include "motu.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static char g_dir[256];
static int g_failed;
static int64_t g_win;
static int g_step;

enum { R_CLIP = 1, R_PRINT };
static int g_tags[3];

static void check(const char *what, int ok, const char *detail) {
  printf("%s %s%s%s\n", ok ? "ok  " : "FAIL", what, detail ? ": " : "", detail ? detail : "");
  if (!ok) g_failed = 1;
}

static void write_file(const char *name, const char *text) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", g_dir, name);
  FILE *f = fopen(path, "w");
  fputs(text, f);
  fclose(f);
}

static const char PAGE[] =
  "<!doctype html><meta charset=utf-8><title>smoke</title><p>Smoke page</p><script>\n"
  "const get = async (u) => { try { const r = await fetch(u); return r.status + ' ' + (await r.text()).slice(0, 5); }\n"
  "  catch (e) { return 'error'; } };\n"
  "const img = (u) => new Promise((ok) => { const i = new Image(); i.onload = () => ok('loaded'); i.onerror = () => ok('error'); i.src = u; });\n"
  "motu.on('echo', (d) => motu.invoke('echo', d));\n"
  "(async () => {\n"
  "  const r = [await get('motu://files/a.txt'), await get('motu://files/..%2fa.txt'), await get('motu://files/none.txt'),\n"
  "    await get('motu://app/'), await get('motu://app/missing.js'), await img('motu://files/dot.svg')];\n"
  "  motu.invoke('files', r.join('|'));\n"
  "})();\n"
  "</script>";

static const char SVG[] = "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='4'><rect width='4' height='4'/></svg>";

static const char PRINTED[] =
  "<!doctype html><style>@media print { h1 { break-after: page; } }</style>"
  "<h1>Page one</h1><p>Page two <img src='dot.svg' width=40></p>";

static void *second_window(void *unused) {
  (void)unused;
  int64_t w = motu_window_new("second", 300, 200);
  check("motu_window_new from another thread", w == 2, 0);
  motu_window_close(w);
  return 0;
}

static void next_step(void) {
  char out[512];
  switch (++g_step) {
    case 1:
      motu_post(g_win, "{\"t\":\"event\",\"name\":\"echo\",\"data\":\"a\xff" "b\"}");
      break;
    case 2:
      motu_clipboard_set_text("clip \xe2\x9c\x93");
      motu_clipboard_get_text(&g_tags[R_CLIP]);
      break;
    case 3:
      snprintf(out, sizeof out, "%s/out.pdf", g_dir);
      motu_print_pdf(&g_tags[R_PRINT], PRINTED, "motu://files/", out);
      break;
    case 4: {
      motu_window_set_title(g_win, "renamed");
      motu_window_set_min_size(g_win, 320, 240);
      motu_window_set_fullscreen(g_win, 1);
      motu_window_set_fullscreen(g_win, 0);
      motu_window_focus(g_win);
      motu_menu_set_state("custom", MOTU_MENU_DISABLED);
      motu_menu_set_state("check", MOTU_MENU_CHECKED);
      motu_menu_set_state("check", 0);
      snprintf(out, sizeof out, "%s/a.txt", g_dir);
      check("motu_fsync_path", motu_fsync_path(out) == 0 && motu_fsync_path("/nonexistent/x") == -1, 0);
      check("motu_data_dir", motu_data_dir()[0] == '/', motu_data_dir());
      check("motu_exe_dir", motu_exe_dir()[0] == '/', motu_exe_dir());
      pthread_t t;
      pthread_create(&t, 0, second_window, 0);
      pthread_detach(t);
      break;
    }
  }
}

void motu_on_message(void *ud, int64_t win, const char *json) {
  (void)ud; (void)win;
  if (strstr(json, "\"cmd\":\"files\"")) {
    check("motu:// app and files hosts", strstr(json, "\"args\":\"200 hello|403 |404 |200 <!doc|404 |loaded\"") != 0, json);
    next_step();
  } else if (strstr(json, "\"cmd\":\"echo\"")) {
    check("motu_post repairs invalid UTF-8", strstr(json, "a\xef\xbf\xbd" "b") != 0, json);
    next_step();
  }
}

void motu_on_window_event(void *ud, int64_t win, const char *kind) {
  (void)ud;
  if (strcmp(kind, "closed") == 0) {
    printf("ok   closed %lld\n", (long long)win);
    if (win == 2) motu_window_close(g_win);
  }
}

void motu_on_menu(void *ud, const char *id) { (void)ud; printf("menu %s\n", id); }
void motu_on_drop(void *ud, int64_t win, const char *paths) { (void)ud; (void)win; printf("drop %s\n", paths); }

void motu_on_reply(void *reply, const char *json) {
  if (reply == &g_tags[R_CLIP]) {
    check("clipboard round trip", strcmp(json, "\"clip \xe2\x9c\x93\"") == 0, json);
  } else if (reply == &g_tags[R_PRINT]) {
    char path[512], head[5] = {0};
    snprintf(path, sizeof path, "%s/out.pdf", g_dir);
    FILE *f = fopen(path, "rb");
    struct stat st = {0};
    int pages = 0;
    if (f && stat(path, &st) == 0) {
      char *pdf = calloc(1, (size_t)st.st_size + 1);
      size_t n = fread(pdf, 1, (size_t)st.st_size, f);
      memcpy(head, pdf, n < 4 ? n : 4);
      for (char *p = pdf; (p = memmem(p, n - (size_t)(p - pdf), "/Type /Page", 11)); p += 11)
        if (p[11] != 's') pages++;
      free(pdf);
    }
    if (f) fclose(f);
    char detail[128];
    snprintf(detail, sizeof detail, "reply %s, %lld bytes, %d pages", json, (long long)st.st_size, pages);
    check("motu_print_pdf", strcmp(json, "true") == 0 && memcmp(head, "%PDF", 4) == 0, detail);
  }
  next_step();
}

int main(void) {
  snprintf(g_dir, sizeof g_dir, "/tmp/motu-smoke-XXXXXX");
  if (!mkdtemp(g_dir)) return 1;
  write_file("index.html", PAGE);
  write_file("a.txt", "hello");
  write_file("dot.svg", SVG);
  motu_init(0, "smoke");
  motu_set_asset_root(g_dir);
  motu_set_files_root(g_dir);
  motu_menu_begin("File");
  motu_menu_item("custom", "Custom", "k", 0);
  motu_menu_item("check", "Check", "K", MOTU_MENU_CHECKABLE);
  motu_menu_end();
  motu_menu_install();
  g_win = motu_window_new("smoke", 640, 400);
  check("motu_window_new", g_win == 1, 0);
  motu_window_load(g_win, "motu://app/index.html");
  motu_run();
  check("all steps ran", g_step >= 4, 0);
  return g_failed;
}
