"""
Behave environment for Kanban CLI BDD tests.

Provides helpers for spawning the kanban binary in a pty with a temporary
board file, and for reading/seeding board data (JSON or SQLite).
"""

import json
import os
import sqlite3
import tempfile
import time

import pexpect


def _binary_path(context):
    """Return the path to the kanban binary under test."""
    return context.config.userdata.get("binary", "./bin/kanban")


def _db_path(json_path):
    """Derive the SQLite database path from a .json path."""
    if json_path.endswith(".json"):
        return json_path[:-5] + ".db"
    return json_path + ".db"


def spawn_kanban(context, board_path=None, dimensions=(30, 90)):
    """Spawn kanban in a pty, return the pexpect child.

    If *board_path* is None, uses ``context.board_path``.
    """
    if board_path is None:
        board_path = context.board_path

    env = os.environ.copy()
    env.setdefault("TERM", "xterm-256color")
    # Do NOT override HOME — ncurses may need it for terminfo/configuration.
    # The board path is always passed as a command-line argument.

    child = pexpect.spawn(
        _binary_path(context),
        args=[board_path],
        env=env,
        encoding="utf-8",
        timeout=8,
        dimensions=dimensions,
    )
    time.sleep(0.3)
    return child


def seed_board(context, board, path=None):
    """Write *board* (dict) as JSON to *path* (default ``context.board_path``).

    Also deletes any existing SQLite database at the derived .db path so the
    app will auto-migrate the JSON on next launch.
    """
    if path is None:
        path = context.board_path
    with open(path, "w") as f:
        json.dump(board, f, indent=2)
    # Remove the .db file if it exists so the app re-migrates from JSON
    db = _db_path(path)
    for fpath in (db, db + "-wal", db + "-shm"):
        try:
            os.unlink(fpath)
        except OSError:
            pass


def read_board(context, path=None):
    """Read board state from SQLite or JSON.

    If the SQLite database (.db) exists at the derived path, read from it.
    Otherwise fall back to reading the JSON file directly.
    Returns a dict in the same format regardless of backend.
    """
    if path is None:
        path = context.board_path

    db = _db_path(path)
    if os.path.exists(db):
        return _read_board_from_sqlite(db)
    return _read_board_from_json(path)


def _read_board_from_sqlite(db_path):
    """Query the SQLite database and return a board dict (JSON-compatible format)."""
    conn = sqlite3.connect(db_path)
    conn.row_factory = sqlite3.Row
    cur = conn.cursor()

    # Find max card id to determine next_id
    cur.execute("SELECT COALESCE(MAX(id), 0) AS max_id FROM cards")
    max_id = cur.fetchone()["max_id"]
    next_id = max_id + 1

    columns = []
    for col_name in ("To Do", "Doing", "Done"):
        cur.execute("""
            SELECT c.id, c.title
            FROM cards c
            JOIN columns col ON c.column_id = col.id
            WHERE col.name = ?
            ORDER BY c.position
        """, (col_name,))
        cards = [{"id": row["id"], "title": row["title"]} for row in cur.fetchall()]
        columns.append({"name": col_name, "cards": cards})

    conn.close()
    return {"next_id": next_id, "columns": columns}


def _read_board_from_json(path):
    """Read board from JSON file (legacy fallback)."""
    with open(path, "r") as f:
        return json.load(f)


def quit_app(child, timeout=5):
    """Send 'q', wait for EOF, assert exit 0."""
    child.send("q")
    child.expect(pexpect.EOF, timeout=timeout)
    child.close()
    assert child.exitstatus == 0, f"Expected exit 0, got {child.exitstatus}"


def col_cards(board, col_name):
    """Return list of card titles in the named column."""
    for c in board["columns"]:
        if c["name"] == col_name:
            return [card["title"] for card in c["cards"]]
    return []


def screen_content(child, context=None):
    """Read current screen output, accumulating across calls.

    Pebuffered content is returned.  Subsequent calls may add more
    data to the buffer if the child has produced more output.
    """
    time.sleep(0.2)
    try:
        chunk = child.read_nonblocking(size=65536, timeout=0.5)
    except pexpect.TIMEOUT:
        chunk = ""
    except pexpect.EOF:
        chunk = child.before or ""

    if context is not None:
        buf = getattr(context, "screen_buffer", "")
        context.screen_buffer = buf + chunk
        return context.screen_buffer
    return chunk


def empty_board():
    """Return an empty kanban board dictionary."""
    return {
        "next_id": 1,
        "columns": [
            {"name": "To Do", "cards": []},
            {"name": "Doing", "cards": []},
            {"name": "Done", "cards": []},
        ],
    }


# ---------------------------------------------------------------------------
# behave hooks
# ---------------------------------------------------------------------------

def before_all(context):
    context.binary = _binary_path(context)


def before_scenario(context, scenario):
    context.tmpdir = tempfile.mkdtemp(prefix="kanban_bdd_")
    context.board_path = os.path.join(context.tmpdir, "board.json")
    # child is set by steps that spawn the app
    context.child = None


def after_scenario(context, scenario):
    if context.child is not None:
        try:
            if context.child.isalive():
                context.child.terminate(force=True)
        except Exception:
            pass
        context.child = None

    # Clean up temp dir
    tmp = getattr(context, "tmpdir", None)
    if tmp and os.path.isdir(tmp):
        import shutil
        shutil.rmtree(tmp, ignore_errors=True)
