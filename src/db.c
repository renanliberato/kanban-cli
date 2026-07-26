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
                  int *ids[3], char **titles[3],
                  char **descriptions[3],
                  char **created_ats[3],
                  char **updated_ats[3],
                  int *archived_flags[3],
                  char ***labels[3],
                  int label_counts[3])
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
        if (label_counts) label_counts[ci] = 0;

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
            if (descriptions) descriptions[ci] = NULL;
            if (created_ats) created_ats[ci] = NULL;
            if (updated_ats) updated_ats[ci] = NULL;
            if (archived_flags) archived_flags[ci] = NULL;
            if (labels) labels[ci] = NULL;
            continue;
        }

        /* allocate arrays for all requested fields */
        ids[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        titles[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        if (!ids[ci] || !titles[ci]) {
            for (int j = 0; j < ci; j++) {
                for (int k = 0; k < col_counts[j]; k++)
                    free(titles[j][k]);
                free(titles[j]); free(ids[j]);
            }
            free(ids[ci]); free(titles[ci]);
            ids[ci] = NULL; titles[ci] = NULL;
            return -1;
        }
        memset(titles[ci], 0, (size_t)col_counts[ci] * sizeof(char *));

        /* allocate optional arrays */
        if (descriptions) {
            descriptions[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
            if (descriptions[ci])
                memset(descriptions[ci], 0, (size_t)col_counts[ci] * sizeof(char *));
        }
        if (created_ats) {
            created_ats[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
            if (created_ats[ci])
                memset(created_ats[ci], 0, (size_t)col_counts[ci] * sizeof(char *));
        }
        if (updated_ats) {
            updated_ats[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
            if (updated_ats[ci])
                memset(updated_ats[ci], 0, (size_t)col_counts[ci] * sizeof(char *));
        }
        if (archived_flags) {
            archived_flags[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        }

        /* load cards ordered by position */
        rc = sqlite3_prepare_v2(db->conn,
            "SELECT id, title, description, created_at, updated_at, archived "
            "FROM cards WHERE column_id = ? ORDER BY position", -1, &stmt, NULL);
        if (rc != SQLITE_OK) return -1;
        sqlite3_bind_int(stmt, 1, col_id);

        int idx = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW && idx < col_counts[ci]) {
            ids[ci][idx] = sqlite3_column_int(stmt, 0);
            const char *t = (const char *)sqlite3_column_text(stmt, 1);
            titles[ci][idx] = t ? xstrdup(t) : xstrdup("");

            if (descriptions && descriptions[ci]) {
                const char *d = (const char *)sqlite3_column_text(stmt, 2);
                descriptions[ci][idx] = (d && d[0]) ? xstrdup(d) : NULL;
            }
            if (created_ats && created_ats[ci]) {
                const char *ca = (const char *)sqlite3_column_text(stmt, 3);
                created_ats[ci][idx] = ca ? xstrdup(ca) : NULL;
            }
            if (updated_ats && updated_ats[ci]) {
                const char *ua = (const char *)sqlite3_column_text(stmt, 4);
                updated_ats[ci][idx] = ua ? xstrdup(ua) : NULL;
            }
            if (archived_flags && archived_flags[ci]) {
                archived_flags[ci][idx] = sqlite3_column_int(stmt, 5);
            }
            idx++;
        }
        col_counts[ci] = idx;  /* actual loaded count */
        sqlite3_finalize(stmt);
        stmt = NULL;

        /* load labels for cards in this column */
        if (labels && col_counts[ci] > 0) {
            /* Build a temp table of card ids for efficient label loading */
            /* Simple approach: query labels per card_id in a batch via IN clause.
               We build the list manually since sqlite3 doesn't support arrays. */
            /* Actually, we'll query labels per card individually for simplicity.
               For a kanban board this is perfectly fine. */
            labels[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
            if (labels[ci]) {
                for (int li = 0; li < col_counts[ci]; li++)
                    labels[ci][li] = NULL;
            }
            if (label_counts) label_counts[ci] = 0;

            for (int i = 0; i < col_counts[ci]; i++) {
                int card_id = ids[ci][i];
                rc = sqlite3_prepare_v2(db->conn,
                    "SELECT l.name FROM labels l "
                    "JOIN card_labels cl ON cl.label_id = l.id "
                    "WHERE cl.card_id = ? ORDER BY l.name", -1, &stmt, NULL);
                if (rc != SQLITE_OK) continue;
                sqlite3_bind_int(stmt, 1, card_id);

                /* Count labels first */
                int lcount = 0;
                char **larr = NULL;
                /* First pass: count */
                sqlite3_stmt *stmt2 = NULL;
                rc = sqlite3_prepare_v2(db->conn,
                    "SELECT COUNT(*) FROM card_labels WHERE card_id = ?",
                    -1, &stmt2, NULL);
                if (rc == SQLITE_OK) {
                    sqlite3_bind_int(stmt2, 1, card_id);
                    if (sqlite3_step(stmt2) == SQLITE_ROW)
                        lcount = sqlite3_column_int(stmt2, 0);
                    sqlite3_finalize(stmt2);
                }

                if (lcount > 0) {
                    larr = malloc((size_t)(lcount + 1) * sizeof(char *));
                    if (larr) {
                        int li = 0;
                        while (sqlite3_step(stmt) == SQLITE_ROW && li < lcount) {
                            const char *ln = (const char *)sqlite3_column_text(stmt, 0);
                            larr[li] = ln ? xstrdup(ln) : xstrdup("");
                            li++;
                        }
                        larr[li] = NULL;
                        lcount = li;
                    }
                }
                sqlite3_finalize(stmt);
                stmt = NULL;

                if (labels[ci])
                    labels[ci][i] = larr;
                if (label_counts) {
                    /* We store the count in a separate array structure.
                       Since label_counts is just int[3] (total counts per col),
                       we need per-card counts. Let me use a different approach:
                       store label_counts as an array-of-arrays.
                       Actually, the API is int label_counts[3] which is just
                       total count per column — not per-card.
                       Let me change approach: make labels[ci][i] a NULL-terminated
                       array of strings, and the caller counts them.
                       label_counts[3] becomes the total label count per column. */
                    label_counts[ci] += lcount;
                }
            }
        }
    }

    return 0;
}

int db_save_board(db_t *db, int next_id, const int col_counts[3],
                  int *ids[3], char **titles[3],
                  char **descriptions[3],
                  char **created_ats[3],
                  char **updated_ats[3],
                  int *archived_flags[3],
                  char ***labels[3],
                  int label_counts[3])
{
    (void)label_counts; /* unused — labels are stored as NULL-terminated arrays */
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

    /* delete all existing cards, labels first (FK cascade handles it, but be explicit) */
    if (exec_sql(db->conn, "DELETE FROM card_labels;") != 0) {
        exec_sql(db->conn, "ROLLBACK;");
        return -1;
    }
    if (exec_sql(db->conn, "DELETE FROM cards;") != 0) {
        exec_sql(db->conn, "ROLLBACK;");
        return -1;
    }

    for (int ci = 0; ci < 3; ci++) {
        int col_id = ci + 1;
        for (int i = 0; i < col_counts[ci]; i++) {
            const char *desc  = (descriptions && descriptions[ci]) ? descriptions[ci][i] : "";
            const char *cat   = (created_ats && created_ats[ci]) ? created_ats[ci][i] : NULL;
            const char *uat   = (updated_ats && updated_ats[ci]) ? updated_ats[ci][i] : NULL;
            int arch = (archived_flags && archived_flags[ci]) ? archived_flags[ci][i] : 0;

            /* Build a dynamic INSERT that omits timestamps when they are NULL,
               letting the DEFAULT (datetime('now')) apply naturally.
               We use two different INSERT forms to avoid NULL-bind on NOT NULL columns. */
            const int use_cat = (cat && cat[0]);
            const int use_uat = (uat && uat[0]);
            char insert_sql[512];
            if (use_cat && use_uat) {
                snprintf(insert_sql, sizeof(insert_sql),
                    "INSERT INTO cards (id, column_id, title, description, "
                    "created_at, updated_at, archived, position) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
            } else if (use_cat && !use_uat) {
                snprintf(insert_sql, sizeof(insert_sql),
                    "INSERT INTO cards (id, column_id, title, description, "
                    "created_at, archived, position) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
            } else if (!use_cat && use_uat) {
                snprintf(insert_sql, sizeof(insert_sql),
                    "INSERT INTO cards (id, column_id, title, description, "
                    "updated_at, archived, position) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?)");
            } else {
                snprintf(insert_sql, sizeof(insert_sql),
                    "INSERT INTO cards (id, column_id, title, description, "
                    "archived, position) "
                    "VALUES (?, ?, ?, ?, ?, ?)");
            }

            sqlite3_stmt *stmt = NULL;
            int rc = sqlite3_prepare_v2(db->conn, insert_sql, -1, &stmt, NULL);
            if (rc != SQLITE_OK) { exec_sql(db->conn, "ROLLBACK;"); return -1; }
            sqlite3_bind_int(stmt, 1, ids[ci][i]);
            sqlite3_bind_int(stmt, 2, col_id);
            sqlite3_bind_text(stmt, 3, titles[ci][i], -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, desc ? desc : "", -1, SQLITE_STATIC);
            int bidx = 5;
            if (use_cat)
                sqlite3_bind_text(stmt, bidx++, cat, -1, SQLITE_STATIC);
            if (use_uat)
                sqlite3_bind_text(stmt, bidx++, uat, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, bidx++, arch);
            sqlite3_bind_int(stmt, bidx++, i);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) { exec_sql(db->conn, "ROLLBACK;"); return -1; }

            /* save labels */
            if (labels && labels[ci] && labels[ci][i]) {
                char **larr = labels[ci][i];
                for (int li = 0; larr[li]; li++) {
                    /* ensure label exists in labels table */
                    sqlite3_stmt *lst = NULL;
                    rc = sqlite3_prepare_v2(db->conn,
                        "INSERT OR IGNORE INTO labels (name) VALUES (?)",
                        -1, &lst, NULL);
                    if (rc == SQLITE_OK) {
                        sqlite3_bind_text(lst, 1, larr[li], -1, SQLITE_STATIC);
                        sqlite3_step(lst);
                        sqlite3_finalize(lst);
                    }

                    /* get label id and insert card_label */
                    rc = sqlite3_prepare_v2(db->conn,
                        "INSERT OR IGNORE INTO card_labels (card_id, label_id) "
                        "SELECT ?, id FROM labels WHERE name = ?",
                        -1, &lst, NULL);
                    if (rc == SQLITE_OK) {
                        sqlite3_bind_int(lst, 1, ids[ci][i]);
                        sqlite3_bind_text(lst, 2, larr[li], -1, SQLITE_STATIC);
                        sqlite3_step(lst);
                        sqlite3_finalize(lst);
                    }
                }
            }
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

int db_set_card_description(db_t *db, int id, const char *desc)
{
    if (!db || !db->conn) return -1;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "UPDATE cards SET description = ?, updated_at = datetime('now') "
        "WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, desc ? desc : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_add_label(db_t *db, int card_id, const char *label)
{
    if (!db || !db->conn || !label) return -1;

    /* ensure the label exists in the labels table */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "INSERT OR IGNORE INTO labels (name) VALUES (?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, label, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* create the card_label association */
    rc = sqlite3_prepare_v2(db->conn,
        "INSERT OR IGNORE INTO card_labels (card_id, label_id) "
        "SELECT ?, id FROM labels WHERE name = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, card_id);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* update card's updated_at */
    if (rc == SQLITE_DONE) {
        exec_sql(db->conn,
            "UPDATE cards SET updated_at = datetime('now') WHERE id = ?");
        /* we don't have the id bound in exec_sql, so use a prepared stmt */
        sqlite3_stmt *ust = NULL;
        sqlite3_prepare_v2(db->conn,
            "UPDATE cards SET updated_at = datetime('now') WHERE id = ?",
            -1, &ust, NULL);
        if (ust) {
            sqlite3_bind_int(ust, 1, card_id);
            sqlite3_step(ust);
            sqlite3_finalize(ust);
        }
        return 0;
    }
    return -1;
}

int db_remove_label(db_t *db, int card_id, const char *label)
{
    if (!db || !db->conn || !label) return -1;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "DELETE FROM card_labels "
        "WHERE card_id = ? AND label_id = (SELECT id FROM labels WHERE name = ?)",
        -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, card_id);
    sqlite3_bind_text(stmt, 2, label, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc == SQLITE_DONE) {
        /* update card's updated_at */
        sqlite3_stmt *ust = NULL;
        sqlite3_prepare_v2(db->conn,
            "UPDATE cards SET updated_at = datetime('now') WHERE id = ?",
            -1, &ust, NULL);
        if (ust) {
            sqlite3_bind_int(ust, 1, card_id);
            sqlite3_step(ust);
            sqlite3_finalize(ust);
        }
        return 0;
    }
    return -1;
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

/* ------------------------------------------------------------------ */
/* labels                                                             */
/* ------------------------------------------------------------------ */

int db_get_all_labels(db_t *db, char ***names_out, int *count_out)
{
    if (!db || !db->conn || !names_out || !count_out) return -1;

    *names_out = NULL;
    *count_out = 0;

    /* count first */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "SELECT COUNT(*) FROM labels", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return 0;
    }
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (count == 0) return 0;

    char **names = malloc((size_t)(count + 1) * sizeof(char *));
    if (!names) return -1;

    rc = sqlite3_prepare_v2(db->conn,
        "SELECT name FROM labels ORDER BY name", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(names);
        return -1;
    }

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < count) {
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        names[idx] = n ? xstrdup(n) : xstrdup("");
        idx++;
    }
    sqlite3_finalize(stmt);

    names[idx] = NULL;  /* NULL terminator */
    *names_out = names;
    *count_out = idx;
    return 0;
}

int db_get_card_labels(db_t *db, int card_id, char ***names_out, int *count_out)
{
    if (!db || !db->conn || !names_out || !count_out) return -1;

    *names_out = NULL;
    *count_out = 0;

    /* count first */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "SELECT COUNT(*) FROM card_labels WHERE card_id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, card_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return 0;
    }
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    stmt = NULL;

    if (count == 0) return 0;

    char **names = malloc((size_t)(count + 1) * sizeof(char *));
    if (!names) return -1;

    rc = sqlite3_prepare_v2(db->conn,
        "SELECT l.name FROM labels l "
        "JOIN card_labels cl ON cl.label_id = l.id "
        "WHERE cl.card_id = ? ORDER BY l.name", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        free(names);
        return -1;
    }
    sqlite3_bind_int(stmt, 1, card_id);

    int idx = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && idx < count) {
        const char *n = (const char *)sqlite3_column_text(stmt, 0);
        names[idx] = n ? xstrdup(n) : xstrdup("");
        idx++;
    }
    sqlite3_finalize(stmt);

    names[idx] = NULL;
    *names_out = names;
    *count_out = idx;
    return 0;
}

/* ------------------------------------------------------------------ */
/* M5: archive / restore (undo)                                       */
/* ------------------------------------------------------------------ */

int db_set_card_archived(db_t *db, int id, int archived)
{
    if (!db || !db->conn) return -1;
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db->conn,
        "UPDATE cards SET archived = ?, updated_at = datetime('now') "
        "WHERE id = ?", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, archived ? 1 : 0);
    sqlite3_bind_int(stmt, 2, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_restore_card(db_t *db, int col, int id, const char *title,
                    const char *desc, int archived)
{
    if (!db || !db->conn || !title) return -1;
    int col_id = col + 1;

    /* find next position in that column */
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
        "INSERT INTO cards (id, column_id, title, description, archived, position) "
        "VALUES (?, ?, ?, ?, ?, ?)", -1, &stmt, NULL);
    if (rc != SQLITE_OK) return -1;
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_int(stmt, 2, col_id);
    sqlite3_bind_text(stmt, 3, title, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, desc ? desc : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, archived ? 1 : 0);
    sqlite3_bind_int(stmt, 6, pos);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
