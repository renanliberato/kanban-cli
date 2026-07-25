#ifndef DB_H
#define DB_H

/*
 * Thin SQLite persistence layer for kanban board data.
 *
 * All functions return 0 on success, -1 on error.
 * The opaque db_t handle is obtained from db_open() and freed by db_close().
 */

typedef struct db_s db_t;

/* lifecycle */
db_t *db_open(const char *path);
void  db_close(db_t *db);

/* full board load / save */
int   db_load_board(db_t *db, int *next_id_out, int col_counts[3],
                    int *ids[3], char **titles[3]);
int   db_save_board(db_t *db, int next_id, const int col_counts[3],
                    int *ids[3], char **titles[3]);

/* incremental mutations */
int   db_add_card(db_t *db, int col, int id, const char *title);
int   db_delete_card(db_t *db, int id);
int   db_move_card(db_t *db, int id, int dest_col);
int   db_edit_card_title(db_t *db, int id, const char *new_title);

/* JSON migration — create tables + import from a cJSON-loaded board */
int   db_migrate_from_board(db_t *db, int next_id,
                            const int col_counts[3],
                            int *ids[3],
                            char **titles[3]);

/* query: does a migration row for `version` exist? */
int   db_has_migration(db_t *db, int version);

#endif
