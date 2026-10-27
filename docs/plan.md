# Motu — plan

Motu builds desktop applications in kaikai: a native kaikai backend and a
user interface drawn by the system webview, in one process. It has the shape
of Tauri: the operating system already ships a browser engine, so an
application does not bundle one.

The name is Rapa Nui. The *motu* are the islets off the island's southwest
cape; on Motu Nui the manutara laid the egg of the birdman contest.
Manutara is kaikai's web framework, and on
the desktop it lives on Motu.

Marks used below: **(M)** measured, **(R)** read in documentation or source,
not yet measured.

## Thesis

Tauri guards the boundary between the page and the backend with capability
files: JSON allowlists of the commands and plugins a window may call, checked
at run time. kaikai already has that boundary in the type system.

- **A command's effect row is its capability.** Every command the page can
  call is a kaikai function with a closed row. The application declares the
  row its commands may use (`type Granted = Project + Dialog + Clock`), and a
  command that performs anything else — `Process`, a socket, the whole file
  system — is a type error, not a denied request at run time.
- **File access is a handler, not a path list.** The `Project` effect reads and
  writes inside the open project. Its handler resolves every path against the
  project root and refuses one that escapes it; the command never sees the
  real file system.
- **The window is a nursery.** Every task a window starts — an export, a
  search, an autosave timer — runs in that window's scope. Closing the window
  cancels them, and no task outlives the window that owns it. This is the
  same move terevaka makes for the terminal.
- **The boundary is typed in both directions.** Requests and replies are
  records with `#[derive(Json)]`; one declaration produces the decoder on the
  backend and the client the page calls.

## Architecture

```
┌──────────────────────────── one process ─────────────────────────────┐
│  main thread: platform event loop (AppKit / GTK), inside motu_run()  │
│    window ── webview ── page (HTML/CSS/JS, or kaikai on wasm)        │
│       │ script message                ▲ evaluate / post              │
│       ▼                               │                              │
│  C shim (c/): exported kaikai callbacks, motu_dispatch()             │
│       │ mailbox_send                  │ reply                        │
│       ▼                               │                              │
│  kaikai scheduler: the app actor, one fiber per command, per window  │
│  nursery, Project / Dialog / Clock handlers                          │
└──────────────────────────────────────────────────────────────────────┘
```

- **Event loop.** `main` calls `motu_run`, an FFI call that runs the platform
  loop on the main thread, as AppKit requires. The shim calls back into
  kaikai through `extern "C"` exported functions. A callback that arrives
  inside that FFI call runs synchronously on the calling fiber (R:
  `kai info ffi`), so callbacks only translate and `mailbox_send` to the app
  actor, then return. They never wait. Replies reach the page through
  `motu_dispatch`, which posts to the main thread.
- **Scheduler.** Commands run on scheduler fibers (`KAI_THREADS > 1`), never on
  the main thread. A long command does not freeze the window.
- **Assets.** The page loads from a custom scheme (`motu://app/…`) that the
  shim serves from the application bundle: `WKURLSchemeHandler` on macOS, a
  registered URI scheme in WebKitGTK (R). There is no localhost server and no
  open port. The scheme also serves `.wasm` with its MIME type, which a page
  written in kaikai needs.
- **IPC.** One message channel per window, carrying JSON:

  | message | direction | meaning |
  |---|---|---|
  | `call {id, cmd, args}` | page → backend | run command `cmd` |
  | `reply {id, ok \| err}` | backend → page | its result |
  | `cancel {id}` | page → backend | cancel the command's fiber |
  | `event {name, data}` | backend → page | push (menu chosen, file dropped, progress) |

  A command that streams (an export reporting progress) sends events tagged
  with its `id` before its reply.
- **Native surface in the shim.** Windows, menus, dialogs, printing and the
  scheme handler are platform code: Objective-C over AppKit and WebKit on
  macOS, C over GTK 4 and WebKitGTK 6.0 on Linux. Everything above the shim is
  kaikai.

### Measured so far

macOS, Apple M4 Pro, native backend:

- Empty command round trip, page → kaikai fiber → page: 106–124 µs over 2 000 calls (M).
- A command sleeping 5 s, cancelled by the page after 50 ms: rejected after 51–53 ms (M).
- Print to PDF through the webview: 18 pages from three chapters, 12 from a 3 420-word
  chapter with footnotes (M).
- Markdown parse and render of 150 000 words: 28 ms native, 16 ms on the C backend (M).

The book application on the same Mac, with a manuscript of 164 775 words in 400
documents (`tools/make-bench-book.py`, run with `#bench=<project>|<dir>`), all (M):

| step | time |
|---|---|
| process start to first contentful paint, then quit (`hello`, warm file cache) | 0.23–0.28 s; paint 41–48 ms after the page starts |
| open the project: load, word counts, first chapter | 87–90 ms |
| word counts of every document | 42 ms |
| search the project | 168 ms |
| outliner of the whole manuscript | 46–48 ms |
| save a 10 000-word chapter | 12 ms |
| read it back, an 81 KB reply | 5 ms |
| compile to Markdown / HTML | 24 / 51 ms |
| compile to EPUB 3 (epubcheck: 0 errors) / Word | 360 / 117 ms |
| compile to PDF | 1.4–1.6 s |
| resident memory of the process at its peak | 132–186 MB |
| `.app` bundle: the book application / `hello` | 2.2 MB / 720 KB |

macOS 15.7.9, GitHub `macos-15` runner (Apple M1, virtual), native backend, a
fresh machine on every run so the first launch finds WebKit and the program
cold (M):

- Start to first paint, then quit: 1.76 s on the first launch; 0.43–0.81 s
  on the next three. First paint 6–146 ms after the page starts.
- Every example's self-test passes. JavaScriptCore on macOS 15 aborts when it
  starts on a fiber stack, so the shim runs the event loop on the main
  thread's own stack; macOS 26 tolerated it, which is why only this runner
  showed it.

Linux, GitHub `ubuntu-24.04` runner under Xvfb, native backend:

- Start to first paint, each launch starting its own Xvfb: 6.4 s on the first
  launch on a fresh runner, 0.7–0.9 s after; first paint 31–36 ms after the
  page starts (M).
- Menus by mouse and by shortcut, checkmarks, the open, save and message
  dialogs (answers and cancel), a real XDND file drop, shell open and reveal,
  focus, blur, resize and close requests: driven with real X input under
  Xvfb and openbox by `tests/interactive`, each asserted in CI (M). Wayland
  and the desktop portal's dialogs are not covered.
- AppImages built by `motu build`: `hello` 1.26 MB, the book application
  1.90 MB; both run from `/` and pass their self-tests (M).
- Empty command round trip: 343–610 µs (M); GTK runs on a thread of its own there
  (JavaScriptCore refuses a stack it does not own), so each call crosses one more queue.
- Every example's self-test passes, the book application included (M).

Both:

- Invalid UTF-8 sent to the page is repaired with U+FFFD in the shim, because WebKit
  drops a script that is not valid UTF-8 (M).

### Why an own shim, not `webview/webview`

`webview/webview` gives a window, `bind`, `eval` and `dispatch` on the three
platforms (R). A Scrivener-class application needs more than that: native
menus with accelerators, open and save dialogs, a veto when the user closes a
window with unsaved changes, a custom scheme, print to PDF, and more than one
window. None of these is in `webview/webview`, so it would be a dependency
plus a second shim beside it. Tauri's own layers (`wry`, `tao`) are Rust and
would add a Rust toolchain to every kaikai build. The shim is the main body
of C work in Motu, and it is bounded: one file per platform behind one header
(`c/motu.h`).

## Platforms

| platform | stage 1 | engine |
|---|---|---|
| macOS 13+, arm64 and x86_64 | yes | WKWebView (system Safari) |
| Linux x86_64 and arm64, current distributions | yes | WebKitGTK 6.0 on GTK 4 |
| Windows | no | WebView2 |

Windows waits on kaikai itself: its runtime does not build for Windows today.
A spike on a GitHub Windows runner measured 46 compile errors in the runtime
with every host facility turned off (mingw-w64 clang), 283 under the MSVC
target, while kaikai's stage-0 compiler builds clean (M). The kaikai repository also cannot be
checked out on Windows, because a fixture is named `aux.kai`, a reserved
device name (M). Motu's shim keeps a WebView2 file in its plan so that the
port, once kaikai runs there, is one platform file.

## Stage 1 — enough for a Scrivener-class book application

Stage 1 is done when the reference application below runs on macOS and Linux
from one source tree. The application is the acceptance test: every
capability in stage 1 is there because the application needs it.

### The reference application (`examples/book`)

A writing studio for long books, modelled on Scrivener:

- **Binder**: the project tree — manuscript, research, trash — with folders
  and documents, drag to reorder, rename in place.
- **Editor**: every document is a Markdown file — headings, emphasis, lists,
  block quotes, footnotes (`[^1]`), comments — edited with live styling, in a
  split view, and the manuscript read as one continuous text ("scrivenings").
- **Corkboard**: one index card per document with its synopsis, coloured by
  label.
- **Outliner**: the tree as a table — title, synopsis, label, status, word
  count, target.
- **Inspector**: synopsis, notes, label, status, keywords and snapshots of the
  selected document.
- **Snapshots**: a dated copy of a document, compared with and restored over
  the current one.
- **Targets**: words per document, per manuscript, and per writing session.
- **Search**: the whole project, with saved searches as collections.
- **Composition mode**: full screen, one column, nothing else on screen.
- **Research**: images and PDFs imported into the project and shown beside
  the text.
- **Safety**: autosave a moment after typing stops; a zip backup of the
  project on close, rotated.
- **Compile**: the manuscript out as Markdown, HTML, EPUB 3, DOCX and PDF,
  with front matter and a chapter layout.

### What Motu provides, and why

| the application needs | Motu capability | depends on |
|---|---|---|
| a main window, composition mode, a second window | `motu.window`: create, title, size, minimum size, full screen, close veto, focus and resize events | shim |
| File, Edit, View, Project menus; ⌘S, ⌘K…; context menu on the binder | `motu.menu`: menu bar, items with accelerators, enabled and checked state, context menus; choices arrive as events | shim |
| open a project, choose an export path, confirm a discard | `motu.dialog`: open file or folder, save, message, confirm | shim |
| every page action calls the backend | `motu.ipc`: commands with `#[derive(Json)]` records, replies, cancellation, events, streams | kaikai `extern "C"` export |
| export with a progress bar, a search that the user abandons | `motu.task`: a command runs on its own fiber in the window's nursery; `cancel` from the page cancels it; progress as events | kaikai fibers and `Cancel` |
| saving without losing work | `motu.project`: the `Project` effect, confined to the project root; atomic write (temporary file, flush, rename) | kaikai `fs`; a flush to disk |
| the project as a bundle of files | a documented bundle layout (below) and recent-projects list in the application data directory | `fs`, `encoding.toml` |
| project search | `motu.search`: a full-text index in memory, rebuilt from the files when a project opens | none |
| EPUB, DOCX and backups | zip writer: stored entries and CRC-32 for EPUB, deflate for backups | Motu: `motu.zip`, `motu.deflate`, `motu.crc32` |
| compile reads the manuscript | Markdown parser to a syntax tree (CommonMark plus footnotes, GFM tables and strikethrough) | Motu: `motu.markdown` |
| EPUB and DOCX contents | XML writer with escaping; the EPUB 3 package | Motu: `motu.xml`, `motu.epub` |
| PDF | print the compiled HTML through the webview to a PDF file | shim: a print operation to file on both platforms, paginated (M) |
| spelling | the engine's spell checker on editable text | shim: WebKit's spelling default on macOS, the user's locale on WebKitGTK; CI counts the marks on both (M) |
| images dropped on the binder | file drop on the window, delivered as paths | shim |
| open a research file in its own application, show it in the file manager | `motu.shell`: open with the default application, reveal in Finder or the file manager | shim |
| paste from a word processor | the engine's clipboard, plus `motu.clipboard` for text from the backend; turning pasted HTML into Markdown is the application's | shim |
| unsaved changes on quit | quit request as an event the backend can veto | shim |
| dark mode | `prefers-color-scheme` in the page; the system appearance as an event | engine; shim |
| one command to start, run and ship | `motu new`, `motu dev` (rebuild and reload on save), `motu build` (`.app` bundle; AppImage and tarball) | kaikai `kai build` |
| tests without a window | command handlers are ordinary functions; `kai test` runs them against an in-memory `Project` handler | kaikai test runner |

### The project bundle

```
My Novel.book/
  project.toml          binder order, labels, status list, targets, compile settings
  docs/<uuid>.md        one document, Markdown with TOML front matter
  notes/<uuid>.md       its notes
  snapshots/<uuid>/<utc-timestamp>.md
  research/<uuid>.<ext> imported images and PDFs
```

```markdown
+++
title = "The harbour"
synopsis = "Ana arrives the night the lights fail."
label = "draft"
status = "first pass"
target = 3000
+++

The ferry came in after dark.[^1] …

[^1]: The last ferry, not the usual one.
```

Every document is a Markdown file another editor can open, so a project syncs
through Dropbox or git and survives the application. The front matter keeps a
document's metadata with its text; `project.toml` holds only what spans
documents. Files are named by a stable id, so reordering or renaming in the
binder rewrites `project.toml` and the title, never a file name. Comments are
written inline as `<!-- … -->`, which every Markdown renderer drops. The search
index lives in memory and is rebuilt on open, so the bundle holds nothing
derived.

### What stage 1 measures

Each figure is reported with the machine and the backend it was measured on.

- Cold start to first paint of the main window.
- IPC round trip of an empty command, and of one that carries a 50 KB
  document.
- Save of a 10 000-word chapter: latency from the page's call to the reply.
- A 150 000-word manuscript in 400 documents: open, search, word counts,
  compile to EPUB and to PDF.
- Resident memory with that project open.
- Size of the `.app` bundle and of the AppImage.

### Smaller applications along the way

The book application arrives last. Before it, each milestone closes with a
small application that uses what the milestone added and runs on both
platforms. They stay in `examples/` as the shortest working program for each
capability, and they are what `motu new` offers as templates.

| example | what it is | exercises | milestone |
|---|---|---|---|
| `hello` | a window that calls one command and shows the reply | window, scheme, channel | 1 |
| `md-viewer` | open or drop a Markdown file, read it rendered, reload, print to PDF | menus, dialogs, file drop, appearance, Markdown parser, PDF | 2, 5 |
| `file-finder` | pick a folder, find files by name and content as you type, open or reveal a result | streamed results, cancellation on each keystroke, `motu.shell` | 3 |
| `notes` | a folder of Markdown notes with autosave and full-text search | `Project` confinement, atomic writes, autosave, in-memory search | 4 |
| `md-to-epub` | Markdown files as chapters, out as EPUB and DOCX, with a progress bar | compile pipeline, zip and XML writers, progress events | 5 |
| `book` | the Scrivener-class studio above | all of stage 1 | 7 |

### Milestones

1. **Window and channel.** `motu_run`, one window, the `motu://` scheme, one
   typed command and its reply, on both platforms. This is stage 0 below, and
   it settles the callback model before anything is built on it. Closes with
   `hello`.
2. **Native surface.** Menus, dialogs, close and quit veto, full screen, a
   second window, file drop, appearance events. Closes with `md-viewer`
   showing the file as text; it renders once milestone 5 brings the parser.
3. **Tasks.** Per-window nurseries, cancellation from the page, progress
   events, an autosave timer, `motu.shell`. Closes with `file-finder`.
4. **Project.** The `Project` effect and its confinement, atomic writes, the
   bundle, recent projects, in-memory search. Closes with `notes`.
5. **Compile.** The Markdown parser, zip and XML writers; out to Markdown,
   HTML, EPUB 3, DOCX, and PDF through the webview. Closes with `md-to-epub`
   and `md-viewer` rendering.
6. **Tooling.** `motu new` (from the examples above), `motu dev`, `motu
   build`, the macOS bundle and the Linux AppImage. The AppImage carries the
   program and its assets; GTK and WebKitGTK stay the system's, loaded at run
   time.
7. **The application.** `examples/book` complete, and the measurements above.

### Where the book application lives

In this repository, as `examples/book`, while stage 1 runs: it is Motu's
acceptance test, one pull request changes the framework and the application
together, and Motu's CI builds it on every change. It moves to a repository
of its own when it starts to rest on ahu, or when it becomes a product of its
own, whichever comes first; otherwise Motu's CI would carry ahu, kohau and
the editor as its own dependencies. The small examples stay.

## Stage 0 — the spike that comes first

Before milestone 1 is written as the framework, a throwaway spike on macOS
and Linux answers four questions with measurements:

1. Does the callback model hold: the main fiber inside `motu_run`, callbacks
   arriving synchronously on it, commands on other workers, replies posted
   back with `motu_dispatch`? Measured under `KAI_THREADS=1` (expected to be
   refused) and the default.
2. What does an empty round trip cost, page to kaikai and back?
3. Do `[native]` sources build Objective-C and link the frameworks
   (`WebKit`, `AppKit`)? The manifest has no key for frameworks today (R).
4. Does the system WebKit run a page written in kaikai? A kaikai page needs
   exception handling with exnref (Safari 18.4) and, to spawn fibers, JSPI
   (Safari 27) (R); the WebKitGTK versions of current distributions decide
   the same on Linux.

Answers, measured (M) on macOS 26.6.2 (Apple M4 Pro) and on the GitHub
`ubuntu-24.04` runner:

1. The model holds. `main` blocks in `motu_run` and every callback arrives on
   it; commands run on other fibers. Under `KAI_THREADS=1` the program would
   hang, since the only scheduler thread is blocked in the event loop, so
   `motu_init` refuses it with a message and exit status 2.
2. 106–124 µs on macOS, 343–610 µs on the Linux runner (figures above).
3. Not needed: the shims are C over the Objective-C runtime and load every
   framework with `dlopen`, so `[native]` takes C sources only and links
   nothing.
4. In part. A kaikai page built with `--target=wasm32-browser` runs in
   WKWebView on macOS 26.6.2: that WebKit has exception handling with exnref.
   A page that spawns a fiber does not: there is no JSPI, and the host says
   so instead of failing silently. WebKitGTK on the Linux runner answers the
   same (`tests/wasm-page`, in CI). So stage 1
   pages are JavaScript, as decided, and kaikai pages wait for JSPI in the
   system WebKit.

## Motu and ahu

Motu depends on kaikai alone: its tasks are fibers in a per-window nursery,
its channel delivers to an actor's `Pid`. It neither requires
[ahu](https://github.com/kaikailang-org/ahu) nor assumes it. The reference
application's state — the open project, the autosave timer, the search
index, the export queue — will in time rest on ahu's cells and restart
helpers; that is the application's work and ahu's, not Motu's. What Motu owes
it is not to stand in the way: a command forwards to any `Pid`, so a cell
can own the state behind it.

## Stage 2 — after the application ships

- Pages written in kaikai with Manutara, on the same channel; the typed
  client becomes a kaikai import instead of generated JavaScript.
- Windows through WebView2, once kaikai runs on Windows.
- Signing and notarization on macOS; Flatpak on Linux.
- Updates, file associations (double-click a `.book`), one running instance.

## Upstream dependencies

| need | where | state |
|---|---|---|
| C calls kaikai (`extern "C"` with a body), from a GUI thread too | kaikai | shipped (R); verified in stage 0 |
| Objective-C sources and macOS frameworks in `[native]` | kaikai | not needed: the shims are C over the Objective-C runtime and load their frameworks with `dlopen` (M) |
| `extern "C"` exports declared in an imported module of a package | kaikai | shipped (lnds/kaikai#3164); `motu.shim` declares the callbacks |
| the compiler bugs stage 1 ran into: mutual tail calls (with effects, type or effect-row parameters) in constant stack on every backend, a `Vec` in a record field or read through an owned parameter written in place, effect-row aliases in function types, a module cycle reading a field, `Random` seeded from the OS | kaikai | shipped in 0.178.0 (lnds/kaikai#3175–#3179); Motu no longer works around any of them |
| zip writer (stored + CRC-32; deflate) | Motu | shipped |
| flush a file to disk (`fsync`) for atomic saves | Motu shim | `motu_fsync_path` (`F_FULLFSYNC` on macOS) |
| XML writer | Motu | shipped |
| Markdown parser (CommonMark plus footnotes, GFM tables and strikethrough) to a syntax tree | Motu | shipped: `motu.markdown` |
| Windows runtime | kaikai | not started; spike numbers above |

Each missing kaikai piece becomes an issue in `lnds/kaikai` when its
milestone starts.

## Decisions taken by the owner

All five are decided: the owner confirmed the recommended option for items 1
to 4 (own shim; HTML/CSS/JavaScript pages; CodeMirror 6; GTK 4 with
WebKitGTK 6.0) and named the application "Book". The options as they were
weighed:


1. **Shim**: Motu's own per-platform shim (recommended, for the reasons
   above) or `webview/webview` plus a second shim.
2. **Stage 1 page language**: HTML/CSS/JavaScript with a generated typed
   client (recommended: it does not wait on Manutara or on JSPI in the system
   WebKit), or kaikai on wasm from the first milestone.
3. **Editor in the reference application**: a vendored CodeMirror 6 (MIT)
   styling the Markdown as it is typed (recommended: the file stays exactly
   as written), or a rich-text editor such as ProseMirror that parses the
   Markdown and writes it back, which normalises the author's formatting on
   every save. This is the application's choice, not Motu's: Motu ships no
   editor.
4. **Linux stack**: GTK 4 with WebKitGTK 6.0 (recommended, current
   distributions only) or GTK 3 with WebKitGTK 4.1.
5. **Name of the reference application.**
