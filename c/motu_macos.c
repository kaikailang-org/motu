/* macOS: AppKit and WKWebView through the Objective-C runtime, so the shim is
 * plain C and the frameworks load with dlopen instead of link flags. */
#ifdef __APPLE__

#include "motu.h"

#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The Objective-C runtime is loaded with dlopen like the frameworks, so the
 * package links nothing on any platform. Every runtime call below goes through
 * these pointers, set in motu_init. */
#define OBJC_FNS(X) \
  X(Class, objc_getClass, (const char *)) \
  X(SEL, sel_registerName, (const char *)) \
  X(void, objc_msgSend, (void)) \
  X(void, objc_msgSendSuper, (void)) \
  X(Class, objc_allocateClassPair, (Class, const char *, size_t)) \
  X(void, objc_registerClassPair, (Class)) \
  X(BOOL, class_addMethod, (Class, SEL, IMP, const char *)) \
  X(BOOL, class_addProtocol, (Class, Protocol *)) \
  X(BOOL, class_addIvar, (Class, const char *, size_t, uint8_t, const char *)) \
  X(Class, class_getSuperclass, (Class)) \
  X(BOOL, class_respondsToSelector, (Class, SEL)) \
  X(Protocol *, objc_getProtocol, (const char *)) \
  X(Class, object_getClass, (id)) \
  X(Ivar, object_getInstanceVariable, (id, const char *, void **)) \
  X(Ivar, object_setInstanceVariable, (id, const char *, void *))
#define OBJC_PTR(ret, name, args) static ret (*p_##name) args;
OBJC_FNS(OBJC_PTR)
#define objc_getClass p_objc_getClass
#define sel_registerName p_sel_registerName
#define objc_msgSend p_objc_msgSend
#define objc_msgSendSuper p_objc_msgSendSuper
#define objc_allocateClassPair p_objc_allocateClassPair
#define objc_registerClassPair p_objc_registerClassPair
#define class_addMethod p_class_addMethod
#define class_addProtocol p_class_addProtocol
#define class_addIvar p_class_addIvar
#define class_getSuperclass p_class_getSuperclass
#define class_respondsToSelector p_class_respondsToSelector
#define objc_getProtocol p_objc_getProtocol
#define object_getClass p_object_getClass
#define object_getInstanceVariable p_object_getInstanceVariable
#define object_setInstanceVariable p_object_setInstanceVariable

static int load_objc(void) {
  void *rt = dlopen("/usr/lib/libobjc.A.dylib", RTLD_LAZY | RTLD_GLOBAL);
  if (!rt) return 0;
#define OBJC_LOAD(ret, name, args) *(void **)&p_##name = dlsym(rt, #name); if (!p_##name) return 0;
  OBJC_FNS(OBJC_LOAD)
  return 1;
}

typedef struct { double x, y, w, h; } MotuRect;
typedef struct { double w, h; } MotuSize;

#define CLS(n) ((id)objc_getClass(n))
#define SEL_(n) sel_registerName(n)
#define MSG(ret, ...) ((ret (*)(id, SEL, ##__VA_ARGS__))objc_msgSend)
#define NEW(cls) MSG(id)(MSG(id)(CLS(cls), SEL_("alloc")), SEL_("init"))

static void *g_ud;
static char *g_root;
static char *g_files_root;
static pthread_mutex_t g_files_lock = PTHREAD_MUTEX_INITIALIZER;
static id g_app;

#define MOTU_MAX_WINDOWS 64
static id g_windows[MOTU_MAX_WINDOWS + 1];
static id g_views[MOTU_MAX_WINDOWS + 1];

static id nsstr(const char *s) {
  return MSG(id, const char *)(CLS("NSString"), SEL_("stringWithUTF8String:"), s ? s : "");
}

static const char *cstr(id s) {
  return s ? MSG(const char *)(s, SEL_("UTF8String")) : "";
}

static int valid(int64_t win) {
  return win >= 1 && win <= MOTU_MAX_WINDOWS && g_windows[win];
}

static int64_t id_of(id obj, id *table) {
  for (int64_t i = 1; i <= MOTU_MAX_WINDOWS; i++)
    if (table[i] == obj) return i;
  return 0;
}

/* Runs `b` on the main thread: now if already there, else queued. */
static void on_main(void (^b)(void)) {
  if (pthread_main_np()) b();
  else dispatch_async(dispatch_get_main_queue(), b);
}

/* --- JSON text for callbacks --- */

typedef struct { char *p; size_t n, cap; } Buf;

static void buf_put(Buf *b, const char *s, size_t n) {
  if (b->n + n + 1 > b->cap) {
    b->cap = (b->n + n + 1) * 2;
    b->p = realloc(b->p, b->cap);
  }
  memcpy(b->p + b->n, s, n);
  b->n += n;
  b->p[b->n] = '\0';
}

static void buf_json_string(Buf *b, const char *s) {
  buf_put(b, "\"", 1);
  for (; *s; s++) {
    unsigned char c = (unsigned char)*s;
    char esc[8];
    if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = (char)c; buf_put(b, esc, 2); }
    else if (c < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", c); buf_put(b, esc, 6); }
    else buf_put(b, (const char *)&c, 1);
  }
  buf_put(b, "\"", 1);
}

/* A JSON array of the paths of an NSArray of file NSURLs. */
static char *paths_json(id urls) {
  Buf b = {0};
  buf_put(&b, "[", 1);
  unsigned long n = urls ? MSG(unsigned long)(urls, SEL_("count")) : 0;
  for (unsigned long i = 0; i < n; i++) {
    if (i) buf_put(&b, ",", 1);
    id url = MSG(id, unsigned long)(urls, SEL_("objectAtIndex:"), i);
    buf_json_string(&b, cstr(MSG(id)(url, SEL_("path"))));
  }
  buf_put(&b, "]", 1);
  return b.p;
}

static void reply_owned(void *reply, char *json) {
  motu_on_reply(reply, json);
  free(json);
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

/* --- page -> kaikai --- */

static void on_script_message(id self, SEL cmd, id controller, id message) {
  (void)self; (void)cmd; (void)controller;
  id view = MSG(id)(message, SEL_("webView"));
  id body = MSG(id)(message, SEL_("body"));
  motu_on_message(g_ud, id_of(view, g_views), cstr(body));
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

static void scheme_respond(id task, id url, long code, const char *mime, id data) {
  /* The page (motu://app) may fetch from motu://files, another origin. */
  id values[2] = {nsstr(mime), nsstr("motu://app")};
  id keys[2] = {nsstr("Content-Type"), nsstr("Access-Control-Allow-Origin")};
  id headers = MSG(id, id *, id *, unsigned long)(CLS("NSDictionary"), SEL_("dictionaryWithObjects:forKeys:count:"),
      values, keys, 2UL);
  id resp = MSG(id, id, long, id, id)(MSG(id)(CLS("NSHTTPURLResponse"), SEL_("alloc")),
      SEL_("initWithURL:statusCode:HTTPVersion:headerFields:"), url, code, nsstr("HTTP/1.1"), headers);
  MSG(void, id)(task, SEL_("didReceiveResponse:"), resp);
  if (data) MSG(void, id)(task, SEL_("didReceiveData:"), data);
  MSG(void)(task, SEL_("didFinish"));
}

static void on_scheme_start(id self, SEL cmd, id view, id task) {
  (void)self; (void)cmd; (void)view;
  id url = MSG(id)(MSG(id)(task, SEL_("request")), SEL_("URL"));
  const char *path = cstr(MSG(id)(url, SEL_("path")));
  int files = strcmp(cstr(MSG(id)(url, SEL_("host"))), "files") == 0;
  pthread_mutex_lock(&g_files_lock);
  char *base = files ? (g_files_root ? strdup(g_files_root) : 0) : (g_root ? strdup(g_root) : 0);
  pthread_mutex_unlock(&g_files_lock);
  if (!base || strstr(path, "..")) { free(base); scheme_respond(task, url, 403, "text/plain", 0); return; }
  if (!files && (path[0] == '\0' || strcmp(path, "/") == 0)) path = "/index.html";
  size_t n = strlen(base) + strlen(path) + 1;
  char *full = malloc(n);
  snprintf(full, n, "%s%s", base, path);
  free(base);
  id data = MSG(id, id)(CLS("NSData"), SEL_("dataWithContentsOfFile:"), nsstr(full));
  if (data) scheme_respond(task, url, 200, mime_of(full), data);
  else scheme_respond(task, url, 404, "text/plain", 0);
  free(full);
}

static void on_scheme_stop(id self, SEL cmd, id view, id task) {
  (void)self; (void)cmd; (void)view; (void)task;
}

/* --- the web view: file drops become paths, other drags reach the page --- */

static id file_urls_of(id info) {
  id pb = MSG(id)(info, SEL_("draggingPasteboard"));
  id classes = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), CLS("NSURL"));
  id opts = MSG(id, id, id)(CLS("NSDictionary"), SEL_("dictionaryWithObject:forKey:"),
      MSG(id, BOOL)(CLS("NSNumber"), SEL_("numberWithBool:"), YES),
      nsstr("NSPasteboardURLReadingFileURLsOnlyKey"));
  id urls = MSG(id, id, id)(pb, SEL_("readObjectsForClasses:options:"), classes, opts);
  return urls && MSG(unsigned long)(urls, SEL_("count")) > 0 ? urls : 0;
}

static unsigned long on_drag_entered(id self, SEL cmd, id info) {
  if (file_urls_of(info)) return 1; /* NSDragOperationCopy */
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  return ((unsigned long (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&sup, cmd, info);
}

static BOOL on_perform_drag(id self, SEL cmd, id info) {
  id urls = file_urls_of(info);
  if (urls) {
    char *json = paths_json(urls);
    motu_on_drop(g_ud, id_of(self, g_views), json);
    free(json);
    return YES;
  }
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  return ((BOOL (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&sup, cmd, info);
}

/* --- windows --- */

static BOOL on_window_should_close(id self, SEL cmd, id window) {
  (void)self; (void)cmd;
  motu_on_window_event(g_ud, id_of(window, g_windows), "close-requested");
  return NO;
}

static void window_note(id note, const char *kind) {
  motu_on_window_event(g_ud, id_of(MSG(id)(note, SEL_("object")), g_windows), kind);
}
static void on_did_become_key(id self, SEL cmd, id note) { (void)self; (void)cmd; window_note(note, "focus"); }
static void on_did_resign_key(id self, SEL cmd, id note) { (void)self; (void)cmd; window_note(note, "blur"); }
static void on_did_resize(id self, SEL cmd, id note) { (void)self; (void)cmd; window_note(note, "resize"); }

static long on_should_terminate(id self, SEL cmd, id app) {
  (void)self; (void)cmd; (void)app;
  motu_on_window_event(g_ud, 0, "quit-requested");
  return 0; /* NSTerminateCancel: the backend decides and calls motu_quit */
}

/* --- menus --- */

static id g_menu_target;
static id g_menu_items;      /* NSMutableDictionary: id -> NSMenuItem */
static id g_menu_stack[16];
static int g_menu_depth;

static void on_menu_chosen(id self, SEL cmd, id item) {
  (void)self; (void)cmd;
  motu_on_menu(g_ud, cstr(MSG(id)(item, SEL_("representedObject"))));
}

static const char *standard_selector(const char *id_) {
  static const char *table[][2] = {
    {"undo", "undo:"}, {"redo", "redo:"}, {"cut", "cut:"}, {"copy", "copy:"},
    {"paste", "paste:"}, {"select-all", "selectAll:"}, {"quit", "terminate:"},
    {"close-window", "performClose:"}, {"fullscreen", "toggleFullScreen:"},
    {"minimize", "performMiniaturize:"},
  };
  for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
    if (strcmp(id_, table[i][0]) == 0) return table[i][1];
  return 0;
}

static id new_menu(const char *title) {
  id m = MSG(id, id)(MSG(id)(CLS("NSMenu"), SEL_("alloc")), SEL_("initWithTitle:"), nsstr(title));
  MSG(void, BOOL)(m, SEL_("setAutoenablesItems:"), NO);
  return m;
}

static void apply_flags(id item, int64_t flags) {
  MSG(void, BOOL)(item, SEL_("setEnabled:"), (flags & MOTU_MENU_DISABLED) ? NO : YES);
  MSG(void, long)(item, SEL_("setState:"), (flags & MOTU_MENU_CHECKED) ? 1L : 0L);
}

static void add_item(id menu, const char *id_, const char *title, const char *key, int64_t flags) {
  const char *std = standard_selector(id_);
  id item = MSG(id, id, SEL, id)(MSG(id)(CLS("NSMenuItem"), SEL_("alloc")),
      SEL_("initWithTitle:action:keyEquivalent:"), nsstr(title),
      std ? SEL_(std) : SEL_("motuMenu:"), nsstr(key));
  if (!std) MSG(void, id)(item, SEL_("setTarget:"), g_menu_target);
  MSG(void, id)(item, SEL_("setRepresentedObject:"), nsstr(id_));
  apply_flags(item, flags);
  MSG(void, id)(menu, SEL_("addItem:"), item);
  MSG(void, id, id)(g_menu_items, SEL_("setObject:forKey:"), item, nsstr(id_));
}

void motu_menu_begin(const char *title) {
  if (g_menu_depth == 0) {
    g_menu_items = NEW("NSMutableDictionary");
    id bar = new_menu("");
    /* The application menu comes first on macOS. */
    id app_item = NEW("NSMenuItem");
    MSG(void, id)(bar, SEL_("addItem:"), app_item);
    id app_menu = new_menu("");
    add_item(app_menu, "hide", "Hide", "h", 0);
    MSG(void, SEL)(MSG(id, id)(g_menu_items, SEL_("objectForKey:"), nsstr("hide")), SEL_("setAction:"), SEL_("hide:"));
    MSG(void, id)(MSG(id, id)(g_menu_items, SEL_("objectForKey:"), nsstr("hide")), SEL_("setTarget:"), (id)0);
    MSG(void, id)(app_menu, SEL_("addItem:"), MSG(id)(CLS("NSMenuItem"), SEL_("separatorItem")));
    add_item(app_menu, "quit", "Quit", "q", 0);
    MSG(void, id)(app_item, SEL_("setSubmenu:"), app_menu);
    g_menu_stack[g_menu_depth++] = bar;
  }
  id parent = g_menu_stack[g_menu_depth - 1];
  id item = MSG(id, id, SEL, id)(MSG(id)(CLS("NSMenuItem"), SEL_("alloc")),
      SEL_("initWithTitle:action:keyEquivalent:"), nsstr(title), (SEL)0, nsstr(""));
  id sub = new_menu(title);
  MSG(void, id)(item, SEL_("setSubmenu:"), sub);
  MSG(void, id)(parent, SEL_("addItem:"), item);
  if (g_menu_depth < 16) g_menu_stack[g_menu_depth++] = sub;
}

void motu_menu_item(const char *id_, const char *title, const char *key, int64_t flags) {
  if (g_menu_depth > 1) add_item(g_menu_stack[g_menu_depth - 1], id_, title, key, flags);
}

void motu_menu_separator(void) {
  if (g_menu_depth > 1)
    MSG(void, id)(g_menu_stack[g_menu_depth - 1], SEL_("addItem:"), MSG(id)(CLS("NSMenuItem"), SEL_("separatorItem")));
}

void motu_menu_end(void) {
  if (g_menu_depth > 1) g_menu_depth--;
}

void motu_menu_install(void) {
  if (g_menu_depth >= 1) MSG(void, id)(g_app, SEL_("setMainMenu:"), g_menu_stack[0]);
  g_menu_depth = 0;
}

void motu_menu_set_state(const char *id_, int64_t flags) {
  id key = MSG(id)(nsstr(id_), SEL_("copy"));
  on_main(^{
    id item = g_menu_items ? MSG(id, id)(g_menu_items, SEL_("objectForKey:"), key) : 0;
    if (item) apply_flags(item, flags);
  });
}

/* --- dialogs --- */

void motu_dialog_open(void *reply, int64_t win, const char *title, int64_t flags, const char *extensions) {
  (void)win;
  id t = MSG(id)(nsstr(title), SEL_("copy"));
  id exts = MSG(id, id)(nsstr(extensions), SEL_("componentsSeparatedByString:"), nsstr(","));
  int any = extensions[0] == '\0';
  on_main(^{
    id panel = MSG(id)(CLS("NSOpenPanel"), SEL_("openPanel"));
    MSG(void, id)(panel, SEL_("setMessage:"), t);
    int dirs = (flags & MOTU_OPEN_DIRECTORIES) != 0;
    MSG(void, BOOL)(panel, SEL_("setCanChooseDirectories:"), dirs ? YES : NO);
    MSG(void, BOOL)(panel, SEL_("setCanChooseFiles:"), dirs ? NO : YES);
    MSG(void, BOOL)(panel, SEL_("setAllowsMultipleSelection:"), (flags & MOTU_OPEN_MULTIPLE) ? YES : NO);
    if (!any) MSG(void, id)(panel, SEL_("setAllowedFileTypes:"), exts);
    long r = MSG(long)(panel, SEL_("runModal"));
    reply_owned(reply, paths_json(r == 1 ? MSG(id)(panel, SEL_("URLs")) : 0));
  });
}

void motu_dialog_save(void *reply, int64_t win, const char *title, const char *suggested_name) {
  (void)win;
  id t = MSG(id)(nsstr(title), SEL_("copy"));
  id name = MSG(id)(nsstr(suggested_name), SEL_("copy"));
  on_main(^{
    id panel = MSG(id)(CLS("NSSavePanel"), SEL_("savePanel"));
    MSG(void, id)(panel, SEL_("setMessage:"), t);
    MSG(void, id)(panel, SEL_("setNameFieldStringValue:"), name);
    long r = MSG(long)(panel, SEL_("runModal"));
    id urls = r == 1 ? MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), MSG(id)(panel, SEL_("URL"))) : 0;
    reply_owned(reply, paths_json(urls));
  });
}

void motu_dialog_message(void *reply, int64_t win, const char *title, const char *message, const char *buttons) {
  (void)win;
  id t = MSG(id)(nsstr(title), SEL_("copy"));
  id m = MSG(id)(nsstr(message), SEL_("copy"));
  id labels = MSG(id, id)(nsstr(buttons), SEL_("componentsSeparatedByString:"), nsstr("\n"));
  on_main(^{
    id alert = NEW("NSAlert");
    MSG(void, id)(alert, SEL_("setMessageText:"), t);
    MSG(void, id)(alert, SEL_("setInformativeText:"), m);
    unsigned long n = MSG(unsigned long)(labels, SEL_("count"));
    for (unsigned long i = 0; i < n; i++)
      MSG(id, id)(alert, SEL_("addButtonWithTitle:"), MSG(id, unsigned long)(labels, SEL_("objectAtIndex:"), i));
    long r = MSG(long)(alert, SEL_("runModal"));
    char out[32];
    snprintf(out, sizeof out, "%ld", r - 1000); /* NSAlertFirstButtonReturn */
    motu_on_reply(reply, out);
  });
}

/* --- shell, clipboard, files --- */

static id file_url(const char *path) {
  return MSG(id, id)(CLS("NSURL"), SEL_("fileURLWithPath:"), nsstr(path));
}

void motu_shell_open(const char *path) {
  id url = file_url(path);
  on_main(^{ MSG(BOOL, id)(MSG(id)(CLS("NSWorkspace"), SEL_("sharedWorkspace")), SEL_("openURL:"), url); });
}

void motu_shell_reveal(const char *path) {
  id urls = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), file_url(path));
  on_main(^{
    MSG(void, id)(MSG(id)(CLS("NSWorkspace"), SEL_("sharedWorkspace")), SEL_("activateFileViewerSelectingURLs:"), urls);
  });
}

void motu_clipboard_set_text(const char *text) {
  id s = MSG(id)(nsstr(text), SEL_("copy"));
  on_main(^{
    id pb = MSG(id)(CLS("NSPasteboard"), SEL_("generalPasteboard"));
    MSG(long)(pb, SEL_("clearContents"));
    MSG(BOOL, id, id)(pb, SEL_("setString:forType:"), s, nsstr("public.utf8-plain-text"));
  });
}

void motu_clipboard_get_text(void *reply) {
  on_main(^{
    id pb = MSG(id)(CLS("NSPasteboard"), SEL_("generalPasteboard"));
    id s = MSG(id, id)(pb, SEL_("stringForType:"), nsstr("public.utf8-plain-text"));
    Buf b = {0};
    buf_json_string(&b, cstr(s));
    reply_owned(reply, b.p);
  });
}

int64_t motu_fsync_path(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) return -1;
  int r = fcntl(fd, F_FULLFSYNC);
  if (r != 0) r = fsync(fd);
  close(fd);
  return r == 0 ? 0 : -1;
}

const char *motu_data_dir(void) {
  static char dir[1024];
  const char *home = getenv("HOME");
  snprintf(dir, sizeof dir, "%s/Library/Application Support", home ? home : "");
  return dir;
}

const char *motu_exe_dir(void) {
  static char dir[PATH_MAX];
  char raw[PATH_MAX];
  uint32_t n = sizeof raw;
  if (_NSGetExecutablePath(raw, &n) != 0 || !realpath(raw, dir)) return ".";
  char *slash = strrchr(dir, '/');
  if (slash) *slash = '\0';
  return dir;
}

/* --- printing --- */

typedef struct { void *reply; char *out; id view; } PrintJob;

static void on_print_loaded(id self, SEL cmd, id view, id nav) {
  (void)cmd; (void)nav;
  void *ptr = 0;
  object_getInstanceVariable(self, "job", &ptr);
  PrintJob *job = ptr;
  id info = MSG(id)(CLS("NSPrintInfo"), SEL_("sharedPrintInfo"));
  info = MSG(id)(info, SEL_("copy"));
  id dict = MSG(id)(info, SEL_("dictionary"));
  MSG(void, id, id)(dict, SEL_("setObject:forKey:"), nsstr("NSPrintSaveJob"), nsstr("NSJobDisposition"));
  MSG(void, id, id)(dict, SEL_("setObject:forKey:"), file_url(job->out), nsstr("NSJobSavingURL"));
  MSG(void, long)(info, SEL_("setHorizontalPagination:"), 0L);
  MSG(void, long)(info, SEL_("setVerticalPagination:"), 0L);
  id op = MSG(id, id)(view, SEL_("printOperationWithPrintInfo:"), info);
  MSG(void, BOOL)(op, SEL_("setShowsPrintPanel:"), NO);
  MSG(void, BOOL)(op, SEL_("setShowsProgressPanel:"), NO);
  id opview = MSG(id)(op, SEL_("view"));
  MotuRect frame = {0, 0, 595, 842};
  MSG(void, MotuRect)(opview, SEL_("setFrame:"), frame);
  id host = MSG(id)(view, SEL_("window"));
  MSG(void, id, id, SEL, void *)(op, SEL_("runOperationModalForWindow:delegate:didRunSelector:contextInfo:"),
      host, self, SEL_("printDone:success:context:"), (void *)job);
}

static void on_print_done(id self, SEL cmd, id op, BOOL ok, void *ctx) {
  (void)self; (void)cmd; (void)op;
  PrintJob *job = ctx;
  motu_on_reply(job->reply, ok ? "true" : "false");
  id win = MSG(id)(job->view, SEL_("window"));
  free(job->out);
  free(job);
  /* Closing the host window inside its own print callback traps in AppKit. */
  dispatch_async(dispatch_get_main_queue(), ^{ MSG(void)(win, SEL_("close")); });
}

static Class g_print_delegate_class;

void motu_print_pdf(void *reply, const char *html, const char *base_url, const char *out_path) {
  PrintJob *job = calloc(1, sizeof *job);
  job->reply = reply;
  job->out = strdup(out_path);
  id h = MSG(id)(nsstr(html), SEL_("copy"));
  id u = MSG(id)(nsstr(base_url), SEL_("copy"));
  on_main(^{
    id config = NEW("WKWebViewConfiguration");
    MSG(void, id, id)(config, SEL_("setURLSchemeHandler:forURLScheme:"),
        MSG(id)((id)objc_getClass("MotuSchemeHandler"), SEL_("new")), nsstr("motu"));
    MotuRect frame = {0, 0, 595, 842};
    id view = MSG(id, MotuRect, id)(MSG(id)(CLS("WKWebView"), SEL_("alloc")),
        SEL_("initWithFrame:configuration:"), frame, config);
    /* An offscreen window: the print operation needs one to run against. */
    MotuRect far = {-10000, -10000, 595, 842};
    id win = MSG(id, MotuRect, unsigned long, unsigned long, BOOL)(MSG(id)(CLS("NSWindow"), SEL_("alloc")),
        SEL_("initWithContentRect:styleMask:backing:defer:"), far, 0UL, 2UL, NO);
    MSG(void, BOOL)(win, SEL_("setReleasedWhenClosed:"), NO);
    MSG(void, id)(win, SEL_("setContentView:"), view);
    MSG(void, id)(win, SEL_("orderBack:"), (id)0);
    job->view = view;
    id delegate = MSG(id)((id)g_print_delegate_class, SEL_("new"));
    object_setInstanceVariable(delegate, "job", (void *)job);
    MSG(void, id)(view, SEL_("setNavigationDelegate:"), delegate);
    MSG(id, id, id)(view, SEL_("loadHTMLString:baseURL:"), h, MSG(id, id)(CLS("NSURL"), SEL_("URLWithString:"), u));
  });
}

void motu_snapshot(void *reply, int64_t win, const char *out_path) {
  char *out = strdup(out_path);
  on_main(^{
    if (!valid(win)) { motu_on_reply(reply, "false"); free(out); return; }
    MSG(void, id, id)(g_views[win], SEL_("takeSnapshotWithConfiguration:completionHandler:"), (id)0, (id)^(id image, id error) {
      (void)error;
      int ok = 0;
      if (image) {
        id tiff = MSG(id)(image, SEL_("TIFFRepresentation"));
        id rep = MSG(id, id)(CLS("NSBitmapImageRep"), SEL_("imageRepWithData:"), tiff);
        id png = MSG(id, unsigned long, id)(rep, SEL_("representationUsingType:properties:"), 4UL, MSG(id)(CLS("NSDictionary"), SEL_("dictionary")));
        ok = png && MSG(BOOL, id, BOOL)(png, SEL_("writeToFile:atomically:"), nsstr(out), YES);
      }
      motu_on_reply(reply, ok ? "true" : "false");
      free(out);
    });
  });
}

/* --- setup --- */

static id g_msg_handler, g_scheme_handler, g_win_delegate, g_app_delegate;
static Class g_webview_class;

static Class make_class(const char *name, const char *super, const char *protocol) {
  Class c = objc_allocateClassPair((Class)objc_getClass(super), name, 0);
  if (protocol) class_addProtocol(c, objc_getProtocol(protocol));
  return c;
}

static id instance_of(Class c) {
  objc_registerClassPair(c);
  return MSG(id)((id)c, SEL_("new"));
}

void motu_init(void *ud, const char *app_name) {
  (void)app_name;
  g_ud = ud;
  if (!load_objc()) { fprintf(stderr, "motu: cannot load the Objective-C runtime\n"); exit(1); }
  dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_LAZY | RTLD_GLOBAL);
  dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_LAZY | RTLD_GLOBAL);
  g_app = MSG(id)(CLS("NSApplication"), SEL_("sharedApplication"));
  MSG(BOOL, long)(g_app, SEL_("setActivationPolicy:"), 0L); /* Regular */

  Class c = make_class("MotuMessageHandler", "NSObject", "WKScriptMessageHandler");
  class_addMethod(c, SEL_("userContentController:didReceiveScriptMessage:"), (IMP)on_script_message, "v@:@@");
  g_msg_handler = instance_of(c);

  c = make_class("MotuSchemeHandler", "NSObject", "WKURLSchemeHandler");
  class_addMethod(c, SEL_("webView:startURLSchemeTask:"), (IMP)on_scheme_start, "v@:@@");
  class_addMethod(c, SEL_("webView:stopURLSchemeTask:"), (IMP)on_scheme_stop, "v@:@@");
  g_scheme_handler = instance_of(c);

  c = make_class("MotuWindowDelegate", "NSObject", "NSWindowDelegate");
  class_addMethod(c, SEL_("windowShouldClose:"), (IMP)on_window_should_close, "c@:@");
  class_addMethod(c, SEL_("windowDidBecomeKey:"), (IMP)on_did_become_key, "v@:@");
  class_addMethod(c, SEL_("windowDidResignKey:"), (IMP)on_did_resign_key, "v@:@");
  class_addMethod(c, SEL_("windowDidResize:"), (IMP)on_did_resize, "v@:@");
  g_win_delegate = instance_of(c);

  c = make_class("MotuAppDelegate", "NSObject", "NSApplicationDelegate");
  class_addMethod(c, SEL_("applicationShouldTerminate:"), (IMP)on_should_terminate, "Q@:@");
  g_app_delegate = instance_of(c);
  MSG(void, id)(g_app, SEL_("setDelegate:"), g_app_delegate);

  c = make_class("MotuMenuTarget", "NSObject", 0);
  class_addMethod(c, SEL_("motuMenu:"), (IMP)on_menu_chosen, "v@:@");
  g_menu_target = instance_of(c);

  g_webview_class = make_class("MotuWebView", "WKWebView", 0);
  class_addMethod(g_webview_class, SEL_("draggingEntered:"), (IMP)on_drag_entered, "Q@:@");
  class_addMethod(g_webview_class, SEL_("performDragOperation:"), (IMP)on_perform_drag, "c@:@");
  objc_registerClassPair(g_webview_class);

  g_print_delegate_class = make_class("MotuPrintDelegate", "NSObject", "WKNavigationDelegate");
  class_addIvar(g_print_delegate_class, "job", sizeof(void *), 3, "^v");
  class_addMethod(g_print_delegate_class, SEL_("webView:didFinishNavigation:"), (IMP)on_print_loaded, "v@:@@");
  class_addMethod(g_print_delegate_class, SEL_("printDone:success:context:"), (IMP)on_print_done, "v@:@c^v");
  objc_registerClassPair(g_print_delegate_class);

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

void motu_set_files_root(const char *root) {
  pthread_mutex_lock(&g_files_lock);
  free(g_files_root);
  g_files_root = root[0] ? strdup(root) : 0;
  pthread_mutex_unlock(&g_files_lock);
}

void motu_set_asset_root(const char *root) {
  free(g_root);
  g_root = strdup(root);
}

static int64_t window_new_main(const char *title, int64_t width, int64_t height) {
  int64_t slot = 0;
  for (int64_t i = 1; i <= MOTU_MAX_WINDOWS; i++)
    if (!g_windows[i]) { slot = i; break; }
  if (!slot) return 0;

  id config = NEW("WKWebViewConfiguration");
  MSG(void, id, id)(config, SEL_("setURLSchemeHandler:forURLScheme:"), g_scheme_handler, nsstr("motu"));
  id ucc = MSG(id)(config, SEL_("userContentController"));
  MSG(void, id, id)(ucc, SEL_("addScriptMessageHandler:name:"), g_msg_handler, nsstr("motu"));
  id script = MSG(id, id, long, BOOL)(MSG(id)(CLS("WKUserScript"), SEL_("alloc")),
      SEL_("initWithSource:injectionTime:forMainFrameOnly:"), nsstr(BRIDGE_JS), 0L, YES);
  MSG(void, id)(ucc, SEL_("addUserScript:"), script);

  MotuRect frame = {0, 0, (double)width, (double)height};
  unsigned long style = 1 | 2 | 4 | 8; /* titled, closable, miniaturizable, resizable */
  id win = MSG(id, MotuRect, unsigned long, unsigned long, BOOL)(MSG(id)(CLS("NSWindow"), SEL_("alloc")),
      SEL_("initWithContentRect:styleMask:backing:defer:"), frame, style, 2UL, NO);
  MSG(void, BOOL)(win, SEL_("setReleasedWhenClosed:"), NO);
  MSG(void, id)(win, SEL_("setTitle:"), nsstr(title));
  MSG(void, id)(win, SEL_("setDelegate:"), g_win_delegate);
  MSG(void, unsigned long)(win, SEL_("setCollectionBehavior:"), 1UL << 7); /* full screen primary */
  id view = MSG(id, MotuRect, id)(MSG(id)((id)g_webview_class, SEL_("alloc")),
      SEL_("initWithFrame:configuration:"), frame, config);
  if (class_respondsToSelector(object_getClass(view), SEL_("setInspectable:")))
    MSG(void, BOOL)(view, SEL_("setInspectable:"), YES);
  id types = MSG(id, id)(CLS("NSArray"), SEL_("arrayWithObject:"), nsstr("public.file-url"));
  MSG(void, id)(view, SEL_("registerForDraggedTypes:"), types);
  MSG(void, id)(win, SEL_("setContentView:"), view);
  MSG(void)(win, SEL_("center"));
  MSG(void, id)(win, SEL_("makeKeyAndOrderFront:"), (id)0);
  g_windows[slot] = win;
  g_views[slot] = view;
  return slot;
}

int64_t motu_window_new(const char *title, int64_t width, int64_t height) {
  if (pthread_main_np()) return window_new_main(title, width, height);
  __block int64_t win = 0;
  id t = MSG(id)(nsstr(title), SEL_("copy"));
  dispatch_sync(dispatch_get_main_queue(), ^{ win = window_new_main(cstr(t), width, height); });
  return win;
}

void motu_window_load(int64_t win, const char *url) {
  id u = MSG(id)(nsstr(url), SEL_("copy"));
  on_main(^{
    if (!valid(win)) return;
    id req = MSG(id, id)(CLS("NSURLRequest"), SEL_("requestWithURL:"), MSG(id, id)(CLS("NSURL"), SEL_("URLWithString:"), u));
    MSG(id, id)(g_views[win], SEL_("loadRequest:"), req);
  });
}

void motu_window_set_title(int64_t win, const char *title) {
  id t = MSG(id)(nsstr(title), SEL_("copy"));
  on_main(^{ if (valid(win)) MSG(void, id)(g_windows[win], SEL_("setTitle:"), t); });
}

void motu_window_set_min_size(int64_t win, int64_t width, int64_t height) {
  on_main(^{
    if (!valid(win)) return;
    MotuSize size = {(double)width, (double)height};
    MSG(void, MotuSize)(g_windows[win], SEL_("setContentMinSize:"), size);
  });
}

void motu_window_set_fullscreen(int64_t win, int64_t on) {
  on_main(^{
    if (!valid(win)) return;
    unsigned long mask = MSG(unsigned long)(g_windows[win], SEL_("styleMask"));
    int is_full = (mask & (1UL << 14)) != 0;
    if ((on != 0) != is_full) MSG(void, id)(g_windows[win], SEL_("toggleFullScreen:"), (id)0);
  });
}

void motu_window_focus(int64_t win) {
  on_main(^{ if (valid(win)) MSG(void, id)(g_windows[win], SEL_("makeKeyAndOrderFront:"), (id)0); });
}

void motu_window_close(int64_t win) {
  on_main(^{
    if (!valid(win)) return;
    id w = g_windows[win];
    g_windows[win] = 0;
    g_views[win] = 0;
    MSG(void)(w, SEL_("close"));
    motu_on_window_event(g_ud, win, "closed");
    for (int64_t i = 1; i <= MOTU_MAX_WINDOWS; i++)
      if (g_windows[i]) return;
    motu_quit();
  });
}

void motu_run(void) {
  MSG(void, BOOL)(g_app, SEL_("activateIgnoringOtherApps:"), YES);
  MSG(void)(g_app, SEL_("run"));
}

void motu_quit(void) {
  dispatch_async(dispatch_get_main_queue(), ^{
    MSG(void, id)(g_app, SEL_("stop:"), (id)0);
    /* stop: takes effect after the next event; post one so run returns now. */
    MotuRect zero = {0, 0, 0, 0};
    id ev = MSG(id, unsigned long, MotuRect, unsigned long, double, long, id, short, long, long)(
        CLS("NSEvent"), SEL_("otherEventWithType:location:modifierFlags:timestamp:windowNumber:context:subtype:data1:data2:"),
        15UL, zero, 0UL, 0.0, 0L, (id)0, (short)0, 0L, 0L);
    MSG(void, id, BOOL)(g_app, SEL_("postEvent:atStart:"), ev, YES);
  });
}

/* Length of the valid UTF-8 sequence at s, or 0. */
static int utf8_seq(const unsigned char *s) {
  if (s[0] < 0x80) return 1;
  int n = s[0] >= 0xC2 && s[0] <= 0xDF ? 2 : s[0] >= 0xE0 && s[0] <= 0xEF ? 3 : s[0] >= 0xF0 && s[0] <= 0xF4 ? 4 : 0;
  if (n == 0) return 0;
  for (int i = 1; i < n; i++)
    if ((s[i] & 0xC0) != 0x80) return 0;
  if (n == 3 && ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0))) return 0;
  if (n == 4 && ((s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90))) return 0;
  return n;
}

/* NSString refuses invalid UTF-8 outright, so each bad byte becomes U+FFFD. */
static void buf_put_valid_utf8(Buf *b, const char *text) {
  const unsigned char *s = (const unsigned char *)text;
  while (*s) {
    int n = utf8_seq(s);
    if (n) { buf_put(b, (const char *)s, (size_t)n); s += n; }
    else { buf_put(b, "\xEF\xBF\xBD", 3); s++; }
  }
}

void motu_post(int64_t win, const char *json) {
  Buf b = {0};
  buf_put(&b, "window.__motu_recv(", 19);
  buf_put_valid_utf8(&b, json);
  buf_put(&b, ")", 1);
  char *js = b.p;
  dispatch_async(dispatch_get_main_queue(), ^{
    if (valid(win))
      MSG(void, id, id)(g_views[win], SEL_("evaluateJavaScript:completionHandler:"), nsstr(js), (id)0);
    free(js);
  });
}

#endif
