#ifndef BOARD_H
#define BOARD_H

#define COL_TODO  0
#define COL_DOING 1
#define COL_DONE  2
#define MAX_COLUMNS 3

typedef struct {
    int id;
    char *title;
} Card;

typedef struct {
    Card *cards;
    int count;
    int capacity;
} Column;

typedef struct {
    Column columns[MAX_COLUMNS];
    int next_id;
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

/* persistence: returns 0 on success, -1 on error.
   board_load returns an empty Board on missing or malformed file. */
int  board_load(Board *b, const char *path);
int  board_save(const Board *b, const char *path);

#endif
