#include "db.h"
#include "../vendor/sqlite3.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SCHEMA_VERSION 1

/* portable strdup — POSIX strdup is not available under -std=c99 */
static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

struct db_s {
    sqlite3 *conn;
    char    *path;
};

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int exec_sql(sqlite3 *conn, const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(conn, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "db: SQL error: %s\nSQL: %s\n", err, sql);
            sqlite3_free(err);
        }
        return -1;
    }
    return 0;
}

static int run_migrations(sqlite3 *conn)
{
    /* check if schema_migrations table exists */
    int version = 0;
    sqlite3_stmt *stmt = NULL;

    int rc = sqlite3_prepare_v2(conn,
        "SELECT MAX(version) FROM schema_migrations", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            version = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    /* table might not exist yet — that's ok, version stays 0 */

    if (version < 1) {
        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS schema_migrations (\n"
            "    version     INTEGER PRIMARY KEY,\n"
            "    applied_at  TEXT NOT NULL DEFAULT (datetime('now'))\n"
            ");") != 0)
            return -1;

        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS boards (\n"
            "    id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    name        TEXT NOT NULL UNIQUE,\n"
            "    next_card_id INTEGER NOT NULL DEFAULT 1,\n"
            "    created_at  TEXT NOT NULL DEFAULT (datetime('now'))\n"
            ");") != 0)
            return -1;

        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS columns (\n"
            "    id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    board_id    INTEGER NOT NULL REFERENCES boards(id) ON DELETE CASCADE,\n"
            "    name        TEXT NOT NULL,\n"
            "    position    INTEGER NOT NULL,\n"
            "    UNIQUE(board_id, name),\n"
            "    UNIQUE(board_id, position)\n"
            ");") != 0)
            return -1;

        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS cards (\n"
            "    id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    column_id   INTEGER NOT NULL REFERENCES columns(id) ON DELETE CASCADE,\n"
            "    title       TEXT NOT NULL,\n"
            "    description TEXT NOT NULL DEFAULT '',\n"
            "    position    INTEGER NOT NULL DEFAULT 0,\n"
            "    archived    INTEGER NOT NULL DEFAULT 0,\n"
            "    created_at  TEXT NOT NULL DEFAULT (datetime('now')),\n"
            "    updated_at  TEXT NOT NULL DEFAULT (datetime('now'))\n"
            ");") != 0)
            return -1;

        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS labels (\n"
            "    id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    name        TEXT NOT NULL UNIQUE,\n"
            "    color       TEXT NOT NULL DEFAULT 'white'\n"
            ");") != 0)
            return -1;

        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS card_labels (\n"
            "    card_id     INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,\n"
            "    label_id    INTEGER NOT NULL REFERENCES labels(id) ON DELETE CASCADE,\n"
            "    PRIMARY KEY (card_id, label_id)\n"
            ");") != 0)
            return -1;

        if (exec_sql(conn,
            "CREATE TABLE IF NOT EXISTS qa_entries (\n"
            "    id          INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "    card_id     INTEGER NOT NULL REFERENCES cards(id) ON DELETE CASCADE,\n"
            "    question    TEXT NOT NULL,\n"
            "    answer      TEXT NOT NULL,\n"
            "    created_at  TEXT NOT NULL DEFAULT (datetime('now'))\n"
            ");") != 0)
            return -1;

        /* record migration */
        if (exec_sql(conn,
            "INSERT INTO schema_migrations (version) VALUES (1);") != 0)
            return -1;

        /* insert default board + columns (idempotent) */
        if (exec_sql(conn,
            "INSERT OR IGNORE INTO boards (id, name) VALUES (1, 'default');") != 0)
            return -1;
        if (exec_sql(conn,
            "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
            "VALUES (1, 1, 'To Do', 0);") != 0)
            return -1;
        if (exec_sql(conn,
            "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
            "VALUES (2, 1, 'Doing', 1);") != 0)
            return -1;
        if (exec_sql(conn,
            "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
            "VALUES (3, 1, 'Done', 2);") != 0)
            return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

db_t *db_open(const char *path)
{
    if (!path) return NULL;

    db_t *db = malloc(sizeof(db_t));
    if (!db) return NULL;
    memset(db, 0, sizeof(*db));

    db->path = xstrdup(path);
    if (!db->path) {
        free(db);
        return NULL;
    }

    int rc = sqlite3_open(path, &db->conn);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db: cannot open '%s': %s\n", path,
                sqlite3_errmsg(db->conn));
        free(db->path);
        free(db);
        return NULL;
    }

    /* enable WAL + foreign keys */
    if (exec_sql(db->conn, "PRAGMA journal_mode=WAL;") != 0) {
        db_close(db);
        return NULL;
    }
    if (exec_sql(db->conn, "PRAGMA foreign_keys=ON;") != 0) {
        db_close(db);
        return NULL;
    }

    if (run_migrations(db->conn) != 0) {
        db_close(db);
        return NULL;
    }

    return db;
}

void db_close(db_t *db)
{
    if (!db) return;
    if (db->conn) {
        sqlite3_close(db->conn);
        db->conn = NULL;
    }
    free(db->path);
    db->path = NULL;
    free(db);
}

/* ------------------------------------------------------------------ */
/* full board load / save                                             */
/* ------------------------------------------------------------------ */

int db_load_board(db_t *db, int *next_id_out, int col_counts[3],
                  int *ids[3], char **titles[3])
{
    if (!db || !db->conn) return -1;

    /* ensure a default board row exists (id=1, name='default') */
    if (exec_sql(db->conn,
        "INSERT OR IGNORE INTO boards (id, name) VALUES (1, 'default');") != 0)
        return -1;

    /* ensure the 3 default columns exist */
    if (exec_sql(db->conn,
        "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
        "VALUES (1, 1, 'To Do', 0);") != 0)
        return -1;
    if (exec_sql(db->conn,
        "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
        "VALUES (2, 1, 'Doing', 1);") != 0)
        return -1;
    if (exec_sql(db->conn,
        "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
        "VALUES (3, 1, 'Done', 2);") != 0)
        return -1;

    /* find max card id to set next_id, also check boards.next_card_id */
    *next_id_out = 1;

    sqlite3_stmt *stmt = NULL;
    int rc;

    /* prefer the stored next_card_id from the boards table */
    rc = sqlite3_prepare_v2(db->conn,
        "SELECT next_card_id FROM boards WHERE id = 1", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int stored = sqlite3_column_int(stmt, 0);
            if (stored > *next_id_out)
                *next_id_out = stored;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    /* also check MAX(id) from cards as safety net */
    rc = sqlite3_prepare_v2(db->conn,
        "SELECT MAX(id) FROM cards", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int max_id = sqlite3_column_int(stmt, 0);
            if (max_id >= *next_id_out)
                *next_id_out = max_id + 1;
        }
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    /* load cards per column (column_id 1=To Do, 2=Doing, 3=Done) */
    for (int ci = 0; ci < 3; ci++) {
        int col_id = ci + 1;   /* column ids are 1..3 */
        col_counts[ci] = 0;

        /* first count how many cards */
        rc = sqlite3_prepare_v2(db->conn,
            "SELECT COUNT(*) FROM cards WHERE column_id = ?", -1, &stmt, NULL);
        if (rc != SQLITE_OK) return -1;
        sqlite3_bind_int(stmt, 1, col_id);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            col_counts[ci] = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        stmt = NULL;

        if (col_counts[ci] == 0) {
            ids[ci] = NULL;
            titles[ci] = NULL;
            continue;
        }

        ids[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        titles[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        if (!ids[ci] || !titles[ci]) {
            for (int j = 0; j < ci; j++) {
                for (int k = 0; k < col_counts[j]; k++)
                    free(titles[j][k]);
                free(titles[j]);
                free(ids[j]);
            }
            free(ids[ci]);
            free(titles[ci]);
            ids[ci] = NULL;
            titles[ci] = NULL;
            return -1;
        }

        /* load cards ordered by position */
        rc = sqlite3_prepare_v2(db->conn,
            "SELECT id, title FROM cards "
            "WHERE column_id = ? ORDER BY position", -1, &stmt, NULL);
        if (rc != SQLITE_OK) return -1;
        sqlite3_bind_int(stmt, 1, col_id);

        int idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && idx < col_counts[ci]) {
            ids[ci][idx] = sqlite3_column_int(stmt, 0);
            const char *title =
                (const char *)sqlite3_column_text(stmt, 1);
            titles[ci][idx] = title ? xstrdup(title) : xstrdup("");
            idx++;
        }
        col_counts[ci] = idx;  /* actual loaded count */
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    return 0;
}

int db_save_board(db_t *db, int next_id, const int col_counts[3],
                  int *ids[3], char **titles[3])
{
    if (!db || !db->conn) return -1;

    /* wrap in a transaction for speed + atomicity */
    if (exec_sql(db->conn, "BEGIN;") != 0) return -1;

    /* ensure default board + columns exist */
    if (exec_sql(db->conn,
        "INSERT OR IGNORE INTO boards (id, name) VALUES (1, 'default');") != 0) {
        exec_sql(db->conn, "ROLLBACK;");
        return -1;
    }

    static const char *col_names[] = {"To Do", "Doing", "Done"};
    for (int ci = 0; ci < 3; ci++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
            "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
            "VALUES (%d, 1, '%s', %d);", ci + 1, col_names[ci], ci);
        if (exec_sql(db->conn, sql) != 0) { exec_sql(db->conn, "ROLLBACK;"); return -1; }
    }

    /* delete all existing cards, then re-insert */
    if (exec_sql(db->conn, "DELETE FROM cards;") != 0) {
        exec_sql(db->conn, "ROLLBACK;");
        return -1;
    }

    for (int ci = 0; ci < 3; ci++) {
        int col_id = ci + 1;
        for (int i = 0; i < col_counts[ci]; i++) {
            sqlite3_stmt *stmt = NULL;
            int rc = sqlite3_prepare_v2(db->conn,
                "INSERT INTO cards (id, column_id, title, position) "
                "VALUES (?, ?, ?, ?)", -1, &stmt, NULL);
            if (rc != SQLITE_OK) { exec_sql(db->conn, "ROLLBACK;"); return -1; }
            sqlite3_bind_int(stmt, 1, ids[ci][i]);
            sqlite3_bind_int(stmt, 2, col_id);
            sqlite3_bind_text(stmt, 3, titles[ci][i], -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, i);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) { exec_sql(db->conn, "ROLLBACK;"); return -1; }
        }
    }

    if (exec_sql(db->conn, "COMMIT;") != 0) {
        exec_sql(db->conn, "ROLLBACK;");
        return -1;
    }

    /* persist next_card_id */
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
            "UPDATE boards SET next_card_id = %d WHERE id = 1;", next_id);
        if (exec_sql(db->conn, sql) != 0) {
            /* non-fatal — next_id will be recomputed on next load */
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* incremental mutations                                              */
/* ------------------------------------------------------------------ */

int db_add_card(db_t *db, int col, int id, const char *title)
{
    if (!db || !db->conn) return -1;
    int col_id = col + 1;

    /* find the next position in that column */
    sqlite3_stmt *stmt = NULL;
    int pos = 0;
    int rc = sqlite3_prepare_v2(db->conn,
        "SELECT COALESCE(MAX(position), -1) + 1 FROM cards "
        "WHERE column_id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, col_id);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            pos = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    rc = sqlite3_prepare_v2(db->conn,
        "INSERT INTO cards (id, column_id, title, position) "
        "VALUES (?, ?, ?, ?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, col_id);
    sqlite3_bind_text(stmt, 3, title, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, pos);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* update next_card_id if this id raises the ceiling */
    if (rc == SQLITE_DONE) {
        char sql[128];
        snprintf(sql, sizeof(sql),
            "UPDATE boards SET next_card_id = MAX(next_card_id, %d) "
            "WHERE id = 1;", id + 1);
        exec_sql(db->conn, sql);
        return 0;
    }
    return -1;
}

int db_delete_card(db_t *db, int id)
{
    if (!db || !db->conn) return -1;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "DELETE FROM cards WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_move_card(db_t *db, int id, int dest_col)
{
    if (!db || !db->conn) return -1;
    int col_id = dest_col + 1;

    /* find the next position in the destination column */
    sqlite3_stmt *stmt = NULL;
    int pos = 0;
    int rc = sqlite3_prepare_v2(db->conn,
        "SELECT COALESCE(MAX(position), -1) + 1 FROM cards "
        "WHERE column_id = ?", -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, col_id);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            pos = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        stmt = NULL;
    }

    rc = sqlite3_prepare_v2(db->conn,
        "UPDATE cards SET column_id = ?, position = ?, "
        "updated_at = datetime('now') WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, col_id);
    sqlite3_bind_int(stmt, 2, pos);
    sqlite3_bind_int(stmt, 3, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_edit_card_title(db_t *db, int id, const char *new_title)
{
    if (!db || !db->conn) return -1;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "UPDATE cards SET title = ?, updated_at = datetime('now') "
        "WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, new_title, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* JSON migration                                                     */
/* ------------------------------------------------------------------ */

int db_migrate_from_board(db_t *db, int next_id,
                          const int col_counts[3],
                          int *ids[3],
                          char **titles[3])
{
    if (!db || !db->conn) return -1;

    /* ensure schema v1 exists */
    if (run_migrations(db->conn) != 0) return -1;

    /* insert default board */
    if (exec_sql(db->conn,
        "INSERT OR IGNORE INTO boards (id, name) VALUES (1, 'default');") != 0)
        return -1;

    /* insert default columns */
    static const char *col_names[] = {"To Do", "Doing", "Done"};
    for (int ci = 0; ci < 3; ci++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
            "INSERT OR IGNORE INTO columns (id, board_id, name, position) "
            "VALUES (%d, 1, '%s', %d);", ci + 1, col_names[ci], ci);
        if (exec_sql(db->conn, sql) != 0) return -1;
    }

    /* insert cards */
    for (int ci = 0; ci < 3; ci++) {
        int col_id = ci + 1;
        for (int i = 0; i < col_counts[ci]; i++) {
            sqlite3_stmt *stmt = NULL;
            int rc = sqlite3_prepare_v2(db->conn,
                "INSERT INTO cards (id, column_id, title, position) "
                "VALUES (?, ?, ?, ?)", -1, &stmt, NULL);
            if (rc != SQLITE_OK) return -1;
            sqlite3_bind_int(stmt, 1, ids[ci][i]);
            sqlite3_bind_int(stmt, 2, col_id);
            sqlite3_bind_text(stmt, 3, titles[ci][i], -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, i);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) return -1;
        }
    }

    /* store next_card_id */
    {
        char sql[128];
        snprintf(sql, sizeof(sql),
            "UPDATE boards SET next_card_id = %d WHERE id = 1;", next_id);
        exec_sql(db->conn, sql);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* query helpers                                                      */
/* ------------------------------------------------------------------ */

int db_has_migration(db_t *db, int version)
{
    if (!db || !db->conn) return 0;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "SELECT 1 FROM schema_migrations WHERE version = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return 0;
    sqlite3_bind_int(stmt, 1, version);
    int found = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(stmt);
    return found;
}
