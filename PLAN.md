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
tests/unit/test_board.c  — tiny assert-based harness
tests/e2e.py        — pexpect e2e driving the binary in a pty
scripts/verify.sh   — build + unit tests + e2e (runs on box)
README.md
```

## Milestones (one or more commits each, all verified on box)
- M1: skeleton — Makefile, vendored cJSON, board model, JSON save/load,
  unit tests. `make` + unit tests green on box.
- M2: ncurses TUI — render 3 columns, navigation, colors, selection.
- M3: card CRUD + move + autosave.
- M4: pexpect e2e tests, UX polish, README with macOS build instructions.

## Verify-on-box loop (after EVERY commit)
1. Sync source to box (tar over box ssh).
2. `scripts/verify.sh` on box: clean build, unit tests, e2e tests.
3. If anything fails: fix and amend the commit. Never commit red.
