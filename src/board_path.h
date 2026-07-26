#ifndef BOARD_PATH_H
#define BOARD_PATH_H

/*
 * Board-path resolution and discovery module (M6).
 *
 * Single source of truth for "which board file to open".
 * Precedence: explicit path > -b name (+ discovery dir) > discovery > ~/.kanban/default.db
 */

/* Opaque info for list-boards */
typedef struct {
    char *path;          /* malloc'd full path to the .db file */
    char *display_name;  /* malloc'd short name (e.g. "default", "project") */
    int   card_count;    /* number of cards in the board */
} BoardInfo;

/*
 * Resolve the board database path.
 *
 * `abs_path`   - explicit path from argv (NULL if not provided).
 * `board_name` - -b/--board name (NULL if not provided).
 *
 * Returns a malloc'd string; caller must free.
 * Never returns NULL (returns default path on any error).
 */
char *board_path_resolve(const char *abs_path, const char *board_name);

/*
 * List all known boards.
 *
 * Scans $HOME/.kanban/ for .db files and (if a .kanban/ directory is
 * discovered from the current working directory) also scans the local
 * .kanban/ directory.
 *
 * Returns a malloc'd array of *out_count entries; caller must free each
 * entry's .path and .display_name and then free the array itself.
 * Returns NULL and sets *out_count = 0 if no boards found.
 */
BoardInfo *board_path_list(int *out_count);

/*
 * Get a short human-readable display name from a db path.
 * e.g. "/home/user/.kanban/default.db" -> "default"
 *      "/home/user/projects/foo/.kanban/bar.db" -> "bar (local)"
 * Returns malloc'd string; caller must free.
 */
char *board_path_display_name(const char *db_path);

/*
 * Extract the .kanban/ directory path from a board db path.
 * E.g., "/home/user/.kanban/default.db" -> "/home/user/.kanban"
 * Returns malloc'd string, or NULL if the path doesn't contain "/.kanban/".
 * Caller must free.
 */
char *board_path_get_kanban_dir(const char *db_path);

#endif
