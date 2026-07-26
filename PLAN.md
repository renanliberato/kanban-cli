# Kanban CLI — Plan

## Goal
A kanban board with a terminal UI, written in C, high-quality UX.
Target platform: macOS Intel (source-portable; verified builds on Linux box).

## Locked decisions (from user)
- **Build strategy**: portable C. Build/test/e2e on the Linux box after every
  commit. Final macOS Intel build happens on the user's machine via `make`.
  No osxcross.
- **TUI**: ncurses.
- **Persistence**: JSON via vendored single-file cJSON.
- **v1 features**:
  - Core: 3 fixed columns (To Do / Doing / Done), card add/edit/delete,
    move cards between columns with keyboard.
  - Keyboard navigation (arrows + hjkl), selection highlight, colored
    column headers.
  - Configurable board file path via CLI arg; default `~/.kanban.json`;
    autosave on every change.
  - NO custom columns in v1.

## Repo layout (target)
```
Makefile
vendor/cJSON.c, vendor/cJSON.h
src/main.c          — entry, arg parsing
src/board.c/.h      — model + JSON load/save
src/tui.c/.h        — ncurses UI
tests/unit/test_board.c       — unit test harness (54 asserts)
tests/bdd/                    — behave/gherkin BDD suite (see below)
scripts/verify.sh             — build + unit tests + behave (runs on box)
README.md
```

## Milestones (one or more commits each, all verified on box)
- M1: skeleton — Makefile, vendored cJSON, board model, JSON save/load,
  unit tests. `make` + unit tests green on box.
- M2: ncurses TUI — render 3 columns, navigation, colors, selection.
- M3: card CRUD + move + autosave.
- M4: pexpect e2e tests, UX polish, README with macOS build instructions.

## Iteration 3 Milestones
- M1: SQLite storage with JSON auto-migration
- M2: LLM seam + fake provider
- M3: CLI subcommands + AI enrich with human-in-the-loop review
- M4: Card detail view, label picker, fuzzy filter
- M5: Adaptive narrow layout, undo, archive
- M6: Background jobs polish + multiple boards (Q&A superseded by M7)
- M7: Comments + Agent primitives (user-requested); review flow removal

## Iteration 2 — BDD Test Layer

The e2e.py pexpect suite was replaced by a full BDD test layer using
`behave` (Python gherkin).  10 feature files cover every TUI operation in
plain business language: startup, navigation (hjkl + arrows), card add
(+ ESC/empty cancel), card edit (+ ESC cancel), card delete (y confirm,
n/other cancel), card move (H/L and angle brackets with boundary no-ops),
persistence (quit/restart), autosave (file-on-disk checks while app is
running), quit from various states, and responsiveness at 80x24 and
120x40.  Step definitions drive the binary via pexpect in a pty following
the same proven patterns from the original e2e.py.

- Framework: `python3-behave` installed via apt on the box.
- Layout: `tests/bdd/features/*.feature` + `tests/bdd/steps/kanban_steps.py`
  + `tests/bdd/environment.py`.
- Suite size: **36 scenarios, 200 steps**, passing with zero failures.
- `verify.sh` now runs behave instead of e2e.py (unit tests remain untouched).

## Verify-on-box loop (after EVERY commit)
1. Sync source to box (tar over box ssh with `COPYFILE_DISABLE=1`).
2. `scripts/verify.sh` on box: clean build, unit tests, behave.
3. If anything fails: fix and amend the commit. Never commit red.
