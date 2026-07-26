# BDD Testing Report — Kanban CLI

**Date:** 2026-07-25
**Author:** Renan Liberato (for project owner)

---

## 1. TL;DR

Every user-facing operation in kanban-cli is specified in plain English via Gherkin
`.feature` files, executed against the **real ncurses binary** in a pseudo-terminal,
and gated on every commit. Across 7 milestones (Iteration 2 through Iteration 3 M7b),
the suite grew from 36 scenarios (10 features) to **79 scenarios (21 features, 441 steps)**.
Zero regressions shipped to `main`.

BDD caught timing bugs, keybinding conflicts, boundary conditions on column edges, and
data-loss paths that unit tests alone could not — because BDD exercises the full
vertical slice: user keypress → pty → ncurses render → SQLite write. It is the
project's safety net, not just documentation.

**What BDD bought this project:**

- Every operation (`add`, `edit`, `delete`, `move`, `enrich`, `comment`, `archive`,
  `undo`, `filter`) is verifiably described and tested.
- AI enrichment flows (Ctrl+E, CLI `--ai`, agent `@mentions`) are deterministic via a
  fake LLM provider — no live `opencode` needed in CI.
- The suite survived two major implementation changes — JSON→SQLite (M1) and review
  screen removal (M7a) — without a single `.feature` file regressing, proving the
  scenarios test *what* the app does, not *how* it stores data or renders dialogs.
- The gate is `scripts/verify.sh` — zero-warning build + 443 unit asserts + 79
  behave scenarios. All three must pass before any commit.

---

## 2. The Stack

| Layer                | What                                                   | Why this, not that                                              |
|----------------------|--------------------------------------------------------|-----------------------------------------------------------------|
| **behave**           | `python3 -m behave tests/bdd`                          | Already using Python (pexpect, sqlite3). No new language runtime. |
| **Gherkin**          | `.feature` files in `tests/bdd/features/`              | Standard BDD syntax (Given/When/Then). Readable by non-coders.  |
| **pexpect**          | `pexpect.spawn` with pty, 80×24 or 120×40 dimensions   | Drives the real ncurses binary through a terminal.              |
| **sqlite3** (Python) | `environment.py → read_board()` queries `.db` directly  | Assert on-persistence state without screen-scraping.            |
| **Fake LLM provider**| `KANBAN_LLM_PROVIDER=fake` + `KANBAN_LLM_FAKE_DELAY`   | Deterministic AI flows. No network, no opencode CLI needed.     |

**Why behave and not cucumber-ruby or godog?**

- The project test tooling was already Python (pexpect for pty control, sqlite3 for DB
  assertions). Adding Ruby or Go would introduce a second language runtime with its own
  package manager, dependency lock, and CI provisioning burden.
- `python3-behave` installs from `apt` (Ubuntu) or `pip`. It shares the same Python
  environment as pexpect, so there's zero environment overhead.

---

## 3. Anatomy of One Scenario (End-to-End)

### The Feature File

```gherkin
# tests/bdd/features/add_card.feature:6
Scenario: Add a card by typing title and pressing Enter
  Given a board with no cards
  When I launch the application
  When I add a card "buy milk" to "To Do"
  Then the "To Do" column should contain "buy milk"
```

### The Step Definitions

The `Given` step seeds an empty board as JSON into a temp directory:

```python
# tests/bdd/steps/kanban_steps.py:56
@given("a board with no cards")
def step_given_board_no_cards(context):
    seed_board(context, empty_board())
```

The `When I launch` step spawns the binary in a pty via pexpect:

```python
# tests/bdd/environment.py:29
def spawn_kanban(context, board_path=None, dimensions=(30, 90)):
    child = pexpect.spawn(
        _binary_path(context),
        args=[board_path],
        env=env,            # KANBAN_LLM_PROVIDER=fake set here
        encoding="utf-8",
        codec_errors="replace",
        timeout=8,
        dimensions=dimensions,
    )
    time.sleep(0.3)         # let ncurses initialise
    return child
```

The `When I add a card` step navigates to the column, presses `a`, types the title,
presses `Enter`, waits for the TUI to process, then reads the board from SQLite to
update its internal selection tracker:

```python
# tests/bdd/steps/kanban_steps.py:270
@when('I add a card "{title}" to "{column}"')
def step_when_add_card_to_column(context, title, column):
    _navigate_to_column(context, column)
    _send_key(context, "a")
    time.sleep(0.1)
    context.child.send(title)
    time.sleep(0.05)
    _send_key(context, "Enter")
    time.sleep(0.15)
    # Track selection: new card is last in column
    board = read_board(context)
    col_idx = {"To Do": 0, "Doing": 1, "Done": 2}[column]
    context.sel_col = col_idx
    context.sel_row = len(board["columns"][col_idx]["cards"]) - 1
```

The `Then` step reads the board from the SQLite database (which the app wrote) and
asserts the card exists:

```python
# tests/bdd/steps/kanban_steps.py:585
@then('the "{column}" column should contain "{title}"')
def step_then_column_contains(context, column, title):
    board = read_board(context)           # reads from .db if it exists
    cards = col_cards(board, column)
    assert title in cards
```

### What Just Happened

1. A temp JSON board is written to `/tmp/kanban_bdd_XXXXXX/board.json`.
2. `./bin/kanban /tmp/.../board.json` starts in a new pty at 30×90.
3. ncurses initialises, reads `KANBAN_LLM_PROVIDER=fake` from the environment.
4. The binary reads board.json, migrates it to SQLite (board.db), renders the TUI.
5. The step logic sends `a`, `b u y   m i l k`, `Enter` keycodes via pexpect.
6. The TUI's input handler creates a new card, ncurses re-renders the board, and
   `board_save()` writes the new card to SQLite (autosave).
7. The `Then` step opens `board.db` with Python's `sqlite3`, queries `cards` JOIN
   `columns`, and asserts `"buy milk"` exists in the `"To Do"` column.
8. The `after_scenario` hook tears down the pty and deletes the temp directory.

Every other scenario follows this same pattern: Given (seed state) → When (drive pty
with keycodes) → Then (assert screen content + SQLite state).

---

## 4. Structure & Conventions

### Directory Layout

```
tests/bdd/
  environment.py              — pexpect spawn helpers, board I/O, behave hooks
  features/
    adaptive_layout.feature   (4 scenarios)
    add_card.feature          (5 scenarios)
    agents.feature            (3 scenarios)
    archive.feature           (4 scenarios)
    autosave.feature          (4 scenarios)
    cli.feature              (11 scenarios)
    comments.feature          (3 scenarios)
    delete_card.feature       (3 scenarios)
    detail_view.feature       (3 scenarios)
    edit_card.feature         (2 scenarios)
    enrich.feature            (3 scenarios)
    filter.feature            (3 scenarios)
    labels.feature            (2 scenarios)
    llm_jobs.feature          (3 scenarios)
    move_card.feature         (7 scenarios)
    navigation.feature        (4 scenarios)
    persistence.feature       (2 scenarios)
    quit.feature              (4 scenarios)
    responsiveness.feature    (1 Scenario Outline, 2 examples)
    startup.feature           (3 scenarios)
    undo.feature              (4 scenarios)
  steps/
    __init__.py
    kanban_steps.py           — 81 step definitions
```

### Feature → Coverage Mapping

| Feature File              | Scenarios | What It Covers                                             |
|---------------------------|-----------|------------------------------------------------------------|
| `startup.feature`         | 3         | Board rendering, column headers, card counters              |
| `navigation.feature`      | 4         | hjkl + arrow keys, selection tracking, boundary clamping    |
| `add_card.feature`        | 5         | Adding cards, ESC cancel, empty-title cancel, all columns   |
| `edit_card.feature`       | 2         | In-place title edit, ESC cancel preserves original          |
| `delete_card.feature`     | 3         | Confirm with `y`, cancel with `n`/other key                 |
| `move_card.feature`       | 7         | `H`/`L` + `<`/`>` to shift columns, boundary no-ops        |
| `autosave.feature`        | 4         | Immediate disk write on add/edit/delete/move                |
| `persistence.feature`     | 2         | Quit + restart preserves board state                        |
| `quit.feature`            | 4         | Exit code 0, persistence after quit                         |
| `responsiveness.feature`  | 2         | Terminal size compatibility (80×24, 120×40)                 |
| `cli.feature`             | 11        | CLI add/list/show/enrich/move, errors, AI enrichment          |
| `enrich.feature`          | 3         | TUI Ctrl+E enrichment, direct-apply flow                    |
| `llm_jobs.feature`        | 3         | Async job lifecycle: submit, responsiveness, completion      |
| `detail_view.feature`     | 3         | Card detail screen, inline title/description editing        |
| `labels.feature`          | 2         | Label picker, label persistence                             |
| `filter.feature`          | 3         | Fuzzy filter, ESC clear, Enter lock                         |
| `adaptive_layout.feature` | 4         | Narrow single-column mode, Tab cycling, resize              |
| `archive.feature`         | 4         | Soft archive, Ctrl+A toggle, CLI list --archived            |
| `undo.feature`            | 4         | Undo delete, move, title edit; hint in status bar           |
| `comments.feature`        | 3         | Comment view/add in detail, empty state message             |
| `agents.feature`          | 3         | @mention triggers, unknown mention error, CLI listing       |

### Conventions

- **environment.py fixtures:** Every scenario gets a fresh `tempfile.mkdtemp()`,
  a unique board path, and a clean `HOME` for isolated agent config discovery. The
  binary is always spawned at a consistent pty size (default 30×90; adaptive-layout
  scenarios use 24×50, responsiveness uses `Scenario Outline` with 24×80 and 40×120).
  The fake LLM provider is set via environment variables in `spawn_kanban()` so no
  scenario ever hits the network.

- **Step-style rules:** Steps are declarative, not procedural. They describe *what* the
  user does (`add a card`, `press Enter`, `move the card right`) and *what* is
  observed (`should contain`, `should show`, `should no longer contain`). Step
  implementations track selection position internally so navigation commands don't
  need to be repeated in every scenario.

- **Screen vs. DB assertions:** Simple text presence checks are against the pty buffer
  (`screen_content()`). State assertions (what cards exist in which column) are done
  against the SQLite database via `read_board()` → Python `sqlite3`. This is faster
  and more reliable than screen-scraping.

- **CLI steps:** Non-TUI commands run via `subprocess.run()` with the same fake LLM
  env. Output is captured as `context.cli_stdout`/`stderr`/`exitcode`. Card IDs are
  tracked globally so downstream steps can reference "the last card".

---

## 5. Evolution — How the Suite Grew

| Milestone                  | Commit      | Scenarios | Features | What changed                                            |
|----------------------------|-------------|-----------|----------|---------------------------------------------------------|
| **Iter 2** (initial BDD)   | `915f6c3`   | 36        | 10       | Replaced `tests/e2e.py` with behave. 200 steps.         |
| **Iter 3 M1** (SQLite)     | `cad21ae`   | 36        | 10       | **Zero scenario changes.** JSON→SQLite migration tested purely at the unit level. BDD confirmed no regression. |
| **Iter 3 M2** (LLM seam)   | `9a620e0`   | 39        | 11       | +`llm_jobs.feature` (3 scenarios): async job lifecycle. |
| **Iter 3 M3** (CLI+enrich) | `5de7547`   | 54        | 13       | +15 scenarios: `cli.feature` (11), `enrich.feature` (3). First AI-driven scenarios. |
| **Iter 3 M4** (detail+labels+filter) | `2f58d86` | 62  | 16       | +8 scenarios: `detail_view`, `labels`, `filter`. |
| **Iter 3 M5** (adaptive+undo+archive) | `2eecd9e` | 74  | 19       | +12 scenarios: `adaptive_layout`, `archive`, `undo`. |
| **Iter 3 M6** (jobs polish+boards) | `dbdf63c` | 74  | 19       | No new scenarios (polish only). |
| **Iter 3 M7a** (comments)  | `e90b7fb`   | 76        | 20       | +`comments.feature` (3). Removed enrich-review-screen scenarios, replaced with direct-apply assertions. |
| **Iter 3 M7b** (agents)    | `31e6c85`   | 79        | 21       | +`agents.feature` (3 scenarios), total steps 441. |

### Key Moments

**JSON → SQLite (M1):** The entire persistence layer was replaced, yet not a single
`.feature` file needed editing. The `read_board()` helper in `environment.py` was
updated to query SQLite instead of parsing JSON, but the Gherkin — the specification of
*what the app should do* — stayed identical. This is the defining proof that BDD
scenarios test behaviour, not implementation.

**Review screen removal (M7a):** M3's AI enrich flow included a human-in-the-loop
review screen (per-field accept/reject). That screen was removed in M7a in favour of
direct-apply + undo. The `enrich.feature` scenarios were rewritten from "review screen
shows proposal" assertions to "card should have a description after job completes"
assertions. The scenarios still validate the high-level behaviour (AI enrichment
works), not the intermediate UI mechanics.

**Adaptive layout (M5):** The `adaptive_layout.feature` scenarios use the
`I launch the application at 24x50` step (custom pty dimensions). This is a
BDD-friendly pattern: the Gherkin states the terminal size as a Given fact, and
pexpect spawns with matching `dimensions=(24, 50)`. The TUI's internal column-width
calculation is never directly tested — only the resulting screen content.

---

## 6. What It Caught / Didn't Catch

### Caught by BDD

| Issue                                                   | How the scenario caught it                                      |
|---------------------------------------------------------|------------------------------------------------------------------|
| Deleted cards not removed from disk until quit          | `autosave.feature` asserts on-disk state while app is running    |
| Empty-title "add" leaking a blank card                  | `add_card.feature:20` asserts "keepme" remains, no blank card    |
| Boundary moves (leftmost→left, rightmost→right) silent  | `move_card.feature:36,40` asserts column contents unchanged      |
| Ctrl+E enrichment freezing the TUI (pre-M2 fix)         | `llm_jobs.feature:10` asserts navigation works during job        |
| Filter not clearing on ESC                              | `filter.feature:9` asserts all cards visible after ESC           |
| Undo not restoring card to original position            | `undo.feature:1` asserts original column content after undo      |
| Codec errors from ncurses escape sequences              | `codec_errors="replace"` in pexpect spawn; caught mojibake bugs  |

### Didn't Catch (Unit Tests Own These)

- **In-memory model correctness** (board.c, db.c): The BDD suite trusts the SQLite
  database reads. If `board_add_card()` corrupts the in-memory model but writes
  correctly to disk, BDD won't notice. The 443 unit tests (`tests/unit/`) assert
  on in-memory state directly.
- **Edge-case SQL logic** (WAL mode, foreign key cascades, migration rollback): Unit
  tests create and mutate databases in isolation.
- **LLM provider contract** (llm.c fork/exec/poll lifecycle): Unit tests with fake
  provider exercise every job state transition.
- **Agent config parsing** (agent.c): BDD has 3 agent scenarios for the @mention
  integration path; the remaining 62 agent unit tests cover config discovery,
  precedence, malformed files, and recursion guards.

### Honest Limitations

- **Screen-content brittleness:** Screen assertions use `assert text in output`.
  If ncurses changes its padding or ANSI escape sequences between terminal versions,
  these assertions can break on text that is functionally still present but
  visually repositioned. Mitigation: assertions are on *semantic* content (card
  titles, column headers), not layout coordinates.
- **Timing waits:** Steps use `time.sleep()` between keypresses (50–300 ms). These
  are tuned for the Linux box's speed. On a slower CI runner, they might need
  adjustment. A more robust approach (pexpect's `expect()` with patterns) was
  avoided because ncurses output is too dynamic for reliable pattern matching.
- **Single-fake-provider testing:** The BDD suite only ever runs against the fake
  LLM provider. Integration testing with a real `opencode` CLI invocation is
  deliberately excluded from CI and left for manual smoke testing.

---

## 7. How to Run & Add a Scenario

### Running the Suite

```sh
# Full verification pipeline (build + unit + BDD):
bash scripts/verify.sh

# BDD only:
python3 -m behave tests/bdd -D binary=./bin/kanban

# Run a single feature:
python3 -m behave tests/bdd/features/add_card.feature -D binary=./bin/kanban

# Run a single scenario by line number:
python3 -m behave tests/bdd/features/add_card.feature:6 -D binary=./bin/kanban

# Dry-run (show scenarios without executing):
python3 -m behave tests/bdd -D binary=./bin/kanban --dry-run
```

### Adding a New Scenario

1. Create or edit a `.feature` file in `tests/bdd/features/`.
2. Write the scenario in Gherkin — use existing step phrases where possible.
3. If a new step phrase is needed, add its Python definition to
   `tests/bdd/steps/kanban_steps.py`. Follow the existing conventions:
   - Use `_send_key(context, "key")` for keypresses.
   - Use `read_board(context)` for SQLite state assertions.
   - Use `screen_content(context.child, context)` for pty buffer assertions.
   - Track selection position in `context.sel_col`/`sel_row` after navigation.
4. Run `python3 -m behave tests/bdd -D binary=./bin/kanban -- <new_file>` to
   verify it passes.
5. Run the full suite to check for regressions.

### Where Things Live

| Path                              | Purpose                                    |
|-----------------------------------|--------------------------------------------|
| `tests/bdd/features/`             | Gherkin `.feature` files                   |
| `tests/bdd/steps/kanban_steps.py` | Python step implementations                |
| `tests/bdd/environment.py`        | Fixtures, helpers, behave hooks            |
| `scripts/verify.sh`               | CI gate (build + unit + BDD)               |
| `tests/unit/`                     | C unit tests (443 assertions)              |

---

## Appendix: Scenario & Step Counts

| Metric                       | Value |
|------------------------------|-------|
| Total `.feature` files       | 21    |
| Total scenarios              | 79    |
| Scenario Outlines (→ 2 examples) | 1  |
| Total behave steps executed  | 441   |
| Python step definitions      | 81    |
| Unit test assertions (C)     | 443   |
| pexpect timeout per scenario | 8 s   |

---

*Report generated from `tests/bdd/` on 2026-07-25. Suite last verified on box
bx_8rz2mckf with all 79 scenarios green and zero build warnings.*
