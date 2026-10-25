# motu

Desktop applications in [kaikai](https://github.com/lnds/kaikai): a native
kaikai backend and a user interface drawn by the system webview, in one
process. Tauri-shaped: the operating system ships the browser engine, so an
application does not bundle one.

> **Status:** plan. No code yet. See [`docs/plan.md`](docs/plan.md).

What sets it apart:

- **A command's effect row is its capability.** The page can call only kaikai
  functions whose closed rows the application grants; anything else is a type
  error, not a denied request at run time.
- **File access is a handler.** The `Project` effect confines every read and
  write to the open project.
- **The window is a nursery.** Closing a window cancels every task it
  started.
- **The boundary is typed.** Requests and replies are `#[derive(Json)]`
  records, decoded on one side and called through a generated client on the
  other.

Stage 1 targets macOS and Linux, and is done when a Scrivener-class
book-writing application (`examples/book`), whose documents are Markdown
files, runs on both from one source tree.

The name is Rapa Nui: the *motu* are the islets off the island's southwest
cape, where the manutara nested. Manutara is kaikai's web framework; on the
desktop it lives on Motu.
