#ifndef BOARD_H
#define BOARD_H

#define COL_TODO  0
#define COL_DOING 1
#define COL_DONE  2
#define MAX_COLUMNS 3

typedef struct {
    int id;
    char *title;
    char *description;   /* may be NULL or empty string */
    char *created_at;    /* ISO 8601 string */
    char *updated_at;    /* ISO 8601 string */
    int archived;        /* 0 or 1 */
    char **labels;       /* simple string array */
    int label_count;
} Card;

typedef struct {
    Card *cards;
    int count;
    int capacity;
} Column;

typedef struct {
    Column columns[MAX_COLUMNS];
    int next_id;
    void *db_handle;   /* opaque db_t* from db.h, NULL when unused */
} Board;

/* lifecycle */
Board board_new(void);
void  board_free(Board *b);

/* access */
Card *board_get_card(const Board *b, int id);

/* mutations */
int  board_add_card(Board *b, int col, const char *title);
int  board_edit_card_title(Board *b, int id, const char *new_title);
int  board_delete_card(Board *b, int id);
int  board_move_card(Board *b, int id, int dest_col);

/* card detail mutations (new in M3) */
int  board_set_card_description(Board *b, int id, const char *desc);
int  board_add_label(Board *b, int id, const char *label);
int  board_remove_label(Board *b, int id, const char *label);

/* archive / unarchive (M5) */
int  board_set_card_archived(Board *b, int id, int archived);

/* restore a deleted card at a specific position (undo support, M5).
   returns new id on success, -1 on error. */
int  board_restore_card(Board *b, int id, int col, int pos,
                        const char *title, const char *desc,
                        int archived);

/* labels: get all labels across the board (from db) */
int  board_get_all_labels(Board *b, char ***names_out, int *count_out);

/* M7: comments */
typedef struct {
    int    id;
    char  *author;
    char  *body;
    char  *created_at;
} Comment;

int  board_add_comment(Board *b, int card_id, const char *author,
                       const char *body);
int  board_get_comments(Board *b, int card_id, Comment **comments_out,
                        int *count_out);
void board_free_comments(Comment *comments, int count);

/* fuzzy match: case-insensitive subsequence match.
   Returns 1 if `pattern` is a subsequence of any of the strings,
   or if `pattern` is empty (match everything).
   Returns 0 otherwise. */
int  fuzzy_match(const char *pattern, int nstrings, const char **strings);

/* persistence: returns 0 on success, -1 on error.
   board_load opens/creates the SQLite database at path, auto-migrating
   from a sibling JSON file if the db doesn't exist.
   missing/malformed files give an empty board (return 0).
   board_save writes the full in-memory state to the database. */
int  board_load(Board *b, const char *path);
int  board_save(const Board *b, const char *path);

#endif
