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
                    int *ids[3], char **titles[3],
                    char **descriptions[3],
                    char **created_ats[3],
                    char **updated_ats[3],
                    int *archived_flags[3],
                    char ***labels[3],
                    int label_counts[3]);
int   db_save_board(db_t *db, int next_id, const int col_counts[3],
                    int *ids[3], char **titles[3],
                    char **descriptions[3],
                    char **created_ats[3],
                    char **updated_ats[3],
                    int *archived_flags[3],
                    char ***labels[3],
                    int label_counts[3]);

/* incremental mutations */
int   db_add_card(db_t *db, int col, int id, const char *title);
int   db_delete_card(db_t *db, int id);
int   db_move_card(db_t *db, int id, int dest_col);
int   db_edit_card_title(db_t *db, int id, const char *new_title);
int   db_set_card_description(db_t *db, int id, const char *desc);
int   db_add_label(db_t *db, int card_id, const char *label);
int   db_remove_label(db_t *db, int card_id, const char *label);

/* JSON migration — create tables + import from a cJSON-loaded board */
int   db_migrate_from_board(db_t *db, int next_id,
                            const int col_counts[3],
                            int *ids[3],
                            char **titles[3]);

/* query: does a migration row for `version` exist? */
int   db_has_migration(db_t *db, int version);

/* labels: get all distinct labels, and labels for a specific card */
int   db_get_all_labels(db_t *db, char ***names_out, int *count_out);
int   db_get_card_labels(db_t *db, int card_id, char ***names_out, int *count_out);

/* M5: archive/unarchive and restore for undo */
int   db_set_card_archived(db_t *db, int id, int archived);
int   db_restore_card(db_t *db, int col, int id, const char *title,
                     const char *desc, int archived);

#endif
