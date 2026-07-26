# Kanban CLI — Orchestration State

Read this first after any context compaction. PLAN.md has the full plan.

## Project Status: Iterations 1–3 COMPLETE

All 3 iterations delivered and verified. The project is feature-complete.

- **Unit tests:** 386 assertions (105 board, 88 db, 81 llm, 28 undo, 22 board_path, 62 agent)
- **BDD suite:** 75 scenarios (+4 from iter4b), 19 features, 449 steps (behave/gherkin/pexpect)
- **Build:** `-Wall -Wextra -std=c99`, zero warnings on gcc 13.3
- **Reports:** `BOX_REPORT.md` (box usage), `BDD_REPORT.md` (BDD testing methodology)
- **Verification:** `bash scripts/verify.sh` — clean build + unit tests + BDD
- **Shipped agents:** 8 planning agents in `agents/` (6 comment, 2 description) with README docs

## Agents

8 shipped planning agents (`agents/*.md`), installed with `cp agents/*.md ~/.kanban/agents/`:

| Agent | Type | Role |
|-------|------|------|
| `analyst` | comment | Analytics/tracking coverage review |
| `pm` | comment | Product lens — challenges weak framing |
| `planner` | comment | Ordered implementation plan with dependencies |
| `risk` | comment | Risk assessment — failure modes, edge cases, LxI ratings |
| `scope` | comment | MVP cut — essential vs. deferrable |
| `ux` | comment | UX review — end-to-end flow, missing states, accessibility |
| `writer` | description | Full spec rewrite incorporating thread decisions |
| `groom` | description | Conservative update — folds thread conclusions into description |

## Environment

- Local repo: /Users/renanliberato/projects/kanban-cli (git, source of truth)
- box CLI: /Users/renanliberato/.ascii/bin/box (not on default PATH —
  prefix commands with `export PATH="$HOME/.ascii/bin:$PATH"`)
- box account: authenticated, Trial plan

## Box (RUNNING as of 2026-07-26, iter4b verification; STOP after commit)

- Box ID: bx_8rz2mckf
- Resume with: `box resume bx_8rz2mckf`
- Stop with: `box stop bx_8rz2mckf`
- Sync command (suggested):
  `tar czf - --exclude=.git . | box ssh <ID> "mkdir -p ~/kanban-cli && tar xzf - -C ~/kanban-cli"`
- Verify command: `box ssh <ID> "cd ~/kanban-cli && bash scripts/verify.sh"`

## Progress

- [x] box CLI installed + authenticated
- [x] PLAN.md / STATE.md written
- [x] Box provisioned (gcc, make, libncurses-dev, python3+pexpect, python3-behave)
- [x] M1: skeleton + model + cJSON + unit tests
- [x] M2: ncurses TUI render + navigation
- [x] M3: CRUD + move + autosave
- [x] M4: e2e + polish + README
- [x] Hand off macOS build instructions (included in README.md)
- [x] Iteration 2: BDD test layer (behave/gherkin, 36 scenarios) replacing e2e.py
- [x] Iteration 3 M1: SQLite storage with JSON auto-migration
- [x] Iteration 3 M2: LLM job subsystem with fork/poll provider seam
- [x] Iteration 3 M3: Non-interactive CLI subcommands + AI enrich flow (human-in-the-loop review)
- [x] Iteration 3 M4: Card detail view, label picker, fuzzy filter
- [x] Iteration 3 M5: Adaptive layout + undo + archive
- [x] Iteration 3 M6: Background jobs polish + multiple boards
- [x] Iteration 3 M7a: Task comments + remove enrich review screen
- [x] Iteration 3 M7b: Agent config files + @mention triggers
- [x] ITERATIONS 1–3 COMPLETE — all milestones DONE
- [x] Iter4a: removed broken enrich feature (Ctrl+E, CLI enrich, enrich module)
- [x] BOX_REPORT.md written
- [x] BDD_REPORT.md written
- [x] Box stopped (bx_8rz2mckf)
- [x] Iter4b: shipped 8 planning agents (6 comment, 2 description), README agents section, shipped-agent BDD scenarios, multi-line wrap_text fix

## Git Log (Complete)

- `2abaaf1` M1: project skeleton, board model, JSON persistence, unit tests
- `8013042` Update STATE.md: M1 checked off, box provisioned
- `ee9da35` M2: ncurses TUI with board rendering, navigation, colors
- `edc6b21` M3: card add/edit/delete, move between columns, autosave
- `804f7bb` M4: e2e suite, card counters, truncation ellipses, README, macOS compat
- `915f6c3` Iteration 2: BDD test suite (behave/gherkin, 36 scenarios, 200 steps, replaces e2e.py)
- `6a0135e` Add iteration 3 design doc and box usage report
- `cad21ae` Iter3 M1: SQLite storage with JSON auto-migration
- `9a620e0` Iter3 M2: LLM job subsystem with fork/poll provider seam
- `5de7547` Iter3 M3: CLI subcommands + AI enrich with human review
- `2f58d86` Iter3 M4: card detail view, label picker, fuzzy filter
- `2eecd9e` Iter3 M5: adaptive narrow layout, undo, archive
- `dbdf63c` Iter3 M6: background job polish + multiple named boards
- `e90b7fb` Iter3 M7a: task comments, schema v2, remove enrich review screen
- `31e6c85` Iter3 M7b: agent primitives — markdown-defined agents triggered via @mentions in comments
- `8fe18e3` Add BDD testing report
- `9bf71aa` Iter4a: remove broken enrich feature
- `bb51740` Iter4b: ship 8 planning agents (comment + description), README agents section, shipped-agent BDD

## Notes for minions

- All builds/tests/e2e MUST run on the box, not locally (box must be resumed first).
- Commit locally only after box verification is green; amend on fixes.
