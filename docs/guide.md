# Motu guide

A Motu application is a kaikai program with a page. The page is HTML, CSS and
JavaScript served from the application's `assets/` directory as
`motu://app/…`; the program answers the page's calls.

## Start

```sh
kai build tools/motu/main.kai -o build/motu      # once, from this repository
build/motu new ~/src/notes --template notes      # hello, md-viewer, file-finder, notes, book
build/motu dev ~/src/notes                       # build, run, rebuild and restart on every save
build/motu build ~/src/notes                     # macOS: build/notes.app; Linux: build/notes-linux/ and a tarball
```

The new application's `kai.toml` depends on Motu by path. Its `main.kai`
imports `motu.app` and calls `app.run` from `main`: the platform's event loop
needs the main thread, and kaikai runs `main` there.

## The program

```kai
import encoding.json
import encoding.json_bind
import motu.app
import motu.desktop
import motu.menu

fn command(c: Call) : Result[JsonValue, String] / Desktop + Dialog = match c.cmd {
  "greet" -> Ok(JStr("Hello, #{app.arg_string(c, "name") ?? "you"}"))
  "pick"  -> Ok(match Dialog.open_folder(c.win, "Choose a folder") { Some(p) -> JStr(p); None -> JNull })
  other   -> Err("unknown command #{other}")
}

fn main() : Int / Spawn + Ffi + File + Stdout = {
  let cfg = Config {
    name: "Hello", assets: "assets", page: "index.html",
    title: "Hello", width: 640, height: 400, menus: [menu.edit_menu()],
  }
  app.run(cfg, c => command(c), ev => app.default_event(ev))
}
```

- **Commands.** Every call from the page runs `command` on a fiber of its own,
  so a slow command never blocks another. The reply is the `Ok` value, or the
  `Err` text, which rejects the page's promise.
- **Capabilities.** A command's row says what it may do. Motu answers four
  effects: `Desktop` (windows, events to the page, menu state, printing,
  quitting), `Dialog`, `Shell` and `Clipboard`. Anything else the command
  performs (`File`, `Clock`, …) must appear in `main`'s row too, so the program's
  signature lists what its page can reach.
- **Pass functions as lambdas.** `app.run(cfg, c => command(c), …)`: a named
  function with a closed row cannot widen to the row `run` expects, a lambda
  can.
- **Files.** `motu.project` gives the `Project` effect: paths relative to one
  directory, refusing absolute paths and `..`, with atomic writes (temporary
  file, flush to disk, rename). Wrap a command's work in
  `project.with_project(root, () => …)` and give it `/ Project` instead of
  `/ File`.
- **Events.** Menu choices, files dropped on the window, close and quit
  requests, focus and resize reach the second function as `Event` values.
  `app.default_event` closes a window when asked and quits when asked; an
  application with unsaved work answers `CloseRequested` itself (see
  `examples/book`: it asks the page to save, then closes).
- **Cancellation.** Closing a window cancels the commands it started; the page
  can cancel one call. A cancelled command stops at its next effect operation.

## The page

The page gets `window.motu`:

```js
const reply = await motu.invoke('greet', { name: 'Ana' });   // rejects with the command's Err text
const call = motu.invoke('search', { query });               // call.id identifies it
motu.cancel(call);                                           // the promise rejects with "cancelled"
motu.on('hit', (data) => { … });                             // events from Desktop.emit(win, "hit", data)
```

`motu://app/<path>` serves the assets; `motu://files/<path>` serves the
directory a command named with `Desktop.serve_files(root)`, so a page can show
a project's images and PDFs.

## Testing

- Commands are functions: test them with `kai test` under your own handlers
  (`project.with_project` over a temporary directory, see `tests/`).
- A page can test itself end to end: the built-in command `motu.report` prints
  its argument and quits. Every example reads `MOTU_PAGE`, so
  `MOTU_PAGE='index.html#selftest' ./build/hello` runs its self-test and prints
  one line.

## Platforms

macOS (WKWebView) and Linux (WebKitGTK 6.0 on GTK 4). The shims are C behind
`c/motu.h` and load their frameworks at run time, so building needs only
kaikai and a C compiler.
