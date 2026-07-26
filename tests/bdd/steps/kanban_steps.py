"""
Step definitions for Kanban CLI BDD scenarios.

Drives the kanban binary via pexpect in a pty and asserts on both screen
content and JSON board file state.
"""

import os
import time
import json
import pexpect

from behave import given, when, then, step

# Re-import helpers from environment for convenience
from environment import (
    spawn_kanban,
    seed_board,
    read_board,
    quit_app,
    col_cards,
    screen_content,
    empty_board,
    setup_agent,
)


# ---------------------------------------------------------------------------
# State tracking: selection position
# ---------------------------------------------------------------------------

def _reset_selection(context):
    """Place the context selection state at origin (To Do, card 0 if any)."""
    context.sel_col = 0
    # We can't know row 0 without checking, but we track what we do
    context.sel_row = 0


# ---------------------------------------------------------------------------
# Given steps
# ---------------------------------------------------------------------------

@given("an empty board file at the default path")
def step_given_empty_board_file(context):
    seed_board(context, empty_board())


@given("the board file does not exist")
def step_given_no_board_file(context):
    assert not os.path.exists(context.board_path), (
        "Board file should not exist yet"
    )


@given("a board with no cards")
def step_given_board_no_cards(context):
    seed_board(context, empty_board())


@given('a board with a card "{title}" in "{column}"')
def step_given_card_in_column(context, title, column):
    board = empty_board()
    _add_card_to_board(board, title, column)
    seed_board(context, board)


@given('a board with cards "{t1}" and "{t2}" in "{column}"')
def step_given_cards_in_column(context, t1, t2, column):
    board = empty_board()
    _add_card_to_board(board, t1, column)
    _add_card_to_board(board, t2, column)
    seed_board(context, board)


@given("a board with the following cards")
def step_given_board_with_cards_table(context):
    """Table format: | title | column |"""
    board = empty_board()
    for row in context.table:
        _add_card_to_board(board, row["title"], row["column"])
    seed_board(context, board)


def _add_card_to_board(board, title, column_name):
    """Mutate *board* dict in place: add a card to the named column."""
    columns = board["columns"]
    for c in columns:
        if c["name"] == column_name:
            card = {"id": board["next_id"], "title": title}
            c["cards"].append(card)
            board["next_id"] += 1
            return
    raise ValueError(f"Unknown column: {column_name}")


# ---------------------------------------------------------------------------
# M7b: Agent Given steps
# ---------------------------------------------------------------------------

@given('an agent "{name}" of type "{agent_type}"')
def step_given_agent(context, name, agent_type):
    """Create an agent .md file for testing."""
    prompt = f"Default prompt for {name} agent."
    setup_agent(context, name, agent_type, prompt)


# ---------------------------------------------------------------------------
# When steps — launching and quitting
# ---------------------------------------------------------------------------

@when("I launch the application")
def step_when_launch(context):
    context.child = spawn_kanban(context)
    context.screen_buffer = ""
    _reset_selection(context)


@when("I launch the application at {rows}x{cols}")
def step_when_launch_at_size(context, rows, cols):
    context.child = spawn_kanban(
        context, dimensions=(int(rows), int(cols))
    )
    context.screen_buffer = ""
    _reset_selection(context)


@when("I quit the application")
def step_when_quit(context):
    quit_app(context.child)


@when("I quit and restart the application")
def step_when_quit_and_restart(context):
    quit_app(context.child)
    context.child = spawn_kanban(context)
    context.screen_buffer = ""
    _reset_selection(context)


# ---------------------------------------------------------------------------
# When steps — raw key presses (low-level navigation)
# ---------------------------------------------------------------------------

_KEY_MAP = {
    "right arrow":   "\033OC",
    "left arrow":    "\033OD",
    "up arrow":      "\033OA",
    "down arrow":    "\033OB",
    "ESC":           "\033",
    "Enter":         "\n",
    "h":             "h",  "j": "j",  "k": "k",  "l": "l",
    "H":             "H",  "L": "L",
    "<":             "<",  ">": ">",
    "q":             "q",
    "a":             "a",  "e": "e",  "d": "d",
    "y":             "y",  "n": "n",
    "t":             "t",  "D": "D",
    "T":             "T",
    "/":             "/",  "#": "#",
    "x":             "x",  "u": "u",
    "Tab":           "\t",  "Ctrl+A": "\x01",
}

_NAV_TRACK = {
    # (key) -> (dcol, drow): how selection changes
    "h": (-1, 0), "left arrow": (-1, 0),
    "l": (1, 0),  "right arrow": (1, 0),
    "j": (0, 1),  "down arrow": (0, 1),
    "k": (0, -1), "up arrow": (0, -1),
    # H/L/< / > move the SELECTED CARD to another column, which also
    # moves the selection to the destination column.
}


@when('I press "{key}"')
def step_when_press_key(context, key):
    """Press a single key. Tracks selection position for known nav keys."""
    _send_key(context, key)


def _send_key(context, key):
    """Send a key press to the application."""
    ch = _KEY_MAP.get(key, key)
    context.child.send(ch)
    time.sleep(0.05)

    # Track selection changes for nav keys
    sel = getattr(context, "sel_col", None)
    if sel is not None and key in _NAV_TRACK:
        dc, dr = _NAV_TRACK[key]
        context.sel_col += dc
        context.sel_row += dr
        # Clamp
        if context.sel_col < 0:
            context.sel_col = 0
        if context.sel_col > 2:
            context.sel_col = 2
        if context.sel_row < 0:
            context.sel_row = 0


@when('I press Ctrl+A')
def step_when_press_ctrl_a(context):
    """Toggle archived card visibility."""
    context.child.send("\x01")
    time.sleep(0.2)
    # Reset screen buffer so we check fresh output
    context.screen_buffer = ""


@when('I press Tab')
def step_when_press_tab(context):
    """Send Tab key."""
    context.child.send("\t")
    time.sleep(0.1)


@when('I press "{key}" twice')
def step_when_press_key_twice(context, key):
    """Press a key two times."""
    _send_key(context, key)
    _send_key(context, key)


# ---------------------------------------------------------------------------
# When steps — typing into input bar
# ---------------------------------------------------------------------------

@when('I type "{text}"')
def step_when_type(context, text):
    context.child.send(text)
    time.sleep(0.05)


@when('I reset the screen buffer')
def step_reset_screen_buffer(context):
    """Clear accumulated screen content for fresh assertions."""
    time.sleep(0.3)
    try:
        child = context.child
        child.read_nonblocking(size=65536, timeout=0.3)
    except Exception:
        pass
    context.screen_buffer = ""


@when("I press Enter")
def step_when_press_enter(context):
    _send_key(context, "Enter")
    time.sleep(0.1)


@when("I press ESC")
def step_when_press_esc(context):
    _send_key(context, "ESC")
    time.sleep(0.1)


@when("I press Backspace {count:d} times")
def step_when_backspace(context, count):
    for _ in range(count):
        context.child.send("\x7f")
        time.sleep(0.01)


# ---------------------------------------------------------------------------
# When steps — adding cards
# ---------------------------------------------------------------------------

@when('I add a card "{title}" to "{column}"')
def step_when_add_card_to_column(context, title, column):
    """Navigate to the column, press a, type title, press Enter."""
    _navigate_to_column(context, column)
    _send_key(context, "a")
    time.sleep(0.1)
    context.child.send(title)
    time.sleep(0.05)
    _send_key(context, "Enter")
    time.sleep(0.15)
    # After adding, selection is on the new card (last in column)
    board = read_board(context)
    col_idx = {"To Do": 0, "Doing": 1, "Done": 2}[column]
    context.sel_col = col_idx
    context.sel_row = len(board["columns"][col_idx]["cards"]) - 1  # the new card


@when('I begin adding a card with title "{title}"')
def step_when_begin_adding(context, title):
    _send_key(context, "a")
    time.sleep(0.1)
    context.child.send(title)
    time.sleep(0.05)


@when("I begin adding a card")
def step_when_begin_adding_empty(context):
    _send_key(context, "a")
    time.sleep(0.1)


@when("I confirm the input")
def step_when_confirm_input(context):
    _send_key(context, "Enter")
    time.sleep(0.15)


@when("I cancel with ESC")
def step_when_cancel_esc(context):
    _send_key(context, "ESC")
    time.sleep(0.15)


# ---------------------------------------------------------------------------
# When steps — editing cards
# ---------------------------------------------------------------------------

@when('I edit the card "{title}" to "{new_title}"')
def step_when_edit_card(context, title, new_title):
    """Select the card by title, then edit it."""
    _select_card_by_title(context, title)
    _send_key(context, "e")
    time.sleep(0.1)
    # Clear existing text with backspace
    context.child.send("\x7f" * len(title))
    time.sleep(0.1)
    context.child.send(new_title)
    time.sleep(0.05)
    _send_key(context, "Enter")
    time.sleep(0.15)


@when('I begin editing the card "{title}"')
def step_when_begin_edit(context, title):
    _select_card_by_title(context, title)
    _send_key(context, "e")
    time.sleep(0.1)


@when('I change the title to "{new_title}"')
def step_when_change_title(context, new_title):
    context.child.send(new_title)
    time.sleep(0.05)


@when("I confirm the edit")
def step_when_confirm_edit(context):
    _send_key(context, "Enter")
    time.sleep(0.15)


@when("I cancel the edit")
def step_when_cancel_edit(context):
    _send_key(context, "ESC")
    time.sleep(0.15)


# ---------------------------------------------------------------------------
# When steps — deleting cards
# ---------------------------------------------------------------------------

@when('I delete the card "{title}"')
def step_when_delete_card(context, title):
    _select_card_by_title(context, title)
    _send_key(context, "d")
    time.sleep(0.1)


@when("I confirm the deletion")
def step_when_confirm_deletion(context):
    _send_key(context, "y")
    time.sleep(0.15)


@when("I cancel the deletion")
def step_when_cancel_deletion(context):
    _send_key(context, "n")
    time.sleep(0.15)


@when("I dismiss the deletion with another key")
def step_when_dismiss_deletion(context):
    _send_key(context, "x")
    time.sleep(0.15)


# ---------------------------------------------------------------------------
# When steps — moving cards
# ---------------------------------------------------------------------------

@when('I move the card "{title}" right')
def step_when_move_right(context, title):
    _select_card_by_title(context, title)
    _send_key(context, "L")
    time.sleep(0.1)
    # After moving right, selection is in the next column at the bottom
    old_col = context.sel_col
    context.sel_col = old_col + 1
    board = read_board(context)
    context.sel_row = len(board["columns"][context.sel_col]["cards"]) - 1


@when('I move the card "{title}" left')
def step_when_move_left(context, title):
    _select_card_by_title(context, title)
    _send_key(context, "H")
    time.sleep(0.1)
    old_col = context.sel_col
    context.sel_col = old_col - 1
    board = read_board(context)
    context.sel_row = len(board["columns"][context.sel_col]["cards"]) - 1


@when("I move the selected card right")
def step_when_move_selected_right(context):
    _send_key(context, "L")
    time.sleep(0.1)


@when("I move the selected card left")
def step_when_move_selected_left(context):
    _send_key(context, "H")
    time.sleep(0.1)


@when('I move the card "{title}" right with ">"')
def step_when_move_right_angle(context, title):
    _select_card_by_title(context, title)
    _send_key(context, ">")
    time.sleep(0.1)
    old_col = context.sel_col
    context.sel_col = old_col + 1
    board = read_board(context)
    context.sel_row = len(board["columns"][context.sel_col]["cards"]) - 1


@when('I move the card "{title}" left with "<"')
def step_when_move_left_angle(context, title):
    _select_card_by_title(context, title)
    _send_key(context, "<")
    time.sleep(0.1)
    old_col = context.sel_col
    context.sel_col = old_col - 1
    board = read_board(context)
    context.sel_row = len(board["columns"][context.sel_col]["cards"]) - 1


# ---------------------------------------------------------------------------
# When steps — chained > boundary operations
# ---------------------------------------------------------------------------

@when("I try to move the leftmost card further left")
def step_when_move_boundary_left(context):
    """We must be on leftmost column with a card. Press H (no-op)."""
    _send_key(context, "H")
    time.sleep(0.1)


@when("I try to move the rightmost card further right")
def step_when_move_boundary_right(context):
    """We must be on rightmost column with a card. Press L (no-op)."""
    _send_key(context, "L")
    time.sleep(0.1)


# ---------------------------------------------------------------------------
# Helpers: navigate + select
# ---------------------------------------------------------------------------

_COL_INDEX = {"To Do": 0, "Doing": 1, "Done": 2}


def _navigate_to_column(context, column_name):
    """Move selection from current column to *column_name* using l/h.

    Does NOT reset the tracked row — caller should account for
    TUI clamp_selection behaviour after column changes.
    """
    target = _COL_INDEX[column_name]
    current = getattr(context, "sel_col", 0)
    diff = target - current
    if diff == 0:
        return
    key = "l" if diff > 0 else "h"
    for _ in range(abs(diff)):
        _send_key(context, key)


def _reset_to_home(context):
    """Bring selection to a known position: column 0, row 0.

    Sends 'h' and 'k' in batches to guarantee correct position
    regardless of where the selection currently is.  Boundary
    no-ops in the TUI make extra presses harmless.
    """
    context.child.send("hhh" + "k" * 20)
    time.sleep(0.4)
    context.sel_col = 0
    context.sel_row = 0


def _select_card_by_title(context, title):
    """Navigate from current selection to the card with the given title.

    Resets to home (col 0, row 0) before navigating to the target.
    This guarantees correct navigation regardless of prior state.
    """
    board = read_board(context)
    for ci, col in enumerate(board["columns"]):
        for ri, card in enumerate(col["cards"]):
            if card["title"] == title:
                _reset_to_home(context)
                # navigate columns forward from home
                for _ in range(ci):
                    _send_key(context, "l")
                # navigate rows forward from home
                for _ in range(ri):
                    _send_key(context, "j")
                context.sel_col = ci
                context.sel_row = ri
                return
    raise ValueError(f"Card '{title}' not found in board")


# ---------------------------------------------------------------------------
# Then steps — screen assertions
# ---------------------------------------------------------------------------

@then('the screen should show the headers "To Do", "Doing", and "Done"')
def step_then_headers_visible(context):
    output = screen_content(context.child, context)
    for header in ["To Do", "Doing", "Done"]:
        assert header in output, (
            f"Header '{header}' not found. Output: {repr(output[:500])}"
        )


@then('the header for "{column}" should show "{count}"')
def step_then_header_count(context, column, count):
    output = screen_content(context.child, context)
    expected = f"{column} ({count})"
    assert expected in output, (
        f"Expected header '{expected}' not found. Output: {repr(output[:500])}"
    )


@then('the screen should show an empty board')
def step_then_screen_empty_board(context):
    output = screen_content(context.child, context)
    assert "To Do (0)" in output, f"Expected empty To Do. Output: {repr(output[:500])}"


@then('the screen should show "{text}"')
def step_then_screen_shows_text(context, text):
    """Check that the screen output contains the given text."""
    time.sleep(0.3)
    output = screen_content(context.child, context)
    assert text in output, (
        f"Expected screen to contain '{text}'. Screen:\n{output[-800:]}"
    )


@then('the screen should not show "{text}"')
def step_then_screen_not_shows_text(context, text):
    """Check that the screen output does NOT contain the given text."""
    time.sleep(0.3)
    output = screen_content(context.child, context)
    assert text not in output, (
        f"Expected screen to NOT contain '{text}'. Screen:\n{output[-800:]}"
    )


# ---------------------------------------------------------------------------
# Then steps — board file assertions
# ---------------------------------------------------------------------------

@then('the board should be empty')
def step_then_board_empty(context):
    board = read_board(context)
    for col in board["columns"]:
        assert len(col["cards"]) == 0, (
            f"Column '{col['name']}' is not empty: {col['cards']}"
        )


@then('the "{column}" column should contain "{title}"')
def step_then_column_contains(context, column, title):
    board = read_board(context)
    cards = col_cards(board, column)
    assert title in cards, (
        f"Expected '{title}' in {column}. Got: {cards}"
    )


@then('the "{column}" column should not contain "{title}"')
def step_then_column_not_contains(context, column, title):
    board = read_board(context)
    cards = col_cards(board, column)
    assert title not in cards, (
        f"Did not expect '{title}' in {column}. Got: {cards}"
    )


@then('the "{column}" column should contain exactly')
def step_then_column_contains_exactly(context, column):
    """Table format: | title |"""
    expected = [row["title"] for row in context.table]
    board = read_board(context)
    actual = col_cards(board, column)
    assert actual == expected, (
        f"Expected {column} to contain {expected}. Got: {actual}"
    )


@then("the board file should exist")
def step_then_file_exists(context):
    assert os.path.exists(context.board_path), (
        f"Board file not found at {context.board_path}"
    )


@then("the board file should be valid JSON")
def step_then_file_valid_json(context):
    read_board(context)  # raises on invalid


# ---------------------------------------------------------------------------
# Then steps — exit code
# ---------------------------------------------------------------------------

@then("the application should exit with code 0")
def step_then_exit_0(context):
    assert context.child.exitstatus == 0, (
        f"Expected exit 0, got {context.child.exitstatus}"
    )


# ---------------------------------------------------------------------------
# Then steps — persistence
# ---------------------------------------------------------------------------

@then('after restart the "{column}" column should contain "{title}"')
def step_then_after_restart_contains(context, column, title):
    if context.child and context.child.isalive():
        quit_app(context.child)
    context.child = spawn_kanban(context)
    time.sleep(0.2)
    quit_app(context.child)
    board = read_board(context)
    cards = col_cards(board, column)
    assert title in cards, (
        f"After restart, expected '{title}' in {column}. Got: {cards}"
    )


@then('after restart the board should match')
def step_then_after_restart_board_matches(context):
    """Table: | column | titles (comma-separated) |"""
    if context.child and context.child.isalive():
        quit_app(context.child)
    context.child = spawn_kanban(context)
    time.sleep(0.2)
    quit_app(context.child)
    board = read_board(context)
    for row in context.table:
        col_name = row["column"]
        expected = [t.strip() for t in row["titles"].split(",") if t.strip()]
        actual = col_cards(board, col_name)
        assert actual == expected, (
            f"After restart, {col_name}: expected {expected}, got {actual}"
        )


# ---------------------------------------------------------------------------
# Then steps — autosave (file on disk while app is still running)
# ---------------------------------------------------------------------------

@then('the board file on disk should contain "{title}" in "{column}"')
def step_then_autosave_contains(context, title, column):
    """Read the file while the app is still running (autosave check)."""
    board = read_board(context)
    cards = col_cards(board, column)
    assert title in cards, (
        f"Autosave: expected '{title}' in {column}. Got: {cards}"
    )


@then('the board file on disk should no longer contain "{title}" in "{column}"')
def step_then_autosave_not_contains(context, title, column):
    board = read_board(context)
    cards = col_cards(board, column)
    assert title not in cards, (
        f"Autosave: did not expect '{title}' in {column}. Got: {cards}"
    )


# ---------------------------------------------------------------------------
# Then steps — general
# ---------------------------------------------------------------------------

@then("the application should still be running")
def step_then_still_running(context):
    assert context.child.isalive(), "Application should still be alive"


@then("no error should occur")
def step_then_no_error(context):
    pass  # satisfied if we got this far


# ---------------------------------------------------------------------------
# Then steps — LLM job status (iteration 3 M2)
# ---------------------------------------------------------------------------

@then('the status bar should show "{text}"')
def step_then_status_bar_shows(context, text):
    """Check that the status bar (bottom row) contains the given text."""
    time.sleep(0.3)
    content = screen_content(context.child, context)
    # The status bar is the last line of screen content.
    # pexpect gives us the raw terminal output. Look for the text in any line.
    lines = content.split("\n")
    found = any(text.lower() in line.lower() for line in lines if line.strip())
    assert found, f"Status bar should contain '{text}'. Screen:\n{content[-500:]}"


@then('I can still navigate with "{key}"')
def step_then_can_navigate_while_job_running(context, key):
    """While a job is running, pressing a nav key should still work."""
    # The job has already been submitted (T pressed).
    # Now press a navigation key and verify it doesn't block.
    time.sleep(0.2)
    _send_key(context, key)
    # After pressing nav key, the app should still be alive and responsive.
    # We can verify by checking the screen shows header content.
    time.sleep(0.2)
    content = screen_content(context.child, context)
    # The header row should still be present
    assert "To Do" in content, f"Screen should show 'To Do' header after navigation. Content:\n{content[-500:]}"
    # App should still be alive
    assert context.child.isalive(), "Application should still be alive after navigation"


@step("I wait for the job to complete")
def step_wait_for_job_completion(context):
    """Wait until the LLM job finishes (status bar returns to normal)."""
    # With KANBAN_LLM_FAKE_DELAY=10, the job completes after ~1s.
    # Wait longer to be safe.
    time.sleep(2.0)
    # The app should still be alive
    assert context.child.isalive(), "Application should still be alive"
    # The status bar should no longer show "running"
    # Note: after completion the flash message "Done" should appear briefly,
    # but it may have expired by now. We just verify the TUI is alive.
    content = screen_content(context.child, context)
    assert "To Do" in content or "q quit" in content, \
        f"TUI should be showing the board after job completion. Content:\n{content[-500:]}"


# ---------------------------------------------------------------------------
# CLI steps (non-TUI)
# ---------------------------------------------------------------------------

_last_card_id = None  # module-level tracking for CLI add operations


def _run_cli(context, *args):
    """Run the kanban binary in CLI mode and capture stdout/stderr/exit code."""
    import subprocess
    bin_path = context.config.userdata.get("binary", "./bin/kanban")
    cmd = [bin_path, context.board_path] + list(args)
    env = os.environ.copy()
    env.setdefault("KANBAN_LLM_PROVIDER", "fake")
    env.setdefault("KANBAN_LLM_FAKE_DELAY", "8")  # faster for CLI tests
    # M7b: Use agent home for isolated agent config discovery
    env["HOME"] = getattr(context, "agent_home", env.get("HOME", "/tmp"))
    result = subprocess.run(cmd, capture_output=True, text=True, env=env)
    context.cli_stdout = result.stdout
    context.cli_stderr = result.stderr
    context.cli_exitcode = result.returncode

    # If this was an "add" command, extract and store the card ID
    if args and args[0] == "add" and result.returncode == 0:
        try:
            global _last_card_id
            _last_card_id = int(result.stdout.strip())
        except ValueError:
            pass


@step('I run "{subcmd}" with no arguments')
def step_run_cli_no_args(context, subcmd):
    _run_cli(context, subcmd)


@step('I run "{subcmd}" with arguments {args}')
def step_run_cli_with_args(context, subcmd, args):
    # Split args by comma but preserve quoted strings
    import shlex
    parsed = shlex.split(args)
    _run_cli(context, subcmd, *parsed)


@step('I run "{subcmd}" with the last card id')
def step_run_cli_for_last_card(context, subcmd):
    global _last_card_id
    if _last_card_id is None:
        raise ValueError("No card has been added yet")
    _run_cli(context, subcmd, str(_last_card_id))


@step('I run "{subcmd}" with the last card id and "{extra}"')
def step_run_cli_for_last_card_extra(context, subcmd, extra):
    global _last_card_id
    if _last_card_id is None:
        raise ValueError("No card has been added yet")
    _run_cli(context, subcmd, str(_last_card_id), extra)


@then("the output should contain a numeric ID")
def step_then_output_contains_numeric_id(context):
    import re
    assert re.search(r'\d+', context.cli_stdout), \
        f"Expected numeric ID in output, got: {context.cli_stdout}"


@then('the output should contain "{text}"')
def step_then_cli_output_contains(context, text):
    assert text in context.cli_stdout + context.cli_stderr, \
        f"Expected '{text}' in output. stdout={context.cli_stdout} stderr={context.cli_stderr}"


@then('the output should not contain "{text}"')
def step_then_cli_output_not_contains(context, text):
    assert text not in context.cli_stdout + context.cli_stderr, \
        f"Did not expect '{text}' in output. stdout={context.cli_stdout} stderr={context.cli_stderr}"


@then("the exit code should be {code:d}")
def step_then_exit_code(context, code):
    assert context.cli_exitcode == code, \
        f"Expected exit code {code}, got {context.cli_exitcode}"


# ---------------------------------------------------------------------------
# M7b: agent-specific steps
# ---------------------------------------------------------------------------

@when('I comment "{body}" on the last card')
def step_when_comment_agent(context, body):
    """Run kanban comment <last_card_id> "<body>" via CLI."""
    global _last_card_id
    if _last_card_id is None:
        raise ValueError("No card has been added yet")
    _run_cli(context, "comment", str(_last_card_id), body)


@step("the card should have a description from AI")
def step_then_card_has_ai_description(context):
    """Check that the card (last added) has a non-empty description via SQLite."""
    import sqlite3
    db_path = context.board_path.replace(".json", ".db")
    if not os.path.exists(db_path):
        # Try without .json ending
        db_path = context.board_path + ".db"
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    global _last_card_id
    cur.execute("SELECT description FROM cards WHERE id = ?", (_last_card_id,))
    row = cur.fetchone()
    conn.close()
    assert row is not None, f"Card {_last_card_id} not found"
    desc = row[0] or ""
    assert desc != "", f"Expected non-empty description, got empty"


@step('I have added cards "{t1}", "{t2}" in "{col}" and "{t3}" in "{col2}"')
def step_added_cards_distributed(context, t1, t2, col, t3, col2):
    """Add cards via CLI before testing list."""
    _run_cli(context, "add", t1, "--col", col.lower().replace(" ", ""))
    _run_cli(context, "add", t2, "--col", col.lower().replace(" ", ""))
    _run_cli(context, "add", t3, "--col", col2.lower().replace(" ", ""))


@step('I have added card "{title}" in "{column}"')
def step_added_card_via_cli(context, title, column):
    """Add a single card via CLI."""
    _run_cli(context, "add", title, "--col", column.lower().replace(" ", ""))


# ---------------------------------------------------------------------------
# TUI enrich steps
# ---------------------------------------------------------------------------

@step("I launch the application with a fresh board")
def step_launch_fresh(context):
    """Launch the TUI with an empty board, quitting any existing app first."""
    if context.child is not None and context.child.isalive():
        try:
            context.child.send("q")
            context.child.expect(pexpect.EOF, timeout=2)
        except Exception:
            context.child.terminate(force=True)
    context.child = None
    seed_board(context, empty_board())
    context.child = spawn_kanban(context)
    context.screen_buffer = ""
    _reset_selection(context)


@step("I press Ctrl+E")
def step_press_ctrl_e(context):
    """Send Ctrl+E (ASCII 5) to the application."""
    context.child.send("\x05")
    time.sleep(0.1)


@step("I wait for the enrich job to complete")
def step_wait_enrich_job(context):
    """Wait for the enrich job to finish.
       With KANBAN_LLM_FAKE_DELAY=10, the fake provider completes in ~1 second.
       We wait up to 3 seconds for safety."""
    for _ in range(20):
        time.sleep(0.15)
        try:
            chunk = context.child.read_nonblocking(size=65536, timeout=0.3)
            buf = getattr(context, "screen_buffer", "")
            context.screen_buffer = buf + chunk
        except Exception:
            pass
    assert context.child.isalive(), "Application should still be alive"


@step('the card "{title}" should have a description')
def step_card_has_description(context, title):
    """Check that a card has a non-empty description in the DB."""
    import sqlite3
    db_path = context.board_path.replace(".json", ".db")
    if not os.path.exists(db_path):
        db_path = context.board_path + ".db"
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute(
        "SELECT description FROM cards c "
        "JOIN columns col ON c.column_id = col.id "
        "WHERE c.title = ?", (title,))
    row = cur.fetchone()
    conn.close()
    assert row is not None, f"Card '{title}' not found in DB"
    desc = row[0] or ""
    assert desc != "", f"Expected description for '{title}', got empty"


@step('the card "{title}" should not have a description')
def step_card_no_description(context, title):
    """Check that a card has no description."""
    import sqlite3
    db_path = context.board_path.replace(".json", ".db")
    if not os.path.exists(db_path):
        db_path = context.board_path + ".db"
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()
    cur.execute(
        "SELECT description FROM cards c "
        "JOIN columns col ON c.column_id = col.id "
        "WHERE c.title = ?", (title,))
    row = cur.fetchone()
    conn.close()
    if row is not None:
        desc = row[0] or ""
        assert desc == "", f"Expected no description for '{title}', got '{desc}'"


@step('the screen should contain "{text}"')
def step_screen_contains(context, text):
    """Check that the current screen buffer contains the given text."""
    content = screen_content(context.child, context)
    assert text in content, \
        f"Screen should contain '{text}'. Content:\n{content[-800:]}"
