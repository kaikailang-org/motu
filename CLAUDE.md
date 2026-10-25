# motu

Desktop framework for kaikai: native kaikai backend, system webview UI. The
plan lives in `docs/plan.md`; this file holds the conventions.

- Commits, PR titles and bodies, code comments and every doc are English.
- Conventional Commits (`feat(ipc): …`, `fix(shim): …`).
- Comments are brief, timeless or absent: no issue numbers, dates or narrative
  in code. Every `pub` symbol carries a one-line `#[doc]`.
- `kai info <topic>` is the authority on kaikai syntax; check it before
  writing a `.kai` snippet.
- New files score `km score` C or better; files stay under 1000 lines.
- Platform code lives in `c/`, one file per platform behind `c/motu.h`.
  Everything above the shim is kaikai.
- Figures in docs are marked (M) measured or (R) read, and name the machine
  and backend they were measured on.
