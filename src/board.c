#include "board.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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

static const char *column_name_str(int col)
{
    switch (col) {
    case COL_TODO:  return "To Do";
    case COL_DOING: return "Doing";
    case COL_DONE:  return "Done";
    default:        return "Unknown";
    }
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

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

Board board_new(void)
{
    Board b;
    memset(&b, 0, sizeof(b));
    b.next_id = 1;
    return b;
}

void board_free(Board *b)
{
    if (!b) return;
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
    return 0;
}

int board_delete_card(Board *b, int id)
{
    int col, idx;
    find_card(b, id, &col, &idx);
    if (col < 0) return -1;
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
    return 0;
}

/* ------------------------------------------------------------------ */
/* JSON persistence                                                   */
/* ------------------------------------------------------------------ */

int board_save(const Board *b, const char *path)
{
    if (!b || !path) return -1;

    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddNumberToObject(root, "next_id", b->next_id);

    cJSON *cols_array = cJSON_AddArrayToObject(root, "columns");
    if (!cols_array) { cJSON_Delete(root); return -1; }

    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        cJSON *col_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(col_obj, "name", column_name_str(ci));

        cJSON *cards_array = cJSON_AddArrayToObject(col_obj, "cards");
        const Column *col = &b->columns[ci];
        for (int i = 0; i < col->count; i++) {
            cJSON *card_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(card_obj, "id", col->cards[i].id);
            cJSON_AddStringToObject(card_obj, "title", col->cards[i].title);
            cJSON_AddItemToArray(cards_array, card_obj);
        }
        cJSON_AddItemToArray(cols_array, col_obj);
    }

    char *json_str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!json_str) return -1;

    FILE *f = fopen(path, "w");
    if (!f) { free(json_str); return -1; }
    int ret = (fputs(json_str, f) >= 0 && fclose(f) == 0) ? 0 : -1;
    free(json_str);
    return ret;
}

int board_load(Board *b, const char *path)
{
    if (!b || !path) return -1;

    /* default: empty board */
    *b = board_new();

    FILE *f = fopen(path, "r");
    if (!f) {
        /* missing file is not an error — return empty board */
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
        if (col_idx < 0) continue;  /* unknown column — skip */

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

            /* keep next_id ahead of loaded ids */
            if (id_json->valueint >= b->next_id)
                b->next_id = id_json->valueint + 1;
        }
    }

    cJSON_Delete(root);
    return 0;
}
