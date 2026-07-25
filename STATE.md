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
- [ ] M1: skeleton + model + cJSON + unit tests
- [ ] M2: ncurses TUI render + navigation
- [ ] M3: CRUD + move + autosave
- [ ] M4: e2e + polish + README
- [ ] Hand off macOS build instructions

## Git log so far
- (empty repo)

## Notes for minions
- All builds/tests/e2e MUST run on the box, not locally.
- Commit locally only after box verification is green; amend on fixes.
- Update this STATE.md (progress + git log) after each milestone.
