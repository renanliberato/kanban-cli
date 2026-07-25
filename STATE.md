# Kanban CLI — Orchestration State

Read this first after any context compaction. PLAN.md has the full plan.

## Environment
- Local repo: /Users/renanliberato/projects/kanban-cli (git, source of truth)
- box CLI: /Users/renanliberato/.ascii/bin/box (not on default PATH —
  prefix commands with `export PATH="$HOME/.ascii/bin:$PATH"`)
- box account: authenticated, Trial plan (4 active boxes max, plenty of seconds)
- Box machines are Linux (Ubuntu-ish). We build/test there; macOS build is
  done by the user locally at the end.

## Box
- Box ID: bx_8rz2mckf
- Sync command (suggested):
  `tar czf - --exclude=.git . | box ssh <ID> "mkdir -p ~/kanban-cli && tar xzf - -C ~/kanban-cli"`
- Verify command: `box ssh <ID> "cd ~/kanban-cli && bash scripts/verify.sh"`

## Progress
- [x] box CLI installed + authenticated
- [x] PLAN.md / STATE.md written
- [x] Box provisioned (gcc, make, libncurses-dev, python3+pexpect)
- [x] M1: skeleton + model + cJSON + unit tests
- [x] M2: ncurses TUI render + navigation
- [x] M3: CRUD + move + autosave
- [x] M4: e2e + polish + README
- [x] Hand off macOS build instructions (included in README.md)

## Git log so far
- `2abaaf1` M1: project skeleton, board model, JSON persistence, unit tests (11 files, 4261+ lines)
- `8013042` Update STATE.md: M1 checked off, box provisioned
- `ee9da35` M2: ncurses TUI with board rendering, navigation, colors (6 files, 545+ lines)
- `edc6b21` M3: card add/edit/delete, move between columns, autosave (6 files, 463+ lines)
- `804f7bb` M4: e2e suite, card counters, truncation ellipses, README, macOS compat (7 files, +1034/-287 lines)

## Notes for minions
- All builds/tests/e2e MUST run on the box, not locally.
- Commit locally only after box verification is green; amend on fixes.
- Update this STATE.md (progress + git log) after each milestone.
