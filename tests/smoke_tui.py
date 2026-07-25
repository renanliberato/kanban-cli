#!/usr/bin/env python3
"""
Smoke test for the ncurses TUI.

This is a throwaway smoke test (not the final e2e suite — that is M4).
It spawns the kanban binary in a pty with a seeded board file, verifies
the process stays alive during navigation, and exits cleanly on 'q'.
"""

import json
import os
import sys
import tempfile
import time

import pexpect


def seed_board(path):
    """Write a board JSON file with a few cards across columns."""
    board = {
        "next_id": 5,
        "columns": [
            {
                "name": "To Do",
                "cards": [
                    {"id": 1, "title": "buy groceries"},
                    {"id": 2, "title": "walk the dog"},
                ],
            },
            {
                "name": "Doing",
                "cards": [
                    {"id": 3, "title": "write TUI"},
                ],
            },
            {
                "name": "Done",
                "cards": [
                    {"id": 4, "title": "setup project"},
                ],
            },
        ],
    }
    with open(path, "w") as f:
        json.dump(board, f, indent=2)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./bin/kanban"

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", delete=False
    ) as tf:
        board_path = tf.name

    try:
        seed_board(board_path)

        # Spawn kanban.  Set TERM so ncurses can initialise.
        env = os.environ.copy()
        env.setdefault("TERM", "xterm-256color")

        child = pexpect.spawn(
            binary,
            args=[board_path],
            env=env,
            encoding="utf-8",
            timeout=10,
            dimensions=(30, 90),   # rows, cols — ample for the TUI
        )

        # Give ncurses a moment to initialise and draw.
        # We cannot expect text because ncurses uses escape sequences,
        # but we can check the child is still alive.
        time.sleep(0.3)
        assert child.isalive(), "kanban process died during startup"

        # Send some navigation keys (hjkl)
        child.send("j")   # move down in To Do column
        time.sleep(0.05)
        child.send("l")   # move right to Doing
        time.sleep(0.05)
        child.send("l")   # move right to Done
        time.sleep(0.05)
        child.send("h")   # move left to Doing
        time.sleep(0.05)
        child.send("h")   # move left to To Do
        time.sleep(0.05)
        child.send("k")   # move up

        # Verify still alive after navigation
        time.sleep(0.1)
        assert child.isalive(), "kanban process died during navigation"

        # Quit
        child.send("q")
        child.expect(pexpect.EOF, timeout=3)

        child.close()
        assert child.exitstatus == 0, (
            f"Expected exit code 0, got {child.exitstatus}"
        )
        print("PASS: smoke_tui — kanban launched, navigated, quit cleanly")
        return 0

    finally:
        os.unlink(board_path)


if __name__ == "__main__":
    sys.exit(main())
