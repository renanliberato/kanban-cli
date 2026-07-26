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
        for (int i = 0; i < b->columns[ci].count; i++) {
            Card *card = &b->columns[ci].cards[i];
            free(card->title);
            free(card->description);
            free(card->created_at);
            free(card->updated_at);
            for (int li = 0; li < card->label_count; li++)
                free(card->labels[li]);
            free(card->labels);
        }
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
    c->cards[idx].id          = b->next_id++;
    c->cards[idx].title       = title_copy;
    c->cards[idx].description = NULL;
    c->cards[idx].created_at  = NULL;
    c->cards[idx].updated_at  = NULL;
    c->cards[idx].archived    = 0;
    c->cards[idx].labels      = NULL;
    c->cards[idx].label_count = 0;

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

int board_set_card_description(Board *b, int id, const char *desc)
{
    Card *card = board_get_card(b, id);
    if (!card) return -1;

    free(card->description);
    card->description = desc ? xstrdup(desc) : NULL;

    if (b->db_handle)
        db_set_card_description((db_t *)b->db_handle, id, desc);

    return 0;
}

int board_add_label(Board *b, int id, const char *label)
{
    Card *card = board_get_card(b, id);
    if (!card || !label) return -1;

    /* check if label already exists */
    for (int i = 0; i < card->label_count; i++) {
        if (card->labels[i] && strcmp(card->labels[i], label) == 0)
            return 0;  /* already present, success */
    }

    /* reallocate labels array */
    char **tmp = realloc(card->labels,
                         (size_t)(card->label_count + 1) * sizeof(char *));
    if (!tmp) return -1;
    card->labels = tmp;
    card->labels[card->label_count] = xstrdup(label);
    if (!card->labels[card->label_count]) return -1;
    card->label_count++;

    if (b->db_handle)
        db_add_label((db_t *)b->db_handle, id, label);

    return 0;
}

int board_remove_label(Board *b, int id, const char *label)
{
    Card *card = board_get_card(b, id);
    if (!card || !label) return -1;

    for (int i = 0; i < card->label_count; i++) {
        if (card->labels[i] && strcmp(card->labels[i], label) == 0) {
            free(card->labels[i]);
            /* shift remaining labels down */
            for (int j = i; j < card->label_count - 1; j++)
                card->labels[j] = card->labels[j + 1];
            card->label_count--;
            card->labels[card->label_count] = NULL; /* safety */
            break;
        }
    }

    if (b->db_handle)
        db_remove_label((db_t *)b->db_handle, id, label);

    return 0;
}

/* ------------------------------------------------------------------ */
/* archive / unarchive (M5)                                           */
/* ------------------------------------------------------------------ */

int board_set_card_archived(Board *b, int id, int archived)
{
    Card *card = board_get_card(b, id);
    if (!card) return -1;

    int old = card->archived;
    card->archived = archived ? 1 : 0;

    /* incremental db update: only if changed.
       db_set_card_archived is not available, so we do a full update
       via a separate db call that updates the archived flag. */
    (void)old;

    if (b->db_handle)
        db_set_card_archived((db_t *)b->db_handle, id, card->archived);

    return 0;
}

/* ------------------------------------------------------------------ */
/* restore a deleted card (undo support, M5)                          */
/* ------------------------------------------------------------------ */

int board_restore_card(Board *b, int id, int col, int pos,
                       const char *title, const char *desc,
                       int archived)
{
    if (col < 0 || col >= MAX_COLUMNS || !title) return -1;

    Column *c = &b->columns[col];
    if (col_ensure_capacity(c) != 0) return -1;

    /* clamp pos */
    if (pos < 0) pos = 0;
    if (pos > c->count) pos = c->count;

    /* shift cards down to make room */
    for (int i = c->count; i > pos; i--)
        c->cards[i] = c->cards[i - 1];

    /* zero-initialize the slot */
    memset(&c->cards[pos], 0, sizeof(Card));

    c->cards[pos].id          = id;
    c->cards[pos].title       = xstrdup(title);
    c->cards[pos].description = desc ? xstrdup(desc) : NULL;
    c->cards[pos].archived    = archived;
    c->cards[pos].labels      = NULL;
    c->cards[pos].label_count = 0;
    c->count++;

    /* ensure next_id is beyond this id */
    if (id >= b->next_id) b->next_id = id + 1;

    /* db: insert the card (NOT via db_add_card which would auto-increment) */
    if (b->db_handle)
        db_restore_card((db_t *)b->db_handle, col, id, title, desc, archived);

    return 0;
}

/* ------------------------------------------------------------------ */
/* labels helpers                                                     */
/* ------------------------------------------------------------------ */

int board_get_all_labels(Board *b, char ***names_out, int *count_out)
{
    if (!b || !names_out || !count_out) return -1;

    if (b->db_handle)
        return db_get_all_labels((db_t *)b->db_handle, names_out, count_out);

    /* no db handle — collect from in-memory cards */
    *names_out = NULL;
    *count_out = 0;

    int total = 0;
    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        for (int i = 0; i < b->columns[ci].count; i++)
            total += b->columns[ci].cards[i].label_count;
    }

    if (total == 0) return 0;

    char **names = malloc((size_t)(total + 1) * sizeof(char *));
    if (!names) return -1;

    int n = 0;
    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        for (int i = 0; i < b->columns[ci].count; i++) {
            Card *card = &b->columns[ci].cards[i];
            for (int li = 0; li < card->label_count; li++) {
                if (!card->labels[li]) continue;
                /* check for duplicates */
                int dup = 0;
                for (int j = 0; j < n; j++) {
                    if (strcmp(names[j], card->labels[li]) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    names[n] = xstrdup(card->labels[li]);
                    if (!names[n]) {
                        for (int j = 0; j < n; j++) free(names[j]);
                        free(names);
                        return -1;
                    }
                    n++;
                }
            }
        }
    }

    names[n] = NULL;
    *names_out = names;
    *count_out = n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* M7: comments                                                        */
/* ------------------------------------------------------------------ */

int board_add_comment(Board *b, int card_id, const char *author,
                       const char *body)
{
    if (!b || !author || !body) return -1;

    /* verify card exists */
    Card *card = board_get_card(b, card_id);
    if (!card) return -1;

    if (b->db_handle)
        return db_add_comment((db_t *)b->db_handle, card_id, author, body);

    return -1;
}

int board_get_comments(Board *b, int card_id, Comment **comments_out,
                        int *count_out)
{
    if (!b || !comments_out || !count_out) return -1;

    *comments_out = NULL;
    *count_out = 0;

    if (!b->db_handle) return -1;

    int *ids = NULL;
    char **authors = NULL;
    char **bodies = NULL;
    char **cats = NULL;
    int count = 0;

    int rc = db_get_comments((db_t *)b->db_handle, card_id,
                             &ids, &authors, &bodies, &cats, &count);
    if (rc != 0) return -1;
    if (count == 0) return 0;

    Comment *comments = malloc((size_t)count * sizeof(Comment));
    if (!comments) {
        for (int i = 0; i < count; i++) {
            free(authors[i]); free(bodies[i]); free(cats[i]);
        }
        free(ids); free(authors); free(bodies); free(cats);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        comments[i].id         = ids[i];
        comments[i].author     = authors[i];
        comments[i].body       = bodies[i];
        comments[i].created_at = cats[i];
    }

    free(ids);
    free(authors); free(bodies); free(cats);

    *comments_out = comments;
    *count_out = count;
    return 0;
}

void board_free_comments(Comment *comments, int count)
{
    if (!comments) return;
    for (int i = 0; i < count; i++) {
        free(comments[i].author);
        free(comments[i].body);
        free(comments[i].created_at);
    }
    free(comments);
}

/* ------------------------------------------------------------------ */
/* fuzzy_match: case-insensitive subsequence matching                 */
/* ------------------------------------------------------------------ */

int fuzzy_match(const char *pattern, int nstrings, const char **strings)
{
    if (!pattern || !pattern[0]) return 1;  /* empty pattern matches everything */
    if (!strings || nstrings == 0) return 0;

    /* For each input string, check if pattern chars appear as a subsequence
       (case-insensitive). */
    for (int s = 0; s < nstrings; s++) {
        const char *str = strings[s];
        if (!str) continue;

        const char *p = pattern;
        const char *t = str;
        while (*p && *t) {
            /* case-insensitive char comparison */
            char pc = *p;
            char tc = *t;
            if (pc >= 'A' && pc <= 'Z') pc = (char)(pc + ('a' - 'A'));
            if (tc >= 'A' && tc <= 'Z') tc = (char)(tc + ('a' - 'A'));
            if (pc == tc) p++;
            t++;
        }
        if (!*p) return 1;  /* found full subsequence */
    }

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
    char **descriptions[3] = {NULL, NULL, NULL};
    char **created_ats[3] = {NULL, NULL, NULL};
    char **updated_ats[3] = {NULL, NULL, NULL};
    int *archived_flags[3] = {NULL, NULL, NULL};
    char ***labels[3] = {NULL, NULL, NULL};
    int label_counts[3] = {0, 0, 0};

    int rc = db_load_board(db, &b->next_id, col_counts, ids, titles,
                           descriptions, created_ats, updated_ats,
                           archived_flags, labels, label_counts);
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
            free(descriptions[ci]);
            free(created_ats[ci]);
            free(updated_ats[ci]);
            free(archived_flags[ci]);
            if (labels[ci]) free(labels[ci]);
            continue;
        }

        col->capacity = col_counts[ci] + 4;  /* some headroom */
        col->cards = malloc((size_t)col->capacity * sizeof(Card));
        if (!col->cards) {
            for (int j = 0; j < ci; j++) {
                for (int k = 0; k < b->columns[j].count; k++) {
                    free(b->columns[j].cards[k].title);
                    free(b->columns[j].cards[k].description);
                    free(b->columns[j].cards[k].created_at);
                    free(b->columns[j].cards[k].updated_at);
                    for (int li = 0; li < b->columns[j].cards[k].label_count; li++)
                        free(b->columns[j].cards[k].labels[li]);
                    free(b->columns[j].cards[k].labels);
                }
                free(b->columns[j].cards);
                b->columns[j].cards = NULL;
                b->columns[j].count = 0;
                b->columns[j].capacity = 0;
            }
            for (int j = ci; j < 3; j++) {
                for (int k = 0; k < col_counts[j]; k++) free(titles[j][k]);
                free(titles[j]); free(ids[j]);
                free(descriptions[j]); free(created_ats[j]);
                free(updated_ats[j]); free(archived_flags[j]);
                if (labels[j]) free(labels[j]);
            }
            return -1;
        }

        for (int i = 0; i < col_counts[ci]; i++) {
            Card *card = &col->cards[i];
            card->id          = ids[ci][i];
            card->title       = titles[ci][i];
            card->description = descriptions[ci] ? descriptions[ci][i] : NULL;
            card->created_at  = created_ats[ci] ? created_ats[ci][i] : NULL;
            card->updated_at  = updated_ats[ci] ? updated_ats[ci][i] : NULL;
            card->archived    = archived_flags[ci] ? archived_flags[ci][i] : 0;
            card->label_count = 0;
            card->labels      = NULL;

            /* extract labels from the NULL-terminated array */
            if (labels[ci] && labels[ci][i]) {
                char **larr = labels[ci][i];
                int lc = 0;
                while (larr[lc]) lc++;
                card->label_count = lc;
                card->labels = larr; /* take ownership */
            }
            col->count++;
        }

        free(ids[ci]);
        free(titles[ci]);  /* free the outer pointer array; strings are owned by cards now */
        free(descriptions[ci]);
        free(created_ats[ci]);
        free(updated_ats[ci]);
        free(archived_flags[ci]);
        free(labels[ci]);  /* strings are now owned by cards (or were free'd above) */
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
    char **descriptions[3];
    char **created_ats[3];
    char **updated_ats[3];
    int *archived_flags[3];
    char ***labels[3];

    for (int ci = 0; ci < 3; ci++) {
        col_counts[ci] = b->columns[ci].count;
        if (col_counts[ci] == 0) {
            ids[ci] = NULL; titles[ci] = NULL;
            descriptions[ci] = NULL;
            created_ats[ci] = NULL;
            updated_ats[ci] = NULL;
            archived_flags[ci] = NULL;
            labels[ci] = NULL;
            continue;
        }
        ids[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        titles[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        descriptions[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        created_ats[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        updated_ats[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        archived_flags[ci] = malloc((size_t)col_counts[ci] * sizeof(int));
        labels[ci] = malloc((size_t)col_counts[ci] * sizeof(char *));
        if (!ids[ci] || !titles[ci] || !descriptions[ci] ||
            !created_ats[ci] || !updated_ats[ci] ||
            !archived_flags[ci] || !labels[ci]) {
            for (int j = 0; j < ci; j++) {
                free(ids[j]); free(titles[j]);
                free(descriptions[j]); free(created_ats[j]);
                free(updated_ats[j]); free(archived_flags[j]);
                free(labels[j]);
            }
            free(ids[ci]); free(titles[ci]);
            free(descriptions[ci]); free(created_ats[ci]);
            free(updated_ats[ci]); free(archived_flags[ci]);
            free(labels[ci]);
            return -1;
        }
        for (int i = 0; i < col_counts[ci]; i++) {
            const Card *card = &b->columns[ci].cards[i];
            ids[ci][i]            = card->id;
            titles[ci][i]         = card->title;
            descriptions[ci][i]   = card->description;
            created_ats[ci][i]    = card->created_at;
            updated_ats[ci][i]    = card->updated_at;
            archived_flags[ci][i] = card->archived;
            /* Build NULL-terminated labels array */
            if (card->label_count > 0 && card->labels) {
                char **larr = malloc((size_t)(card->label_count + 1) * sizeof(char *));
                if (larr) {
                    for (int li = 0; li < card->label_count; li++)
                        larr[li] = card->labels[li];
                    larr[card->label_count] = NULL;
                }
                labels[ci][i] = larr;
            } else {
                labels[ci][i] = NULL;
            }
        }
    }

    int label_counts[3] = {
        b->columns[0].count,  /* dummy — db_save_board ignores this */
        b->columns[1].count,
        b->columns[2].count,
    };

    db_t *db = (db_t *)b->db_handle;
    int close_after = 0;

    if (!db) {
        /* no db handle yet — open one for the save (used by unit tests
           that create boards from scratch and save without loading) */
        char *db_path = json_to_db_path(path);
        if (!db_path) {
            for (int ci = 0; ci < 3; ci++) {
                free(ids[ci]); free(titles[ci]);
                free(descriptions[ci]); free(created_ats[ci]);
                free(updated_ats[ci]); free(archived_flags[ci]);
                if (labels[ci]) {
                    for (int i = 0; i < col_counts[ci]; i++)
                        free(labels[ci][i]);
                    free(labels[ci]);
                }
            }
            return -1;
        }
        mkdir_p(db_path);
        db = db_open(db_path);
        free(db_path);
        if (!db) {
            for (int ci = 0; ci < 3; ci++) {
                free(ids[ci]); free(titles[ci]);
                free(descriptions[ci]); free(created_ats[ci]);
                free(updated_ats[ci]); free(archived_flags[ci]);
                if (labels[ci]) {
                    for (int i = 0; i < col_counts[ci]; i++)
                        free(labels[ci][i]);
                    free(labels[ci]);
                }
            }
            return -1;
        }
        close_after = 1;
    }

    int rc = db_save_board(db, b->next_id, col_counts, ids, titles,
                           descriptions, created_ats, updated_ats,
                           archived_flags, labels, label_counts);

    for (int ci = 0; ci < 3; ci++) {
        free(ids[ci]);
        free(titles[ci]);
        free(descriptions[ci]);
        free(created_ats[ci]);
        free(updated_ats[ci]);
        free(archived_flags[ci]);
        if (labels[ci]) {
            for (int i = 0; i < col_counts[ci]; i++)
                free(labels[ci][i]);
            free(labels[ci]);
        }
    }

    if (close_after)
        db_close(db);

    return rc;
}
