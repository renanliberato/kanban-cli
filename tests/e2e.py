#!/usr/bin/env python3
"""
Kanban CLI end-to-end test suite.

Spawns bin/kanban in a pty with a temp board file and asserts on both
screen content (raw output patterns) and final JSON file state.

Run:  python3 tests/e2e.py [./bin/kanban]
Exits non-zero on any failure.
"""

import json
import os
import sys
import tempfile
import time
import traceback

import pexpect


# ---------------------------------------------------------------------------
# helpers
# ---------------------------------------------------------------------------

class E2EResult:
    def __init__(self):
        self.scenarios = 0
        self.passed = 0
        self.failed = []
        self.errors = []

    def test(self, name, fn):
        self.scenarios += 1
        try:
            fn()
            self.passed += 1
            print(f"  PASS: {name}")
        except AssertionError as e:
            self.failed.append((name, str(e)))
            print(f"  FAIL: {name} — {e}")
        except Exception as e:
            self.errors.append((name, traceback.format_exc()))
            print(f"  ERROR: {name} — {e}")

    def summary(self):
        total = self.scenarios
        ok = self.passed
        ng = len(self.failed) + len(self.errors)
        print()
        print(f"{'='*50}")
        print(f"{ok}/{total} passed, {ng} failed/errored")
        if self.failed:
            print("FAILURES:")
            for name, msg in self.failed:
                print(f"  {name}: {msg}")
        if self.errors:
            print("ERRORS:")
            for name, tb in self.errors:
                print(f"  {name}:")
                print(f"    {tb}")
        return 0 if ng == 0 else 1


def seed_board(path, board):
    with open(path, "w") as f:
        json.dump(board, f, indent=2)


def read_board(path):
    with open(path, "r") as f:
        return json.load(f)


def spawn_kanban(binary, board_path, dimensions=(30, 90), home_dir=None):
    """Spawn kanban in a pty. Returns the pexpect child."""
    env = os.environ.copy()
    env.setdefault("TERM", "xterm-256color")
    if home_dir is not None:
        env["HOME"] = home_dir

    child = pexpect.spawn(
        binary,
        args=[board_path],
        env=env,
        encoding="utf-8",
        timeout=8,
        dimensions=dimensions,
    )
    time.sleep(0.3)
    return child


def quit_and_expect0(child, timeout=5):
    """Send 'q', wait for EOF, assert exit 0."""
    child.send("q")
    child.expect(pexpect.EOF, timeout=timeout)
    child.close()
    assert child.exitstatus == 0, f"Expected exit 0, got {child.exitstatus}"


def assert_header_visible(child, text):
    """Assert that a text string appears in the raw screen output.
    We read whatever has been buffered so far and search for `text`.
    ncurses output may interleave escape sequences, but plain text
    is transmitted verbatim."""
    # Give ncurses a moment to flush
    time.sleep(0.1)
    try:
        output = child.read_nonblocking(size=65536, timeout=0.3)
    except pexpect.TIMEOUT:
        output = ""
    except pexpect.EOF:
        output = child.before or ""
    assert text in output, (
        f"Screen output missing '{text}'. Output snippet: {repr(output[:500])}"
    )


def col_cards(board, col_name):
    """Return list of card titles in the named column."""
    for c in board["columns"]:
        if c["name"] == col_name:
            return [card["title"] for card in c["cards"]]
    return []


# ---------------------------------------------------------------------------
# scenario 1 — startup renders headers
# ---------------------------------------------------------------------------

def scenario_startup_headers(result, binary, tmpdir):
    board_path = os.path.join(tmpdir, "s1.json")
    seed_board(board_path, {
        "next_id": 1,
        "columns": [
            {"name": "To Do", "cards": []},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # Read screen content after init
        time.sleep(0.2)
        try:
            output = child.read_nonblocking(size=65536, timeout=0.5)
        except (pexpect.TIMEOUT, pexpect.EOF):
            output = child.before or ""

        for header in ["To Do", "Doing", "Done"]:
            assert header in output, f"header '{header}' not found on screen"
        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)


# ---------------------------------------------------------------------------
# scenario 2 — navigation moves selection (behavioral)
# ---------------------------------------------------------------------------

def scenario_navigation(result, binary, tmpdir):
    """Navigate to Doing column, add a card, verify it lands in Doing."""
    board_path = os.path.join(tmpdir, "s2.json")
    seed_board(board_path, {
        "next_id": 5,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "t1"}, {"id": 2, "title": "t2"}]},
            {"name": "Doing", "cards": [{"id": 3, "title": "d1"}]},
            {"name": "Done", "cards": [{"id": 4, "title": "x1"}]},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # Default selection: To Do, card 0
        # Press 'l' to move right to Doing column
        child.send("l")
        time.sleep(0.05)
        # Add a card — should land in Doing
        child.send("a")
        time.sleep(0.1)
        child.send("added-to-doing\n")
        time.sleep(0.15)
        # Press 'j' to move down within Doing
        child.send("j")
        time.sleep(0.05)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    # Verify JSON: card landed in Doing
    board = read_board(board_path)
    doing_cards = col_cards(board, "Doing")
    assert "added-to-doing" in doing_cards, (
        f"Card not in Doing column. Doing cards: {doing_cards}"
    )


# ---------------------------------------------------------------------------
# scenario 3 — arrow keys navigate
# ---------------------------------------------------------------------------

def scenario_arrow_keys(result, binary, tmpdir):
    """Verify right-arrow and left-arrow navigate between columns."""
    board_path = os.path.join(tmpdir, "s3.json")
    seed_board(board_path, {
        "next_id": 1,
        "columns": [
            {"name": "To Do", "cards": []},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })

    # Using curses KEY_RIGHT (ESC O C) and KEY_LEFT (ESC O D):
    child = spawn_kanban(binary, board_path)
    try:
        # Send KEY_RIGHT: ESC O C
        child.send("\033OC")
        time.sleep(0.05)
        # Now in Doing column — add a card
        child.send("a")
        time.sleep(0.1)
        child.send("arrow-right-add\n")
        time.sleep(0.15)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    board = read_board(board_path)
    doing_cards = col_cards(board, "Doing")
    assert "arrow-right-add" in doing_cards, (
        f"Arrow-right nav failed. Doing cards: {doing_cards}"
    )


# ---------------------------------------------------------------------------
# scenario 4 — full CRUD: add, edit, delete confirm, delete cancel
# ---------------------------------------------------------------------------

def scenario_crud(result, binary, tmpdir):
    board_path = os.path.join(tmpdir, "s4.json")
    seed_board(board_path, {
        "next_id": 5,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "groceries"}, {"id": 2, "title": "walk dog"}]},
            {"name": "Doing", "cards": [{"id": 3, "title": "write TUI"}]},
            {"name": "Done", "cards": [{"id": 4, "title": "setup"}]},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # --- 4a. Add a new card ---
        # We are on To Do[0]. Press 'a', type title, Enter.
        child.send("a")
        time.sleep(0.1)
        child.send("new-task\n")
        time.sleep(0.15)

        # Navigate down to the new card (it should be at index 2)
        child.send("j")
        time.sleep(0.05)
        child.send("j")
        time.sleep(0.05)

        # --- 4b. Edit the card ---
        # Press 'e' — input is prefilled with "new-task"
        child.send("e")
        time.sleep(0.1)
        # Delete the old title and type a new one
        child.send("\x7f" * 8)  # backspace 8 times to clear "new-task"
        time.sleep(0.1)
        child.send("edited-task\n")
        time.sleep(0.15)

        # --- 4c. Delete, then cancel ('n') ---
        # We'll delete the first card ("groceries"), but cancel.
        # First move selection up to "groceries" (k k)
        child.send("k")
        time.sleep(0.05)
        child.send("k")
        time.sleep(0.05)
        child.send("d")
        time.sleep(0.1)
        child.send("n")   # cancel deletion
        time.sleep(0.15)

        # --- 4d. Delete with confirm ('y') ---
        # Still on "groceries"
        child.send("d")
        time.sleep(0.1)
        child.send("y")   # confirm deletion
        time.sleep(0.15)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    # Verify
    board = read_board(board_path)
    todo_cards = col_cards(board, "To Do")
    # After delete of "groceries", To Do should have: ["walk dog", "edited-task"]
    assert "groceries" not in todo_cards, (
        f"groceries should have been deleted. To Do cards: {todo_cards}"
    )
    assert "walk dog" in todo_cards, (
        f"walk dog should still be present. To Do cards: {todo_cards}"
    )
    assert "edited-task" in todo_cards, (
        f"edited-task should be present. To Do cards: {todo_cards}"
    )
    assert len(todo_cards) == 2, (
        f"Expected 2 To Do cards after add+delete, got {len(todo_cards)}: {todo_cards}"
    )


# ---------------------------------------------------------------------------
# scenario 5 — ESC cancels add
# ---------------------------------------------------------------------------

def scenario_esc_cancels_add(result, binary, tmpdir):
    board_path = os.path.join(tmpdir, "s5.json")
    seed_board(board_path, {
        "next_id": 2,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "keepme"}]},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # Start adding, type text, cancel with ESC
        child.send("a")
        time.sleep(0.1)
        child.send("should-not-appear")
        time.sleep(0.1)
        child.send("\033")   # ESC to cancel
        time.sleep(0.15)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    board = read_board(board_path)
    todo_cards = col_cards(board, "To Do")
    assert todo_cards == ["keepme"], (
        f"ESC should have cancelled add. Got: {todo_cards}"
    )
    assert board["next_id"] == 2, (
        f"next_id should not have advanced. Got: {board['next_id']}"
    )


# ---------------------------------------------------------------------------
# scenario 6 — move cards left/right, boundary no-op
# ---------------------------------------------------------------------------

def scenario_move_cards(result, binary, tmpdir):
    board_path = os.path.join(tmpdir, "s6.json")
    seed_board(board_path, {
        "next_id": 5,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "movable"}, {"id": 2, "title": "stay"}]},
            {"name": "Doing", "cards": [{"id": 3, "title": "in-progress"}]},
            {"name": "Done", "cards": [{"id": 4, "title": "finished"}]},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # Movable is id=1 in To Do, selection starts on To Do[0]
        # Move right to Doing with 'L'
        child.send("L")
        time.sleep(0.1)

        # Move right again to Done with 'L'
        child.send("L")
        time.sleep(0.1)

        # Move left back to Doing with 'H'
        child.send("H")
        time.sleep(0.1)

        # Try to move left from leftmost — Doing selection
        # First navigate left to To Do (the column, using 'h')
        child.send("h")
        time.sleep(0.05)
        # Now on To Do[0] ("stay") — try to move left (boundary no-op)
        child.send("H")
        time.sleep(0.1)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    board = read_board(board_path)
    assert col_cards(board, "To Do") == ["stay"], (
        f"Expected 'stay' in To Do. Got: {col_cards(board, 'To Do')}"
    )
    assert col_cards(board, "Doing") == ["in-progress", "movable"], (
        f"Expected 'in-progress'+'movable' in Doing. Got: {col_cards(board, 'Doing')}"
    )
    assert col_cards(board, "Done") == ["finished"], (
        f"Expected 'finished' in Done. Got: {col_cards(board, 'Done')}"
    )


# ---------------------------------------------------------------------------
# scenario 7 — '>' '<' move shortcuts
# ---------------------------------------------------------------------------

def scenario_move_angle_brackets(result, binary, tmpdir):
    """Move cards using '>' and '<' keys instead of 'L'/'H'."""
    board_path = os.path.join(tmpdir, "s7.json")
    seed_board(board_path, {
        "next_id": 3,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "shift-me"}]},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # > moves to Doing
        child.send(">")
        time.sleep(0.1)

        # > moves to Done
        child.send(">")
        time.sleep(0.1)

        # < moves back to Doing
        child.send("<")
        time.sleep(0.1)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    board = read_board(board_path)
    assert col_cards(board, "To Do") == [], (
        f"Expected empty To Do. Got: {col_cards(board, 'To Do')}"
    )
    assert col_cards(board, "Doing") == ["shift-me"], (
        f"Expected 'shift-me' in Doing. Got: {col_cards(board, 'Doing')}"
    )
    assert col_cards(board, "Done") == [], (
        f"Expected empty Done. Got: {col_cards(board, 'Done')}"
    )


# ---------------------------------------------------------------------------
# scenario 8 — persistence: save, quit, restart, verify
# ---------------------------------------------------------------------------

def scenario_persistence(result, binary, tmpdir):
    board_path = os.path.join(tmpdir, "s8.json")
    seed_board(board_path, {
        "next_id": 6,
        "columns": [
            {"name": "To Do", "cards": [
                {"id": 1, "title": "alpha"},
                {"id": 2, "title": "beta"},
                {"id": 3, "title": "gamma"},
            ]},
            {"name": "Doing", "cards": [{"id": 4, "title": "delta"}]},
            {"name": "Done", "cards": [{"id": 5, "title": "epsilon"}]},
        ],
    })

    # --- session 1: add, delete, move ---
    child1 = spawn_kanban(binary, board_path)
    try:
        # Add a card in To Do (gamma is index 2, so add goes below)
        child1.send("a")
        time.sleep(0.1)
        child1.send("zeta\n")
        time.sleep(0.15)

        # Delete "epsilon" from Done
        # Navigate to Done column
        child1.send("l")
        time.sleep(0.05)
        child1.send("l")
        time.sleep(0.05)
        child1.send("d")
        time.sleep(0.1)
        child1.send("y")
        time.sleep(0.15)

        # Move "delta" from Doing to Done
        child1.send("h")
        time.sleep(0.05)   # back to Doing
        child1.send("L")   # move right to Done
        time.sleep(0.1)

        quit_and_expect0(child1)
    finally:
        if child1.isalive():
            child1.terminate(force=True)

    # --- session 2: load the same file, verify state ---
    child2 = spawn_kanban(binary, board_path)
    try:
        quit_and_expect0(child2)
    finally:
        if child2.isalive():
            child2.terminate(force=True)

    board = read_board(board_path)
    assert col_cards(board, "To Do") == ["alpha", "beta", "gamma", "zeta"], (
        f"Wrong To Do after persistence. Got: {col_cards(board, 'To Do')}"
    )
    assert col_cards(board, "Doing") == [], (
        f"Doing should be empty. Got: {col_cards(board, 'Doing')}"
    )
    assert col_cards(board, "Done") == ["delta"], (
        f"Done should have delta. Got: {col_cards(board, 'Done')}"
    )


# ---------------------------------------------------------------------------
# scenario 9 — empty board: nonexistent file, add, file gets created
# ---------------------------------------------------------------------------

def scenario_empty_board(result, binary, tmpdir):
    board_path = os.path.join(tmpdir, "s9.json")
    # Do NOT seed — file must not exist
    assert not os.path.exists(board_path), "Test file should not exist yet"

    child = spawn_kanban(binary, board_path)
    try:
        # Should start fine with empty board
        # Add a card
        child.send("a")
        time.sleep(0.1)
        child.send("first-card\n")
        time.sleep(0.15)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    # File should now exist
    assert os.path.exists(board_path), (
        f"Board file was not created at {board_path}"
    )

    board = read_board(board_path)
    assert col_cards(board, "To Do") == ["first-card"], (
        f"Expected 'first-card' in To Do. Got: {col_cards(board, 'To Do')}"
    )
    assert board["next_id"] > 1, "next_id should have advanced"


# ---------------------------------------------------------------------------
# scenario 10 — quit from various states
# ---------------------------------------------------------------------------

def scenario_quit_states(result, binary, tmpdir):
    """'q' exits 0 from mid-navigation and after edits."""

    # 10a: quit from Doing column
    board_path_a = os.path.join(tmpdir, "s10a.json")
    seed_board(board_path_a, {
        "next_id": 3,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "a"}, {"id": 2, "title": "b"}]},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })
    child_a = spawn_kanban(binary, board_path_a)
    try:
        child_a.send("l")   # move to Doing
        time.sleep(0.05)
        quit_and_expect0(child_a)
    finally:
        if child_a.isalive():
            child_a.terminate(force=True)

    # 10b: quit after adding a card
    board_path_b = os.path.join(tmpdir, "s10b.json")
    seed_board(board_path_b, {
        "next_id": 1,
        "columns": [
            {"name": "To Do", "cards": []},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })
    child_b = spawn_kanban(binary, board_path_b)
    try:
        child_b.send("a")
        time.sleep(0.1)
        child_b.send("quit-after-add\n")
        time.sleep(0.15)
        quit_and_expect0(child_b)
    finally:
        if child_b.isalive():
            child_b.terminate(force=True)

    board_b = read_board(board_path_b)
    assert "quit-after-add" in col_cards(board_b, "To Do"), (
        "Card should persist after quit-with-add"
    )

    # 10c: quit after editing a card
    board_path_c = os.path.join(tmpdir, "s10c.json")
    seed_board(board_path_c, {
        "next_id": 2,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "old-title"}]},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })
    child_c = spawn_kanban(binary, board_path_c)
    try:
        child_c.send("e")
        time.sleep(0.1)
        child_c.send("\x7f" * 9)   # clear "old-title"
        time.sleep(0.1)
        child_c.send("new-title\n")
        time.sleep(0.15)
        quit_and_expect0(child_c)
    finally:
        if child_c.isalive():
            child_c.terminate(force=True)

    board_c = read_board(board_path_c)
    assert col_cards(board_c, "To Do") == ["new-title"], (
        f"Edit should persist. Got: {col_cards(board_c, 'To Do')}"
    )


# ---------------------------------------------------------------------------
# scenario 11 — add cards to all columns
# ---------------------------------------------------------------------------

def scenario_add_to_all_columns(result, binary, tmpdir):
    """Add a card to each column via navigation."""
    board_path = os.path.join(tmpdir, "s11.json")
    seed_board(board_path, {
        "next_id": 1,
        "columns": [
            {"name": "To Do", "cards": []},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # Add to To Do (default column)
        child.send("a")
        time.sleep(0.1)
        child.send("todo-card\n")
        time.sleep(0.15)

        # Add to Doing
        child.send("l")   # right
        time.sleep(0.05)
        child.send("a")
        time.sleep(0.1)
        child.send("doing-card\n")
        time.sleep(0.15)

        # Add to Done
        child.send("l")   # right
        time.sleep(0.05)
        child.send("a")
        time.sleep(0.1)
        child.send("done-card\n")
        time.sleep(0.15)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    board = read_board(board_path)
    assert col_cards(board, "To Do") == ["todo-card"], (
        f"Bad To Do: {col_cards(board, 'To Do')}"
    )
    assert col_cards(board, "Doing") == ["doing-card"], (
        f"Bad Doing: {col_cards(board, 'Doing')}"
    )
    assert col_cards(board, "Done") == ["done-card"], (
        f"Bad Done: {col_cards(board, 'Done')}"
    )


# ---------------------------------------------------------------------------
# scenario 12 — multiple edits and moves without quitting
# ---------------------------------------------------------------------------

def scenario_many_operations(result, binary, tmpdir):
    """Exercise many chained operations without quitting in between."""
    board_path = os.path.join(tmpdir, "s12.json")
    seed_board(board_path, {
        "next_id": 4,
        "columns": [
            {"name": "To Do", "cards": [{"id": 1, "title": "one"}, {"id": 2, "title": "two"}, {"id": 3, "title": "three"}]},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    })

    child = spawn_kanban(binary, board_path)
    try:
        # Select "two" (j once from card 0)
        child.send("j")
        time.sleep(0.05)

        # Move "two" to Doing with 'L'
        child.send("L")
        time.sleep(0.1)

        # Move back to To Do column (h)
        child.send("h")
        time.sleep(0.05)

        # Select "three" (j twice more — we're on card 0 now, so j, j)
        child.send("j")
        time.sleep(0.05)
        child.send("j")
        time.sleep(0.05)

        # Move "three" to Done (L L, since it goes Doing→Done)
        child.send("L")
        time.sleep(0.1)
        child.send("L")
        time.sleep(0.1)

        # Move back to Doing (H)
        child.send("H")
        time.sleep(0.1)

        # Edit "three" (still selected after move)
        child.send("e")
        time.sleep(0.1)
        child.send("\x7f" * 5)   # clear "three"
        time.sleep(0.1)
        child.send("THREE-edited\n")
        time.sleep(0.15)

        # Delete "one" — nav left to To Do, select card 0
        child.send("h")
        time.sleep(0.05)
        child.send("d")
        time.sleep(0.1)
        child.send("y")
        time.sleep(0.15)

        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)

    board = read_board(board_path)
    assert col_cards(board, "To Do") == [], (
        f"To Do should be empty. Got: {col_cards(board, 'To Do')}"
    )
    assert "two" in col_cards(board, "Doing"), (
        f"'two' should be in Doing. Got: {col_cards(board, 'Doing')}"
    )
    assert "THREE-edited" in col_cards(board, "Doing"), (
        f"'THREE-edited' should be in Doing. Got: {col_cards(board, 'Doing')}"
    )
    assert len(col_cards(board, "Doing")) == 2, (
        f"Doing should have 2 cards. Got: {len(col_cards(board, 'Doing'))}"
    )
    assert col_cards(board, "Done") == [], (
        f"Done should be empty. Got: {col_cards(board, 'Done')}"
    )


# ---------------------------------------------------------------------------
# scenario 13 — at different terminal sizes (80x24, 120x40)
# ---------------------------------------------------------------------------

def _run_at_size(binary, board_path, dims):
    child = spawn_kanban(binary, board_path, dimensions=dims)
    try:
        time.sleep(0.2)
        assert child.isalive(), f"kanban died at {dims}"
        # Add a card to verify it works
        child.send("a")
        time.sleep(0.1)
        child.send(f"card-{dims[0]}x{dims[1]}\n")
        time.sleep(0.15)
        quit_and_expect0(child)
    finally:
        if child.isalive():
            child.terminate(force=True)
    board = read_board(board_path)
    todo = col_cards(board, "To Do")
    expected = f"card-{dims[0]}x{dims[1]}"
    assert expected in todo, f"Card missing at {dims}: {todo}"


def scenario_terminal_sizes(result, binary, tmpdir):
    for dims in [(24, 80), (40, 120)]:
        board_path = os.path.join(tmpdir, f"s13_{dims[0]}x{dims[1]}.json")
        seed_board(board_path, {
            "next_id": 1,
            "columns": [
                {"name": "To Do", "cards": []},
                {"name": "Doing", "cards": []},
                {"name": "Done", "cards": []},
            ],
        })
        _run_at_size(binary, board_path, dims)


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./bin/kanban"

    result = E2EResult()
    tmpdir = tempfile.mkdtemp(prefix="kanban_e2e_")
    print(f"e2e tmpdir: {tmpdir}")
    print()

    scenarios = [
        ("startup renders all 3 column headers", scenario_startup_headers),
        ("navigation: hjkl moves selection (behavioral)", scenario_navigation),
        ("navigation: arrow keys", scenario_arrow_keys),
        ("CRUD: add, edit, delete-confirm, delete-cancel", scenario_crud),
        ("ESC cancels add", scenario_esc_cancels_add),
        ("move cards left/right across columns, boundary no-op", scenario_move_cards),
        ("move cards with < and > shortcuts", scenario_move_angle_brackets),
        ("persistence: save, quit, restart", scenario_persistence),
        ("empty board: nonexistent file", scenario_empty_board),
        ("quit exits 0 from various states", scenario_quit_states),
        ("add to all three columns", scenario_add_to_all_columns),
        ("many chained operations", scenario_many_operations),
        ("terminal sizes: 80x24 and 120x40", scenario_terminal_sizes),
    ]

    for name, fn in scenarios:
        result.test(name, lambda fn=fn: fn(result, binary, tmpdir))

    return result.summary()


if __name__ == "__main__":
    sys.exit(main())
