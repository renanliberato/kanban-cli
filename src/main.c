#include "board.h"
#include <stdio.h>
#include <string.h>

#define BOARD_FILE_DEFAULT "kanban.json"

int main(int argc, char **argv)
{
    const char *path = BOARD_FILE_DEFAULT;
    if (argc > 1)
        path = argv[1];

    Board b = board_new();
    if (board_load(&b, path) != 0) {
        fprintf(stderr, "kanban: failed to load board from '%s'\n", path);
        board_free(&b);
        return 1;
    }

    printf("Board loaded from: %s\n", path);
    printf("  Next ID: %d\n", b.next_id);
    printf("  To Do:  %d cards\n", b.columns[COL_TODO].count);
    printf("  Doing:  %d cards\n", b.columns[COL_DOING].count);
    printf("  Done:   %d cards\n", b.columns[COL_DONE].count);

    board_free(&b);
    return 0;
}
