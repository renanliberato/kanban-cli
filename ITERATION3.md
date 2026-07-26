# Kanban CLI — Iteration 3 Design

## Part 1 — Ranked Feature List

| # | Feature | Pitch | UX Sketch | Why It Earns Its Place | Cost |
|---|---------|-------|-----------|------------------------|------|
| 1 | **Non-interactive CLI** | `kanban add "fix login" --ai --col doing` — scriptable entry, feed-by-AI, feed-by-cron | Subcommands: `add`, `enrich <id>`, `ask <id> "q"`, `list`, `show <id>`. No TUI launch; returns JSON or plain text. | The "AI-native both directions" promise is hollow if AI agents can't write to the board. Also enables shell scripting, git hooks, and the BDD suite can test AI flows without a TTY. | M |
| 2 | **Card detail view** | `Enter` opens a full-screen card: title, multiline description, labels, Q&A log, timestamps, history | `Enter` → detail screen (scrollable pager), `e` edit field, `Esc` back. Title bar shows `[Card #4]`. | Cards today are title-only. Every downstream feature (labels, Q&A, AI enrich) needs fields to write into. This is the structural prerequisite. | M |
| 3 | **AI enrich on quick-add** | Type one line → LLM expands into description + labels + clarifying questions → human reviews inline → confirm writes to card | `a` then `Ctrl+E` (or `--ai` flag in CLI) triggers enrich after title is entered. Review screen shows proposed description/labels/questions with accept/edit/reject per field. | The core UX differentiator the user asked for. Maximises human action: human provides intent, LLM does the scaffolding. Human-in-the-loop review keeps trust. | L |
| 4 | **Labels + filter/search** | `/` fuzzy-filters across title, description, labels; labels shown as coloured tags on cards | `/` opens filter bar; real-time narrowing of visible cards. `l` on a card opens label picker. Predefined labels + freeform. | Once a board exceeds ~15 cards, scanning columns is useless. Filter is the gateway to board-at-scale. Labels give the AI structured output to write into. | M |
| 5 | **Adaptive layout** | Single-column tabbed view at narrow widths (<60 cols); columns shrink with text truncation at medium widths | At <60 cols: one column fills the screen, `Tab`/`S-Tab` cycles columns. Column header shows `[To Do (3)]  Doing (1)  Done (2)` with current highlighted. At >=60 cols: current 3-column view. | The board will be used on a desktop but window sizes vary. A 40-col half-screen terminal is a daily reality. This makes it usable everywhere. | S |
| 6 | **Background LLM jobs** | Cards show a subtle `⏳` spinner while LLM is working; TUI never freezes; results arrive and board updates live | Non-blocking `llm_poll()` in main loop. Status-bar shows `[1 job running]`. Completed job → card detail updates + notification flash on the card. `Ctrl+C` on a job cancels it. | Without this, every AI interaction freezes the TUI for 3–30 seconds. Unacceptable for a tool used all day. | L |
| 7 | **Undo** | `u` reverses the last destructive op (delete, move, overwrite); ring buffer of 20 inverse operations | After delete: status bar flashes "Undo? (u)" for 2s. `u` restores card to original column+position. Stack stored in memory, not persisted. | Low-cost safety net. A mis-typed `d`+`y` is devastating without undo. Ring buffer costs ~200 bytes of memory. | S |
| 8 | **Archive / soft-delete** | `x` archives a card instead of `d` deleting it; archived cards hidden by default, `Ctrl+A` toggles visibility | Archived cards get `archived=1` in storage, dimmed in the Done column when visible. `d` still exists for true delete (with undo). | Hard delete is scary. Archive keeps history, enables done-column cleanup without data loss. | S |
| 9 | **Multiple named boards** | `kanban -b myproject` loads `~/.kanban/myproject.db`; default discovers `.kanban/default.db` in cwd or falls back to `~/.kanban/default.db` | `-b` flag overrides board name. Directory `~/.kanban/` holds one SQLite file per board. `kanban list-boards` shows all. | Project-scoped work is the natural growth path. A solo dev juggling 3 projects needs isolated boards. | S |
| 10 | **AI Q&A on a card** | On a card detail screen, `?` lets you ask the LLM a question; answer logged as a Q&A entry on the card; LLM can investigate the local codebase via `opencode run` | Detail screen → `?` → prompt bar "Ask about this card: " → LLM responds (with optional `--dir .` so it can read code), answer displayed in a scrollable pane, saved as a Q&A pair. | Deepens the "AI-native" promise: the board becomes a thinking partner, not just an organiser. Defers well because it depends on detail view + LLM seam + background jobs. | L |

**Killed from seed list:**
- **Priority/due dates**: date management in a terminal is brittle and high-effort to get right (parsing, timezones, notifications). The user explicitly flagged doubt. Kill.
- **Mouse support**: ncurses mouse API (`mousemask`, `MEVENT`) is fragile across terminal emulators, breaks on SSH, and keyboard is objectively faster for this interaction model. Cost is ~M (ncurses mouse handling + drag-and-drop state machine), value is low. Kill.

### Ranking Rationale

The order follows a **tracer-bullet dependency chain**: each feature unlocks the next. Non-interactive CLI (#1) is the thinnest end-to-end slice — get a card into the system from outside the TUI. Card detail (#2) creates the data fields that AI enrich (#3) fills. Labels+filter (#4) gives AI enrichment a structured output target. Adaptive layout (#5) is low-cost, high-daily-impact, so it slots in early. Background jobs (#6) are required before any real AI feature ships, but the LLM module can be built and tested synchronously first. Undo (#7) and archive (#8) are small safety features that can land anytime. Multiple boards (#9) is tiny now that storage is SQLite. AI Q&A (#10) is the capstone — depends on everything above.

---

## Part 2 — Architecture Proposals

### 1. LLM Interface (Clean Seam)

**Module**: `src/llm.h` / `src/llm.c`

**Design principle**: The rest of the codebase never knows it's calling `opencode run`. A compile-time or runtime provider switch hides the implementation. The first provider is `opencode run --format json`. The fake provider (for testing) is an in-process echo.

#### Core Types

```c
/* llm.h */

typedef enum {
    LLM_QUEUED,
    LLM_RUNNING,
    LLM_DONE,
    LLM_FAILED,
    LLM_CANCELLED
} LlmJobState;

typedef struct {
    int          id;            /* unique job id */
    LlmJobState  state;
    char        *prompt;        /* what was sent */
    char        *result;        /* raw stdout (or error message) */
    int          card_id;       /* associated card, -1 if none */
    pid_t        child_pid;     /* 0 when not running */
    int          pipe_fd;       /* read end of stdout pipe, -1 when not running */
    time_t       started_at;
    int          timeout_secs;
} LlmJob;

/* lifecycle */
int  llm_init(void);
void llm_free(void);

/* submit a job — spawns provider asynchronously. Returns job id or -1. */
int  llm_submit(const char *prompt, int card_id, int timeout_secs);

/* poll running jobs — non-blocking. Returns number of jobs that
   transitioned to DONE/FAILED/CANCELLED this call. Call from main loop. */
int  llm_poll(void);

/* access */
const LlmJob *llm_get_job(int job_id);
int            llm_cancel(int job_id);      /* sends SIGTERM to child */
int            llm_job_count(void);
const LlmJob *llm_job_at(int index);        /* iteration */

/* provider abstraction (internal, swapped for testing) */
typedef void (*llm_spawn_fn)(LlmJob *job);
void llm_set_provider(llm_spawn_fn fn);     /* default: opencode_run_provider */
```

#### Async Strategy: fork + exec + pipe + poll

**Recommendation: `fork()` + `exec()` with a non-blocking pipe, polled in the main loop.**

**Why not pthreads:**
- ncurses is famously not thread-safe (POSIX does not require it). All rendering must happen on the main thread.
- Mutexes around the board model add complexity the user (a C novice) will struggle to debug.
- The child process is a separate `opencode run` invocation anyway — threading buys nothing.

**How it works:**
1. `llm_submit()` calls `pipe()` to create a stdout pipe, then `fork()`.
2. Child: `dup2(pipe[1], STDOUT_FILENO)`, close pipe fds, `execvp("opencode", args)`. If exec fails, `_exit(1)`.
3. Parent: close write end, set `O_NONBLOCK` on read end via `fcntl()`, store read fd + child pid in `LlmJob`.
4. `llm_poll()`: for each RUNNING job, `read()` from pipe fd into a per-job buffer. If `read()` returns 0 (EOF), child exited → `waitpid()` to reap, set state to DONE, parse result. If `read()` returns -1 with `EAGAIN`, nothing to read yet. Check timeout via `time(NULL) - started_at`; if exceeded, `kill(SIGTERM)` + set FAILED.
5. Results are applied to the board by the **main loop** after `llm_poll()` returns — never in the signal/poll path. This ensures single-threaded board access.

**`opencode run` invocation** (first provider):

```c
static void opencode_run_provider(LlmJob *job) {
    /* build argv */
    char *argv[] = {
        "opencode", "run", "--format", "json",
        "--dir", g_project_dir,   /* so LLM sees codebase */
        job->prompt,
        NULL
    };
    /* fork + execvp in llm_submit */
}
```

The `--format json` flag (assumed available — spiked during M2 if not) gives machine-parseable output.

**Cancellation**: `llm_cancel()` sends `SIGTERM` to `child_pid`. The poll loop reaps the zombie and sets state to CANCELLED.

**Max concurrent jobs**: capped at 3 (compile-time constant). Prevents fork-bombing.

### 2. Storage: SQLite (Recommended)

**Recommendation: SQLite**, vendored as a single-file amalgamation matching the existing `vendor/cJSON.c` pattern.

#### Deciding Factors (ordered by weight)

| Factor | SQLite | JSON | Winner |
|--------|--------|------|--------|
| **Concurrent access** (CLI + TUI simultaneously) | WAL mode: concurrent readers + one writer, no locking headaches in application code. `kanban add` from a shell while the TUI is open just works. | Needs `flock()`/`fcntl()` advisory locking in every read/write path. Easy to get wrong. A crashed process leaves a stale lock. | SQLite |
| **Incremental writes** | Autosave mutates one row at a time via `UPDATE`/`INSERT`. O(1) per keystroke. Write-ahead log is crash-safe. | Full-file serialize + overwrite on every mutation. Current code writes the entire JSON file on every card add/edit/delete/move. At 100 cards this is noticeable. Truncation on crash = data loss. | SQLite |
| **Growth path** | Schema handles labels, descriptions, Q&A, jobs, boards naturally via new tables and columns. Queries (`WHERE`, `LIKE`, `JOIN`) make filter/search trivial. Full-text search (`FTS5`) available for free later. | Nested JSON for card.labels[], card.description, card.qa[] becomes a hand-rolled tree walk. Filter/search means scanning every card in-process. Every new field means extending save/load serialization. | SQLite |
| **Crash resilience** | WAL + atomic commit. Power loss mid-write cannot corrupt the database. | No atomicity. File truncation on crash means lost data. (Currently we don't even write-to-temp-then-rename.) | SQLite |
| **Vendoring cost** | `sqlite3.c` + `sqlite3.h` is ~250KB, ~8K lines. Single `#include` in the build. Same pattern as cJSON. | Zero new dependency. | JSON (minor) |
| **User debuggability** | `sqlite3 ~/.kanban/default.db "SELECT * FROM cards"` — the user can inspect/modify data with standard tools. | `cat ~/.kanban.json | jq` — also inspectable. | Tie |

#### Schema v1

```sql
PRAGMA journal_mode=WAL;
PRAGMA foreign_keys=ON;

CREATE TABLE schema_migrations (
    version     INTEGER PRIMARY KEY,
    applied_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE boards (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL UNIQUE,
    created_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE columns (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    board_id    INTEGER NOT NULL REFERENCES boards(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    position    INTEGER NOT NULL,
    UNIQUE(board_id, name),
    UNIQUE(board_id, position)
);

CREATE TABLE cards (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    column_id   INTEGER NOT NULL REFERENCES columns(id) ON DELETE CASCADE,
    title       TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    position    INTEGER NOT NULL DEFAULT 0,
    archived    INTEGER NOT NULL DEFAULT 0,
    created_at  TEXT NOT NULL DEFAULT (datetime('now')),
    updated_at  TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE labels (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    name        TEXT NOT NULL UNIQUE,
    color       TEXT NOT NULL DEFAULT 'white'
);

CREATE TABLE card_labels (
    card_id     INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
    label_id    INTEGER NOT NULL REFERENCES labels(id) ON DELETE CASCADE,
    PRIMARY KEY (card_id, label_id)
);

CREATE TABLE qa_entries (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    card_id     INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,
    question    TEXT NOT NULL,
    answer      TEXT NOT NULL,
    created_at  TEXT NOT NULL DEFAULT (datetime('now'))
);
```

#### Migration from existing JSON

On startup, if the JSON file exists and the SQLite database does not:
1. Load the JSON board via existing `board_load()`.
2. Create the SQLite database with schema v1.
3. Insert the default board, 3 default columns, and all cards with positions.
4. Write `schema_migrations` row for v1.
5. The JSON file is **kept** (never deleted — user data is sacred).
6. Subsequent startups open SQLite directly.

#### What changes in board.c?

The `Board` in-memory model stays structurally similar but grows:

```c
typedef struct {
    int     id;
    char   *title;
    char   *description;    /* new */
    char   *created_at;     /* new */
    char   *updated_at;     /* new */
    int     archived;       /* new */
    /* labels: stored as a simple array of strings for now;
       a hash table or linked list would be premature */
    char  **labels;
    int     label_count;
} Card;
```

`board_load()` and `board_save()` become thin wrappers around `db_load()` / `db_save()` in `src/db.c`. The public mutation API (`board_add_card`, `board_edit_card_title`, etc.) is unchanged. The TUI and `main.c` do not change at all — this is the key insulation.

### 3. Data Versioning

**Schema migrations from day one.** Regardless of storage backend:

- Every database/storage format has a `version` integer.
- On load, check `SELECT MAX(version) FROM schema_migrations`. If < expected, run migration functions sequentially.
- Each migration is a named function: `migrate_v1_to_v2(sqlite3 *db)`, `migrate_v2_to_v3(sqlite3 *db)`.
- Migrations are **tested**: the unit test suite includes a test that creates a v1 database, runs migrations to current, and verifies data integrity.
- **Never strand a user**: if a migration fails, the original file is untouched (we migrate into a copy, then rename on success).

With SQLite this is natural: `schema_migrations` table, transactions around each migration. With JSON it would mean writing a `"version": N` field and hand-coding transformation logic — same principle, more error-prone code.

### 4. Responsiveness Under Load

The main loop changes from blocking `getch()` to a **non-blocking poll loop with dirty-flag redraw**:

```c
int tui_run(Board *board, const char *save_path) {
    tui_init();
    tui_draw(board);

    timeout(100);           /* getch() returns ERR after 100ms */
    int running  = 1;
    int dirty    = 0;

    while (running) {
        int ch = getch();

        if (ch != ERR) {
            running = handle_input(board, ch);
            dirty   = 1;
        }

        int jobs_updated = llm_poll();
        if (jobs_updated > 0) {
            /* results were applied to board inside llm_poll */
            dirty = 1;
        }

        if (dirty) {
            tui_draw(board);
            dirty = 0;
        }
    }

    tui_shutdown();
    board_save(board, save_path);
    return 0;
}
```

**Key properties:**
- `timeout(100)` means the TUI is **always responsive within 100ms** — keypress latency is imperceptible.
- `llm_poll()` does non-blocking `read()` on pipe fds (max 3 simultaneous jobs). Each read is a few KB at most. Even on a slow machine, the poll path completes in microseconds.
- The dirty flag prevents continuous redrawing when idle (no CPU spin).
- `KEY_RESIZE` is handled inside `handle_input()` → sets dirty → redraw at new size.

**LLM result application**: When `llm_poll()` detects a job completed, it parses the JSON result and calls **the same board.c API** as the TUI (`board_edit_card_title`, a new `board_set_card_description()`, `board_add_label()`, etc.). These are simple in-memory operations that then get autosaved. No special path, no concurrent mutation.

### 5. Milestone Slicing

Each milestone produces a **verifiably green** state: unit tests + BDD suite pass on the Linux box. Tracer-bullet style — thinnest end-to-end slice first.

| Milestone | Scope | Tracer-Bullet Verification |
|-----------|-------|---------------------------|
| **M1: SQLite storage** | Vendor sqlite3 amalgamation. Write `src/db.c/.h`. Port board.c persistence to SQLite with identical in-memory model. JSON→SQLite migration on first load. All 54 unit tests pass. TUI works identically. BDD suite confirms no regression. | Load old `~/.kanban.json`, quit, verify `~/.kanban/default.db` contains same cards. Load it again — same board. |
| **Status M1: DONE** | 95 unit tests (56 board + 39 db), 36 BDD scenarios green. Build zero warnings. Default path `~/.kanban/default.db`, legacy JSON auto-migrated and preserved. |
| **M2: LLM seam + fake provider** | Write `src/llm.c/.h` with fork+exec+poll. Fake provider (immediate echo). Non-blocking main loop with dirty flag. Unit tests for job lifecycle. TUI shows job status in status bar (no visual card change yet). | Press a test keybinding → fake job submitted → status bar shows "Working..." → 1s later shows "Done". TUI never freezes. |
| **Status M2: DONE** | 155 unit tests (56 board + 39 db + 60 llm), 39 BDD scenarios (213 steps). Build zero warnings. Fake provider via KANBAN_LLM_PROVIDER=fake env var. Debug key 'T' exercises lifecycle. Non-blocking loop with timeout(100). |
 | **Status M3: DONE** | CLI subcommands (add/list/show/enrich/move). Card model extended (description, labels, timestamps, archived). Enrich prompt builder + result parser + envelope unwrapping (TODO spike for opencode --format json). TUI Ctrl+E enrich with human-in-the-loop review screen (per-field accept/reject). Fake provider emits enrichment JSON. 213 unit tests (56+39+61+57), 54 BDD scenarios (13 features), zero warnings. | `kanban add "fix login" --ai` → proposed description + labels → `kanban show <id>` shows all metadata. Ctrl+E in TUI → spinner → review screen → confirm applies to card. |
| **M4: Card detail view + labels + filter** | Detail screen (Enter on card). Label picker. `/` fuzzy filter. Labels rendered as coloured tags on cards. Editable description in detail view. | Open a card, see all fields. Add labels. Filter by label. All BDD scenarios pass. |
| **Status M4: DONE** | 259 unit tests (85 board + 56 db + 61 llm + 57 enrich), 62 BDD scenarios (16 features), zero warnings. Card detail view (Enter, ESC/q, t edit title, D edit desc, l label picker), fuzzy filter (/, real-time subsequence match, shown/total counters, Enter commits, ESC clears), label picker (# board view, l detail view, toggle + new). Labels rendered as colored [tag] on cards (hash→palette). Build zero warnings. | Enter on card → full detail screen with scrollable description, labels, timestamps. / → filter narrows. # → label picker. All 62 BDD scenarios green. |
| **M5: Adaptive layout + undo + archive** | Single-column narrow mode. Tab between columns at <60 cols. Undo ring buffer (20 ops). Soft archive (`x` key) + visibility toggle. | Resize terminal to 40×24 → single column with tabs visible. Delete card → undo → card restored. Archive card → toggle visibility. |
| **M6: Background jobs polish + AI Q&A + multiple boards** | Real LLM jobs with spinner indicators on cards. Q&A on card detail. `-b` flag, `.kanban/` directory, `list-boards`. Final BDD coverage for all iteration 3 features. | Submit an enrich job from TUI → spinner appears on card → result arrives → card updates live. `kanban -b project2` opens separate board. Q&A pair saved and viewable. |

---

## Appendix: Files Changed (projected)

| File | Change |
|------|--------|
| `vendor/sqlite3.c`, `vendor/sqlite3.h` | New: SQLite amalgamation |
| `src/db.c`, `src/db.h` | New: SQLite persistence layer |
| `src/board.h` | Extend `Card` struct (description, labels, timestamps, archived) |
| `src/board.c` | Wire load/save to db.c; new `board_set_card_description()`, `board_add_label()`, etc. |
| `src/llm.c`, `src/llm.h` | New: LLM job subsystem |
| `src/tui.c`, `src/tui.h` | Non-blocking main loop, dirty flag, detail screen, filter bar, job indicators, adaptive layout |
| `src/main.c` | Subcommand dispatch, `-b` flag, ESCDELAY unchanged |
| `Makefile` | Add sqlite3.c + llm.c to build; `-lpthread` if needed (sqlite3 needs it on some platforms) |
| `tests/unit/test_board.c` | Extend with detail/label/archive asserts |
| `tests/unit/test_db.c` | New: SQLite migration + CRUD tests |
| `tests/unit/test_llm.c` | New: job lifecycle tests with fake provider |
| `tests/bdd/` | New feature files for CLI, enrich, detail, filter, adaptive layout, undo, archive |

No existing public API signature changes — only additions.
