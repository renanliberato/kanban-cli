#ifndef TUI_H
#define TUI_H

#include "board.h"

typedef struct {
    int sel_col;   /* 0..2, the currently selected column */
    int sel_card;  /* index within the column, -1 if column is empty */
} TuiState;

/*
 * Run the ncurses TUI loop on the given board.
 * Saves the board to 'save_path' on normal exit.
 * board_name is a short display name shown in the title bar (can be NULL).
 * Returns 0 on normal exit (user pressed q), non-zero on error.
 */
int tui_run(Board *board, const char *save_path, const char *board_name);

#endif
