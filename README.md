# Kanban CLI

A terminal kanban board written in C with ncurses. Three columns (To Do, Doing, Done), full keyboard control, and JSON persistence.

```
+----------------------+----------------------+----------------------+
|    To Do (3)         |    Doing (1)         |    Done (2)          |
+----------------------+----------------------+----------------------+
| buy groceries        | write TUI            | setup project        |
| walk the dog         |                      | fix bugs             |
| plan vacation        |                      |                      |
|                      |                      |                      |
+----------------------+----------------------+----------------------+
 q quit  |  hjkl navigate  |  a add  e edit  d del  H/L move
```

## Key Bindings

| Key          | Action                          |
|-------------|---------------------------------|
| `h` / `←`   | Move selection left             |
| `j` / `↓`   | Move selection down             |
| `k` / `↑`   | Move selection up               |
| `l` / `→`   | Move selection right            |
| `a`         | Add a card                      |
| `e`         | Edit the selected card          |
| `d` then `y`/`n` | Delete (confirm / cancel) |
| `H` / `<`   | Move selected card left         |
| `L` / `>`   | Move selected card right        |
| `Esc`       | Cancel current input            |
| `q`         | Quit                            |

## Build & Run

### macOS (Intel)

macOS ships with system ncurses that works out of the box. If you prefer a newer version, install via Homebrew:

```
brew install ncurses
```

But the default system ncurses is fine. Build:

```
make
./bin/kanban [board-file]
```

If no board file is given, `~/.kanban.json` is used by default.

### Linux

```
sudo apt install build-essential libncurses-dev
make
./bin/kanban [board-file]
```

## Development

- **Unit tests**: `make test` — builds and runs 54 unit tests against the board model.
- **Full verification**: `bash scripts/verify.sh` — clean build, unit tests, and 13 e2e scenarios via pexpect.
- **E2E tests only**: `python3 tests/e2e.py ./bin/kanban`

### Project Layout

```
Makefile                  — build (gcc, C99, ncurses)
vendor/cJSON.c, .h        — vendored JSON library
src/
  main.c                  — entry point, arg parsing, ESCDELAY setup
  board.c, .h             — board model: add/edit/delete/move cards, JSON save/load
  tui.c, .h               — ncurses TUI: render, navigation, input, status bar
tests/
  unit/test_board.c       — model unit tests (54 assertions)
  e2e.py                  — pexpect end-to-end suite (13 scenarios)
scripts/
  verify.sh               — full CI script (build + unit + e2e)
```

### Data File Format

The board is stored as JSON:

```json
{
  "next_id": 6,
  "columns": [
    {
      "name": "To Do",
      "cards": [
        {"id": 1, "title": "buy groceries"},
        {"id": 2, "title": "walk the dog"}
      ]
    },
    {
      "name": "Doing",
      "cards": [
        {"id": 3, "title": "write TUI"}
      ]
    },
    {
      "name": "Done",
      "cards": [
        {"id": 4, "title": "setup project"},
        {"id": 5, "title": "fix bugs"}
      ]
    }
  ]
}
```

The file is autosaved on every change and on quit. If the file does not exist at startup, an empty board is created.

## Verified On

- **Linux**: Ubuntu 24.04, gcc 13, ncurses 6, Python 3 + pexpect — full CI green.
- **macOS**: Intel, system ncurses — builds and runs identically. Build with `make`.
