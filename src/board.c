#include "board.h"
#include "db.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* portable strdup — POSIX strdup is not available under -std=c99 */
static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

static int column_from_name(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "To Do") == 0) return COL_TODO;
    if (strcmp(name, "Doing") == 0) return COL_DOING;
    if (strcmp(name, "Done")  == 0) return COL_DONE;
    return -1;
}

static int col_ensure_capacity(Column *col)
{
    if (col->count < col->capacity) return 0;
    int newcap = col->capacity ? col->capacity * 2 : 4;
    Card *tmp = realloc(col->cards, (size_t)newcap * sizeof(Card));
    if (!tmp) return -1;
    col->cards    = tmp;
    col->capacity = newcap;
    return 0;
}

/* Find a card by id across all columns. Returns {col_index, card_index}
   or {-1, -1}. */
static void find_card(const Board *b, int id, int *col_out, int *idx_out)
{
    *col_out = -1;
    *idx_out = -1;
    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        const Column *col = &b->columns[ci];
        for (int i = 0; i < col->count; i++) {
            if (col->cards[i].id == id) {
                *col_out = ci;
                *idx_out = i;
                return;
            }
        }
    }
}

/* Remove a card from a column at [idx], shifting the rest down.
   The card's title pointer is invalidated (the caller must free it first). */
static void col_remove_at(Column *col, int idx)
{
    if (idx < 0 || idx >= col->count) return;
    for (int i = idx; i < col->count - 1; i++)
        col->cards[i] = col->cards[i + 1];
    col->count--;
}

/* Check if a file exists */
static int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0);
}

/* Derive a .db path from a .json path (replace the extension). */
static char *json_to_db_path(const char *json_path)
{
    size_t len = strlen(json_path);
    const char *ext = NULL;
    /* find the last '.' */
    for (size_t i = len; i > 0; i--) {
        if (json_path[i - 1] == '.') {
            ext = json_path + i - 1;
            break;
        }
    }
    if (ext) {
        size_t base_len = (size_t)(ext - json_path);
        char *db_path = malloc(base_len + 4);  /* ".db" + \0 */
        if (!db_path) return NULL;
        memcpy(db_path, json_path, base_len);
        memcpy(db_path + base_len, ".db", 4);
        return db_path;
    }
    /* no extension — just append .db */
    char *db_path = malloc(len + 4);
    if (!db_path) return NULL;
    memcpy(db_path, json_path, len);
    memcpy(db_path + len, ".db", 4);
    return db_path;
}

/* Ensure the parent directories for a file path exist (like mkdir -p) */
static int mkdir_p(const char *path)
{
    char *copy = xstrdup(path);
    if (!copy) return -1;

    /* find last '/' */
    char *slash = strrchr(copy, '/');
    if (!slash) { free(copy); return 0; }  /* no directory part */
    *slash = '\0';

    /* build path piece by piece */
    char *p = copy;
    if (*p == '/') p++;  /* skip leading '/' */
    while (*p) {
        char *next = strchr(p, '/');
        if (next) *next = '\0';

        struct stat st;
        if (stat(copy, &st) != 0) {
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return -1;
            }
        }
        if (!next) break;
        *next = '/';
        p = next + 1;
    }

    free(copy);
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

Board board_new(void)
{
    Board b;
    memset(&b, 0, sizeof(b));
    b.next_id = 1;
    b.db_handle = NULL;
    return b;
}

void board_free(Board *b)
{
    if (!b) return;
    if (b->db_handle) {
        db_close((db_t *)b->db_handle);
        b->db_handle = NULL;
    }
    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        for (int i = 0; i < b->columns[ci].count; i++)
            free(b->columns[ci].cards[i].title);
        free(b->columns[ci].cards);
        b->columns[ci].cards    = NULL;
        b->columns[ci].count    = 0;
        b->columns[ci].capacity = 0;
    }
}

Card *board_get_card(const Board *b, int id)
{
    int col, idx;
    find_card(b, id, &col, &idx);
    if (col < 0) return NULL;
    return (Card *)&b->columns[col].cards[idx];
}

int board_add_card(Board *b, int col, const char *title)
{
    if (col < 0 || col >= MAX_COLUMNS || !title) return -1;
    Column *c = &b->columns[col];
    if (col_ensure_capacity(c) != 0) return -1;

    char *title_copy = xstrdup(title);
    if (!title_copy) return -1;

    int idx = c->count++;
    c->cards[idx].id    = b->next_id++;
    c->cards[idx].title = title_copy;

    /* incremental db write */
    if (b->db_handle)
        db_add_card((db_t *)b->db_handle, col, c->cards[idx].id, title);

    return c->cards[idx].id;
}

int board_edit_card_title(Board *b, int id, const char *new_title)
{
    Card *card = board_get_card(b, id);
    if (!card || !new_title) return -1;

    char *title_copy = xstrdup(new_title);
    if (!title_copy) return -1;

    free(card->title);
    card->title = title_copy;

    /* incremental db write */
    if (b->db_handle)
        db_edit_card_title((db_t *)b->db_handle, id, new_title);

    return 0;
}

int board_delete_card(Board *b, int id)
{
    int col, idx;
    find_card(b, id, &col, &idx);
    if (col < 0) return -1;

    /* delete from db before freeing the in-memory card */
    if (b->db_handle)
        db_delete_card((db_t *)b->db_handle, id);

    free(b->columns[col].cards[idx].title);
    col_remove_at(&b->columns[col], idx);
    return 0;
}

int board_move_card(Board *b, int id, int dest_col)
{
    if (dest_col < 0 || dest_col >= MAX_COLUMNS) return -1;
    int src_col, idx;
    find_card(b, id, &src_col, &idx);
    if (src_col < 0) return -1;
    if (src_col == dest_col) return 0;  /* no-op */

    Card card = b->columns[src_col].cards[idx];
    col_remove_at(&b->columns[src_col], idx);

    Column *dc = &b->columns[dest_col];
    if (col_ensure_capacity(dc) != 0) {
        /* attempt to put card back (best-effort) */
        col_ensure_capacity(&b->columns[src_col]);
        int bi = b->columns[src_col].count++;
        b->columns[src_col].cards[bi] = card;
        return -1;
    }
    int di = dc->count++;
    dc->cards[di] = card;

    /* incremental db write */
    if (b->db_handle)
        db_move_card((db_t *)b->db_handle, id, dest_col);

    return 0;
}

/* ------------------------------------------------------------------ */
/* Load board from SQLite via db.c                                    */
/* ------------------------------------------------------------------ */

static int load_from_db(Board *b, db_t *db)
{
    int col_counts[3] = {0, 0, 0};
    int *ids[3] = {NULL, NULL, NULL};
    char **titles[3] = {NULL, NULL, NULL};

    int rc = db_load_board(db, &b->next_id, col_counts, ids, titles);
    if (rc != 0) return -1;

    /* Load cards directly into columns — bypass board_add_card to avoid
       incremental DB writes and unwanted next_id increments. */
    for (int ci = 0; ci < 3; ci++) {
        Column *col = &b->columns[ci];
        col->count = 0;
        col->capacity = 0;
        col->cards = NULL;

        if (col_counts[ci] == 0) {
            free(titles[ci]);
            free(ids[ci]);
            continue;
        }

        col->capacity = col_counts[ci] + 4;  /* some headroom */
        col->cards = malloc((size_t)col->capacity * sizeof(Card));
        if (!col->cards) {
            for (int j = 0; j < ci; j++) {
                for (int k = 0; k < b->columns[j].count; k++)
                    free(b->columns[j].cards[k].title);
                free(b->columns[j].cards);
                b->columns[j].cards = NULL;
                b->columns[j].count = 0;
                b->columns[j].capacity = 0;
            }
            for (int j = ci; j < 3; j++) {
                for (int k = 0; k < col_counts[j]; k++) free(titles[j][k]);
                free(titles[j]); free(ids[j]);
            }
            return -1;
        }

        for (int i = 0; i < col_counts[ci]; i++) {
            col->cards[i].id = ids[ci][i];
            col->cards[i].title = titles[ci][i];
            col->count++;
        }

        free(ids[ci]);
        free(titles[ci]);  /* free the outer pointer array; strings are owned by cards now */
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* Load board from JSON (old path, used for migration)                */
/* ------------------------------------------------------------------ */

static int load_from_json(Board *b, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT) return 0;
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return 0; }
    rewind(f);

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) { fclose(f); return -1; }
    size_t nread = fread(buf, 1, (size_t)fsize, f);
    fclose(f);
    if (nread != (size_t)fsize) { free(buf); return 0; }
    buf[fsize] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return 0;  /* malformed JSON -> empty board */

    cJSON *next_id_json = cJSON_GetObjectItem(root, "next_id");
    if (cJSON_IsNumber(next_id_json))
        b->next_id = next_id_json->valueint;

    cJSON *cols_array = cJSON_GetObjectItem(root, "columns");
    if (!cJSON_IsArray(cols_array)) { cJSON_Delete(root); return 0; }

    int ncols = cJSON_GetArraySize(cols_array);
    for (int i = 0; i < ncols; i++) {
        cJSON *col_obj = cJSON_GetArrayItem(cols_array, i);
        if (!cJSON_IsObject(col_obj)) continue;

        cJSON *name_json = cJSON_GetObjectItem(col_obj, "name");
        int col_idx = column_from_name(name_json ? name_json->valuestring : NULL);
        if (col_idx < 0) continue;

        cJSON *cards_array = cJSON_GetObjectItem(col_obj, "cards");
        if (!cJSON_IsArray(cards_array)) continue;

        int ncards = cJSON_GetArraySize(cards_array);
        for (int j = 0; j < ncards; j++) {
            cJSON *card_obj = cJSON_GetArrayItem(cards_array, j);
            if (!cJSON_IsObject(card_obj)) continue;

            cJSON *id_json    = cJSON_GetObjectItem(card_obj, "id");
            cJSON *title_json = cJSON_GetObjectItem(card_obj, "title");
            if (!cJSON_IsNumber(id_json) || !cJSON_IsString(title_json))
                continue;

            Column *col = &b->columns[col_idx];
            if (col_ensure_capacity(col) != 0) continue;

            char *title_copy = xstrdup(title_json->valuestring);
            if (!title_copy) continue;

            int ci = col->count++;
            col->cards[ci].id    = id_json->valueint;
            col->cards[ci].title = title_copy;

            if (id_json->valueint >= b->next_id)
                b->next_id = id_json->valueint + 1;
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Migrate from JSON to SQLite (JSON file is preserved)               */
/* ------------------------------------------------------------------ */

static int migrate_json_to_db(Board *b, db_t *db)
{
    int col_counts[3];
    int *ids[3];
    char **titles[3];

    for (int ci = 0; ci < 3; ci++) {
        col_counts[ci] = b->columns[ci].count;
        ids[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        titles[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        if (!ids[ci] || !titles[ci]) return -1;
        for (int i = 0; i < col_counts[ci]; i++) {
            ids[ci][i] = b->columns[ci].cards[i].id;
            titles[ci][i] = b->columns[ci].cards[i].title;
        }
    }

    int rc = db_migrate_from_board(db, b->next_id, col_counts, ids, titles);

    for (int ci = 0; ci < 3; ci++) {
        free(ids[ci]);
        free(titles[ci]);
    }

    return rc;
}

/* ------------------------------------------------------------------ */
/* board_load — main entry point for persistence                      */
/* ------------------------------------------------------------------ */

int board_load(Board *b, const char *path)
{
    if (!b || !path) return -1;

    /* start with a fresh board */
    *b = board_new();

    /* determine db_path and json_path */
    char *db_path = NULL;
    char *json_path = NULL;

    size_t plen = strlen(path);
    int is_json = (plen >= 5 && strcmp(path + plen - 5, ".json") == 0);
    int is_db   = (plen >= 3 && strcmp(path + plen - 3, ".db") == 0);

    if (is_json) {
        json_path = xstrdup(path);
        db_path = json_to_db_path(path);
    } else if (is_db) {
        db_path = xstrdup(path);
        /* derive json_path by replacing .db with .json */
        json_path = malloc(plen + 2);
        if (json_path) {
            memcpy(json_path, path, plen);
            memcpy(json_path + plen - 3, ".json", 6);
        }
    } else {
        /* path has no recognized extension; treat as db */
        db_path = malloc(plen + 4);
        if (db_path) {
            memcpy(db_path, path, plen);
            memcpy(db_path + plen, ".db", 4);
        }
        json_path = malloc(plen + 6);
        if (json_path) {
            memcpy(json_path, path, plen);
            memcpy(json_path + plen, ".json", 6);
        }
    }

    if (!db_path) { free(json_path); return -1; }

    int db_exists = file_exists(db_path);
    int json_exists = json_path ? file_exists(json_path) : 0;

    /* fallback: when using default path ~/.kanban/default.db, also
       check for the legacy ~/.kanban.json file */
    if (!json_exists) {
        const char *home = getenv("HOME");
        if (home) {
            size_t home_len = strlen(home);
            /* check if db_path is under ~/.kanban/ */
            if (strncmp(db_path, home, home_len) == 0) {
                /* check for ~/.kanban.json */
                char legacy_json[1024];
                snprintf(legacy_json, sizeof(legacy_json),
                         "%s/.kanban.json", home);
                if (file_exists(legacy_json)) {
                    free(json_path);
                    json_path = xstrdup(legacy_json);
                    json_exists = 1;
                }
            }
        }
    }

    if (db_exists) {
        /* Normal path: open db and load directly */
        if (mkdir_p(db_path) != 0) { free(db_path); free(json_path); return -1; }
        db_t *db = db_open(db_path);
        if (!db) { free(db_path); free(json_path); return -1; }
        b->db_handle = db;
        if (load_from_db(b, db) != 0) {
            board_free(b);
            *b = board_new();
            b->db_handle = NULL;
        }
    } else if (json_exists) {
        /* Migration path: load JSON, create db, migrate */
        if (load_from_json(b, json_path) != 0) {
            free(db_path); free(json_path);
            return -1;
        }
        if (mkdir_p(db_path) != 0) { free(db_path); free(json_path); return -1; }
        db_t *db = db_open(db_path);
        if (!db) {
            /* db creation failed but JSON loaded — keep in-memory board */
            free(db_path); free(json_path);
            return 0;
        }
        b->db_handle = db;
        if (migrate_json_to_db(b, db) != 0) {
            /* migration failed — close db, keep in-memory board */
            db_close((db_t *)b->db_handle);
            b->db_handle = NULL;
        }
        /* JSON file is preserved (never deleted) */
    } else {
        /* Fresh start: create empty db */
        if (mkdir_p(db_path) != 0) { free(db_path); free(json_path); return -1; }
        db_t *db = db_open(db_path);
        if (!db) { free(db_path); free(json_path); return -1; }
        b->db_handle = db;
    }

    free(db_path);
    free(json_path);
    return 0;
}

/* ------------------------------------------------------------------ */
/* board_save — full sync of in-memory board to SQLite database       */
/* ------------------------------------------------------------------ */

int board_save(const Board *b, const char *path)
{
    if (!b || !path) return -1;

    int col_counts[3];
    int *ids[3];
    char **titles[3];

    for (int ci = 0; ci < 3; ci++) {
        col_counts[ci] = b->columns[ci].count;
        if (col_counts[ci] == 0) {
            ids[ci] = NULL;
            titles[ci] = NULL;
            continue;
        }
        ids[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        titles[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        if (!ids[ci] || !titles[ci]) {
            for (int j = 0; j < ci; j++) { free(ids[j]); free(titles[j]); }
            return -1;
        }
        for (int i = 0; i < col_counts[ci]; i++) {
            ids[ci][i] = b->columns[ci].cards[i].id;
            titles[ci][i] = b->columns[ci].cards[i].title;
        }
    }

    db_t *db = (db_t *)b->db_handle;
    int close_after = 0;

    if (!db) {
        /* no db handle yet — open one for the save (used by unit tests
           that create boards from scratch and save without loading) */
        char *db_path = json_to_db_path(path);
        if (!db_path) {
            for (int ci = 0; ci < 3; ci++) { free(ids[ci]); free(titles[ci]); }
            return -1;
        }
        mkdir_p(db_path);
        db = db_open(db_path);
        free(db_path);
        if (!db) {
            for (int ci = 0; ci < 3; ci++) { free(ids[ci]); free(titles[ci]); }
            return -1;
        }
        close_after = 1;
    }

    int rc = db_save_board(db, b->next_id, col_counts, ids, titles);

    for (int ci = 0; ci < 3; ci++) {
        free(ids[ci]);
        free(titles[ci]);
    }

    if (close_after)
        db_close(db);

    return rc;
}
