# Box Usage Report — Kanban CLI Iteration

**Box ID:** `bx_8rz2mckf`
**Date:** 2026-07-25
**Author:** Renan Liberato (for project owner)

---

## 1. TL;DR

Box (ascii.dev) was the exclusive build, test, and e2e execution environment for the
kanban-cli project. The local macOS machine was used only for git, file editing, and
running the `box` CLI itself. One box served all four milestones across 6 commits.
All verification gates were green on box before any code was committed locally.

**Box consumption:** 1,833 seconds (~31 wall-clock minutes) out of a 2,000,000-second
trial — **0.09% of the trial quota**.

---

## 2. What Box Is

Box is a CLI-driven service that provisions disposable Linux sandbox machines on
demand. Each box gets an ephemeral Ubuntu VM with root access, a public IP, and
per-second billing against a usage quota. You interact with it entirely from the
terminal: `box new`, `box ssh`, `box scp`, `box stop`, `box resume`, etc. The trial
plan includes 2,000,000 seconds of compute, 4 concurrent active boxes, and 20
creation requests per day.

For this project, box acted as a remote CI-like build server — fast to provision, fast
to sync code to, and zero local environment pollution.

---

## 3. Setup

### Installation

```sh
curl -fsSL https://ascii.dev/install.sh | sh
```

This installs `box` to `~/.ascii/bin/box`. **The installer does not add this directory
to PATH.** You must manually export it in every shell (or add to `.zshrc` / `.bashrc`):

```sh
export PATH="$HOME/.ascii/bin:$PATH"
```

### Authentication

On first `box new`, the CLI prompted for GitHub OAuth login. The session token was
stored and persisted across restarts. No re-authentication needed during this
iteration.

### Box Creation

```sh
box new --no-auto-stop
```

The `--no-auto-stop` flag was chosen because the workflow involved many sequential
sync-and-verify cycles across multiple hours. Without it, an idle box auto-stops after
15 minutes, which would have interrupted the iterative loop.

The created box:

| Property          | Value                                      |
|-------------------|--------------------------------------------|
| Box ID            | `bx_8rz2mckf`                              |
| Name              | Box 2026-07-25 19:52                       |
| OS                | Ubuntu 24.04.4 LTS (Noble Numbat)          |
| Kernel            | 6.8.0-117-generic x86_64                   |
| CPUs              | 4                                          |
| RAM               | 7.6 GiB                                    |
| GCC               | 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1)   |
| IP                | Dynamic (allocated on resume)              |

### Toolchain Provisioning (one-time)

```sh
box ssh bx_8rz2mckf "sudo apt update && sudo apt install -y build-essential libncurses-dev python3 python3-pip"
box ssh bx_8rz2mckf "pip3 install pexpect"
```

Packages installed:
- `build-essential` — gcc, make
- `libncurses-dev` — ncurses headers and library
- `python3` + `pip3` + `pexpect` — e2e test driver

---

## 4. The Working Loop

The core discipline was: **edit locally → sync to box → verify → commit only when
green.** This was enforced for every commit.

### Step-by-step

**1. Edit files locally (macOS).** All source edits happened in the local repository at
`/Users/renanliberato/projects/kanban-cli`.

**2. Sync source tree to box via tar-over-SSH:**

```sh
tar czf - --exclude=.git . | \
  box ssh bx_8rz2mckf "mkdir -p ~/kanban-cli && tar xzf - -C ~/kanban-cli"
```

This pipes a gzipped tarball of the working tree (excluding `.git/`) over SSH directly
into the box's filesystem. It completes in under a second for a ~100 KB source tree.

**Why tar-over-SSH instead of alternatives:**

| Method                 | Issue                                                 |
|------------------------|-------------------------------------------------------|
| `git push/pull`        | Would require key setup on box; adds commit noise     |
| `box scp`              | One file per invocation; no directory sync            |
| `rsync`               | Not installed on box; tar pipe is close enough        |
| tarball via ssh stdin  | Single command, fast, only sends what's changed       |

The exclude of `.git/` keeps the transferred payload minimal. The box does not need git
history — it just needs the working tree for build and test.

**3. Run verification on box:**

```sh
box ssh bx_8rz2mckf "cd ~/kanban-cli && bash scripts/verify.sh"
```

`scripts/verify.sh` runs, in order:
1. `make clean` — wipe previous build artifacts
2. `make` — compile `bin/kanban` with `-Wall -Wextra -std=c99` (zero warnings)
3. `make test` — build and run 54 unit test assertions against the board model
4. `python3 tests/e2e.py ./bin/kanban` — 13 pexpect scenarios driving the binary in a
   pseudo-terminal

All steps must pass. Any failure triggers a fix locally followed by re-sync and
re-verify. No commit is made until the box output shows `=== verify: all green ===`.

**4. Commit locally (macOS).** Only after step 3 passes:

```sh
git add -A && git commit -m "..."
```

---

## 5. What Ran Where

| Task                        | Location | Tooling                     |
|-----------------------------|----------|-----------------------------|
| File editing                | local    | editor (macOS)              |
| git commits, log, diff      | local    | git on macOS                |
| Source sync to box          | local→box| `tar` pipe over `box ssh`   |
| Compilation (`gcc`, `make`) | box      | gcc 13.3.0, GNU make        |
| Unit tests (54 asserts)     | box      | custom assert harness in C  |
| e2e tests (13 scenarios)    | box      | Python 3 + pexpect          |
| macOS binary builds         | local    | `make` with system ncurses  |

**Key decision:** No cross-compilation (no osxcross). The box verifies Linux
correctness and portability. The final macOS binary is built by the user on their own
machine via `make` — the project is pure C99 with no platform-specific code, so the
Linux verification is sufficient to catch regressions.

---

## 6. Per-Milestone Usage

| Milestone | Commits | What Was Verified on Box                                  |
|-----------|---------|-----------------------------------------------------------|
| M1        | 2       | Makefile skeleton, vendored cJSON compiles, board model JSON save/load round-trips, 54 unit assertions pass |
| M2        | 2       | ncurses links cleanly, TUI renders 3 columns with colors, arrow/hjkl navigation, selection highlight works in a pty via pexpect smoke test |
| M3        | 1       | Card add/edit/delete operations, move between columns, autosave on every mutation, CRUD smoke test in pexpect |
| M4        | 1       | Full 13-scenario e2e suite (add/move/delete/edit/navigation/edge cases), card count display, truncation with ellipsis, `ESCDELAY` fix, README with Linux + macOS instructions |

Every milestone's verification on box was green before the commit was created locally.

**Total:** 6 commits, 4 milestones, 1 box, 0 red-commits.

---

## 7. Cost & Limits Observed

### Trial Plan (at iteration time)

| Resource             | Quota                   |
|----------------------|-------------------------|
| Compute seconds      | 2,000,000               |
| Concurrent boxes     | 4                       |
| Creations per day    | 20                      |
| Creations per minute | 5                       |
| Trial period         | 7 days (ends 2026-08-01T19:38:57Z) |

### Actual Consumption

| Metric                       | Value                          |
|------------------------------|--------------------------------|
| Seconds used by this project | **1,833** (creditUsedSeconds)  |
| Additional (report probes)   | ~60 (box resume + spec checks) |
| Total on account             | 1,893 (at report time)         |
| Remaining                   | 1,998,107                      |
| % of trial consumed          | **0.09%**                      |

At this rate, the trial would support ~1,090 equivalent project iterations.

### Billing Observations

- Box bills only for running time. When the box is stopped, no seconds accrue.
(creditUsedSeconds is the authoritative counter.)
- The `--no-auto-stop` box consumed seconds continuously between creation and when it
  was manually stopped after M4. The initial M1–M4 run was dense enough that this was
  acceptable. For looser workflows where work is spread across days, stopping between
  sessions (`box stop` / `box resume`) would save seconds.
- There is no charge for storage or snapshots during trial.

---

## 8. What Worked Well

| Aspect                     | Notes                                                          |
|----------------------------|----------------------------------------------------------------|
| Non-interactive `box ssh`  | Running `box ssh <id> "<cmd>"` worked exactly like a CI gate — output to stdout, exit code propagated, no TTY required |
| JSON output by default     | When piped (`box info --json | jq .`), box commands emit JSON automatically — no flag needed |
| Snapshot / stop / resume   | After M4, the box was stopped and snapshotted. The snapshot preserves the full provisioning + built binary + test results on disk |
| Fast toolchain provisioning| `apt install` + `pip install` took under 30 seconds; all needed packages available from standard repos |
| Build reliability          | `gcc -Wall -Wextra` produced zero warnings across all milestones; the Linux environment was 100% reproducible |
| Clean separation           | Local = editing + git; remote = build + test. No stale artifacts, no "works on my machine" ambiguity |

## Friction & Limitations

| Issue                                           | Workaround / Mitigation                                                |
|-------------------------------------------------|------------------------------------------------------------------------|
| `box` not on PATH after install                 | Must manually `export PATH="$HOME/.ascii/bin:$PATH"` in every shell. Documented in STATE.md for minions. |
| No macOS boxes (Linux only)                     | Architectural decision: write portable C99, verify on Linux box, let user run `make` on macOS for the final binary. No cross-compilation needed. |
| No `rsync` on box; `scp` is file-at-a-time     | tar-over-SSH pipe instead. Works well for small trees but lacks delta transfer; for larger repos this would waste bandwidth. |
| `box ssh` adds a slight invocation latency      | ~1-2 seconds per command. Not a problem for verify.sh (under 5s total), but interactive debugging over SSH feels a hair laggy. |
| `box stop` / `box resume` takes 15-30 seconds   | The snapshot-and-restore cycle is not instant. Planning around it helps. |
| Agent-chat events pollute `box events` output   | The events stream mixes lifecycle events with agent chat. For a pure operations view, filtering is necessary. In practice, `box info` and `box limits` gave us everything we needed. |

---

## 9. Recommendations for Next Iterations

1. **Reuse `bx_8rz2mckf`.** The box is provisioned and snapshotted. Resume it
   (`box resume bx_8rz2mckf`) instead of creating a new box. This avoids
   re-provisioning the toolchain and preserves the snapshot as a known-good checkpoint.

2. **Stop between sessions.** If work spans multiple days, run `box stop` at the end of
   each session to pause billing. Resume picks up exactly where you left off. At
   ~31 minutes per dense iteration, the trial is generous, but this is good hygiene.

3. **Snapshot before risky refactors.** Before a large rename, restructuring, or
   dependency change, take a manual snapshot (or just stop — `box stop` auto-snapshots).
   If the refactor goes wrong, `box fork` from the snapshot to start fresh without
   losing the provisioning work.

4. **Consider `box prompt` for autonomous loops.** The `box prompt` subcommand sends a
   prompt to an AI agent (Codex or Claude) running inside the box. For future
   iterations with fully autonomous "sync → build → test → report" agent loops, this
   could replace the manual orchestration. The agent would have direct access to the
   box filesystem and could run verify.sh itself.

5. **`box host` for a web UI preview.** If the kanban project ever grows a web
   interface (e.g., a live board served over HTTP), `box host` exposes a port on a
   stable HTTPS URL. This would let you demo or test the web UI without deploying
   anywhere.

6. **Watch the creation rate limit (20/day).** This was never hit during this iteration
   (only 1 box created), but if a future workflow involves creating many ephemeral
   boxes (e.g., parallel matrix testing), plan around the daily cap.

7. **Keep the PATH quirk in STATE.md.** Every minion or collaborator who opens a new
   shell needs to know to export `~/.ascii/bin`. The current STATE.md already documents
   this — don't remove that line.

---

## Appendix: Key Commands Reference Card

```sh
# Ensure box is on PATH
export PATH="$HOME/.ascii/bin:$PATH"

# Create a box for long-running work
box new --no-auto-stop

# Check account usage
box limits | jq '{used: .creditUsedSeconds, remaining: .subscriptionRemainingSeconds}'

# Sync working tree to box
tar czf - --exclude=.git . | \
  box ssh bx_8rz2mckf "mkdir -p ~/kanban-cli && tar xzf - -C ~/kanban-cli"

# Run the full verification pipeline
box ssh bx_8rz2mckf "cd ~/kanban-cli && bash scripts/verify.sh"

# Stop to snapshot + pause billing
box stop bx_8rz2mckf

# Resume next session
box resume bx_8rz2mckf

# Check box status
box info bx_8rz2mckf | jq '{state: .box.state, snapshot: .box.snapshotAvailable}'
```

---

*Report generated from live box API data on 2026-07-25. Box ID: bx_8rz2mckf.
1,833 seconds consumed of 2,000,000 trial quota (0.09%).*
