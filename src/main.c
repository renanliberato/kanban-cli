#include "board.h"
#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *default_board_path(void)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    size_t len  = strlen(home) + strlen("/.kanban.json") + 1;
    char  *path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/.kanban.json", home);
    return path;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    char       *allocated_path = NULL;

    if (argc > 1) {
        path = argv[1];
    } else {
        allocated_path = default_board_path();
        path = allocated_path;
    }

    Board b = board_new();
    if (board_load(&b, path) != 0) {
        fprintf(stderr, "kanban: failed to load board from '%s'\n", path);
        board_free(&b);
        free(allocated_path);
        return 1;
    }

    int ret = tui_run(&b, path);

    board_free(&b);
    free(allocated_path);
    return ret;
}
