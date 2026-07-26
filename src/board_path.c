#define _POSIX_C_SOURCE 200809L
#include "board_path.h"
#include "db.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* internal helpers                                                   */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

/* Return the kanban home directory: $HOME/.kanban.
   Returns malloc'd string. Never returns NULL. */
static char *kanban_home_dir(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t len = strlen(home) + 8;  /* "/.kanban" */
    char *path = malloc(len + 1);
    if (!path) { path = xstrdup("/tmp/.kanban"); return path; }
    snprintf(path, len + 1, "%s/.kanban", home);
    return path;
}

/* Walk up from cwd looking for a .kanban/ directory.
   Stops at $HOME (or "/" if HOME is not set).
   Returns malloc'd path to the .kanban/ directory itself,
   or NULL if not found. */
static char *discover_kanban_dir(void)
{
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return NULL;

    const char *home = getenv("HOME");
    if (!home) home = "";

    char *current = xstrdup(cwd);
    if (!current) return NULL;

    while (current && current[0]) {
        size_t len = strlen(current) + 9;
        char *kanban_path = malloc(len);
        if (!kanban_path) { free(current); return NULL; }
        snprintf(kanban_path, len, "%s/.kanban", current);

        struct stat st;
        if (stat(kanban_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* Found it — return the .kanban/ path itself */
            free(current);
            return kanban_path;
        }
        free(kanban_path);

        /* Stop at HOME */
        if (home[0] && strcmp(current, home) == 0) break;
        /* Stop at root */
        if (strcmp(current, "/") == 0) break;

        /* Go up one level */
        char *slash = strrchr(current, '/');
        if (!slash || slash == current) {
            free(current);
            current = xstrdup("/");
        } else {
            *slash = '\0';
        }
    }

    free(current);
    return NULL;
}

/* Build a full .db path from a .kanban/ directory + board name.
   path = <kanban_dir>/<name>.db */
static char *build_db_path(const char *kanban_dir, const char *name)
{
    size_t len = strlen(kanban_dir) + 1 + strlen(name) + 4;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/%s.db", kanban_dir, name);
    return path;
}

/* Count cards in a board database (via db.h). Returns -1 on error. */
static int count_cards_in_db(const char *db_path)
{
    db_t *db = db_open(db_path);
    if (!db) return -1;

    /* Use db_load_board to get col_counts — we don't actually
       need the card data, just the counts. This is slightly
       wasteful but avoids adding a new db_count_cards API. */
    int next_id = 0;
    int col_counts[3] = {0, 0, 0};
    int *ids[3] = {NULL, NULL, NULL};
    char **titles[3] = {NULL, NULL, NULL};
    char **descs[3] = {NULL, NULL, NULL};
    char **cats[3] = {NULL, NULL, NULL};
    char **uats[3] = {NULL, NULL, NULL};
    int *archs[3] = {NULL, NULL, NULL};
    char ***lbls[3] = {NULL, NULL, NULL};
    int lbl_counts[3] = {0, 0, 0};

    int rc = db_load_board(db, &next_id, col_counts,
                           ids, titles, descs, cats, uats,
                           archs, lbls, lbl_counts);

    if (rc != 0) {
        db_close(db);
        return -1;
    }

    /* Free all returned data */
    for (int ci = 0; ci < 3; ci++) {
        for (int i = 0; i < col_counts[ci]; i++) {
            free(titles[ci] ? titles[ci][i] : NULL);
            free(descs[ci] ? descs[ci][i] : NULL);
            free(cats[ci] ? cats[ci][i] : NULL);
            free(uats[ci] ? uats[ci][i] : NULL);
            if (lbls[ci] && lbls[ci][i]) {
                for (int j = 0; lbls[ci][i][j]; j++)
                    free(lbls[ci][i][j]);
                free(lbls[ci][i]);
            }
        }
        free(ids[ci]);
        free(titles[ci]);
        free(descs[ci]);
        free(cats[ci]);
        free(uats[ci]);
        free(archs[ci]);
        free(lbls[ci]);
    }

    db_close(db);
    return col_counts[0] + col_counts[1] + col_counts[2];
}

/* Scan a directory for *.db files (boards) and append to list. */
static int scan_dir_for_boards(const char *kanban_dir, const char *hint_suffix,
                               BoardInfo **list, int *count, int *cap)
{
    DIR *d = opendir(kanban_dir);
    if (!d) return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len <= 3) continue;
        if (strcmp(entry->d_name + len - 3, ".db") != 0) continue;
        /* Skip WAL and journal files (sqlite3 creates .db-wal, .db-shm) */
        if (strstr(entry->d_name, ".db-wal") || strstr(entry->d_name, ".db-shm"))
            continue;

        /* Build full path */
        size_t plen = strlen(kanban_dir) + 1 + len + 1;
        char *full_path = malloc(plen);
        if (!full_path) continue;
        snprintf(full_path, plen, "%s/%s", kanban_dir, entry->d_name);

        /* Check if it's a regular file and readable */
        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(full_path);
            continue;
        }

        /* Deduplicate: skip if already in list */
        int dup = 0;
        for (int i = 0; i < *count; i++) {
            if (strcmp((*list)[i].path, full_path) == 0) {
                dup = 1;
                break;
            }
        }
        if (dup) {
            free(full_path);
            continue;
        }

        /* Grow list */
        if (*count >= *cap) {
            int newcap = *cap ? *cap * 2 : 4;
            BoardInfo *tmp = realloc(*list, (size_t)newcap * sizeof(BoardInfo));
            if (!tmp) { free(full_path); continue; }
            *list = tmp;
            *cap = newcap;
        }

        /* Build display name */
        char name_buf[256];
        memcpy(name_buf, entry->d_name, len - 3);
        name_buf[len - 3] = '\0';

        int card_count = count_cards_in_db(full_path);

        BoardInfo *bi = &(*list)[*count];
        bi->path = full_path;
        if (hint_suffix) {
            size_t dlen = strlen(name_buf) + strlen(hint_suffix) + 4;
            bi->display_name = malloc(dlen);
            if (bi->display_name)
                snprintf(bi->display_name, dlen, "%s%s", name_buf, hint_suffix);
            else
                bi->display_name = xstrdup(name_buf);
        } else {
            bi->display_name = xstrdup(name_buf);
        }
        bi->card_count = card_count;
        (*count)++;
    }

    closedir(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

char *board_path_resolve(const char *abs_path, const char *board_name)
{
    /* 1. Explicit absolute path — use as-is */
    if (abs_path) return xstrdup(abs_path);

    /* 2. Find discovery directory (walk up cwd for .kanban/) */
    char *discovery = discover_kanban_dir();
    char *home_kanban = kanban_home_dir();

    /* 3. Choose .kanban/ directory: discovery if found, else ~/.kanban/ */
    const char *kanban_dir = discovery ? discovery : home_kanban;

    /* 4. Choose board name: -b flag, else "default" */
    const char *name = board_name ? board_name : "default";

    char *result = build_db_path(kanban_dir, name);

    /* Ensure the kanban directory exists if using the home dir */
    if (!discovery) {
        struct stat st;
        if (stat(home_kanban, &st) != 0)
            mkdir(home_kanban, 0755);
    }

    free(discovery);
    free(home_kanban);
    return result;
}

BoardInfo *board_path_list(int *out_count)
{
    if (!out_count) return NULL;
    *out_count = 0;

    BoardInfo *list = NULL;
    int count = 0;
    int cap = 0;

    /* Always scan $HOME/.kanban/ */
    char *home_kanban = kanban_home_dir();
    scan_dir_for_boards(home_kanban, NULL, &list, &count, &cap);
    free(home_kanban);

    /* If discovery found, scan that too (with "(local)" suffix) */
    char *discovery = discover_kanban_dir();
    if (discovery) {
        scan_dir_for_boards(discovery, " (local)", &list, &count, &cap);
        free(discovery);
    }

    *out_count = count;
    return list;
}

char *board_path_display_name(const char *db_path)
{
    if (!db_path) return xstrdup("unknown");

    /* Extract just the filename without .db extension */
    const char *slash = strrchr(db_path, '/');
    const char *fname = slash ? slash + 1 : db_path;

    size_t len = strlen(fname);
    if (len > 3 && strcmp(fname + len - 3, ".db") == 0)
        len -= 3;

    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, fname, len);
        result[len] = '\0';
    }
    return result;
}
