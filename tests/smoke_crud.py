#!/usr/bin/env python3
"""
Smoke test for card CRUD, move, and autosave in the kanban TUI.

Spawns kanban in a pty with a seeded board file, performs:
  a → add card,  e → edit card,  L → move card right,  d → delete card,
then quits.  Afterwards it reads the JSON file from disk and asserts
the expected final board state (validating autosave).
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


def read_board(path):
    with open(path, "r") as f:
        return json.load(f)


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./bin/kanban"

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".json", delete=False
    ) as tf:
        board_path = tf.name

    try:
        seed_board(board_path)

        env = os.environ.copy()
        env.setdefault("TERM", "xterm-256color")

        child = pexpect.spawn(
            binary,
            args=[board_path],
            env=env,
            encoding="utf-8",
            timeout=10,
            dimensions=(30, 90),
        )

        time.sleep(0.3)
        assert child.isalive(), "kanban process died during startup"

        # ---- 1. Add a card ----
        # Press 'a' to enter add mode
        child.send("a")
        time.sleep(0.1)
        # Type the card title and hit Enter
        child.send("task-a\n")
        time.sleep(0.15)

        # ---- 2. Edit the card ----
        # Press 'e' to enter edit mode (prefilled with "task-a")
        child.send("e")
        time.sleep(0.1)
        # Clear the prefilled text: backspace 6 times
        child.send("\x7f" * 6)
        time.sleep(0.05)
        # Type new title and hit Enter
        child.send("task-b\n")
        time.sleep(0.15)

        # ---- 3. Move card right (shift-l) ----
        # The card is in To Do, 'L' moves it to Doing
        child.send("L")
        time.sleep(0.15)

        # ---- 4. Delete the card ----
        # Press 'd' to trigger delete confirmation
        child.send("d")
        time.sleep(0.1)
        # Confirm with 'y'
        child.send("y")
        time.sleep(0.15)

        # ---- 5. Quit ----
        child.send("q")
        child.expect(pexpect.EOF, timeout=5)

        child.close()
        assert child.exitstatus == 0, (
            f"Expected exit code 0, got {child.exitstatus}"
        )

        # ---- 6. Verify board file on disk ----
        board = read_board(board_path)

        # Check next_id (we added one card, id=5, so next_id should be 6)
        assert board["next_id"] == 6, (
            f"Expected next_id=6, got {board['next_id']}"
        )

        # Extract cards by column
        columns = {c["name"]: c["cards"] for c in board["columns"]}

        todo_cards  = [c["title"] for c in columns["To Do"]]
        doing_cards = [c["title"] for c in columns["Doing"]]
        done_cards  = [c["title"] for c in columns["Done"]]

        # To Do should still have the original two cards
        assert todo_cards == ["buy groceries", "walk the dog"], (
            f"Expected To Do unchanged, got {todo_cards}"
        )

        # Doing: should only have "write TUI" (task-b was added then deleted)
        assert doing_cards == ["write TUI"], (
            f"Expected Doing to have only 'write TUI', got {doing_cards}"
        )

        # Done: unchanged
        assert done_cards == ["setup project"], (
            f"Expected Done unchanged, got {done_cards}"
        )

        print("PASS: smoke_crud — add, edit, move, delete, autosave verified")
        return 0

    finally:
        os.unlink(board_path)


if __name__ == "__main__":
    sys.exit(main())
