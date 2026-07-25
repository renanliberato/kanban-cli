#include "tui.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* internal constants                                                 */
/* ------------------------------------------------------------------ */

#define MIN_ROWS 20
#define MIN_COLS 60

enum { COL_TUI_TODO = 0, COL_TUI_DOING = 1, COL_TUI_DONE = 2 };

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

static TuiState state;
static int has_color = 0;

/* color-pair ids */
enum {
    PAIR_HEADER_TODO = 1,
    PAIR_HEADER_DOING,
    PAIR_HEADER_DONE,
    PAIR_SELECTED,
    PAIR_STATUSBAR
};

static const char *column_names[] = { "To Do", "Doing", "Done" };
static const short header_bg[]    = { COLOR_BLUE, COLOR_YELLOW, COLOR_GREEN };

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int col_width(int total_w)
{
    /* 3 columns + 4 vertical-bar positions: (total_w - 4) / 3 */
    int w = (total_w - 4) / 3;
    return w < 6 ? 6 : w;   /* minimum usable column width */
}

/* centre a string in a field of `width` characters, padding
   with spaces on either side.  Returns the number of chars written,
   which is always `width`. */
static int draw_centered(int y, int x, const char *str, int width)
{
    if (!str) str = "";
    int len = (int)strlen(str);
    if (len > width) len = width;
    int left_pad  = (width - len) / 2;
    int right_pad = width - len - left_pad;

    move(y, x);
    for (int i = 0; i < left_pad;  i++) addch(' ');
    for (int i = 0; i < len;       i++) addch((unsigned char)str[i]);
    for (int i = 0; i < right_pad; i++) addch(' ');
    return width;
}

/* draw a card title truncated to fit `width` characters.
   If `selected` is non-zero the text is rendered in reverse video. */
static void draw_card(int y, int x, const char *title, int width, int selected)
{
    if (!title) title = "";
    int len = (int)strlen(title);

    move(y, x);
    if (selected && has_color)
        attron(COLOR_PAIR(PAIR_SELECTED));
    else if (selected)
        attron(A_REVERSE);

    addch(' ');
    for (int i = 0; i < width - 1 && i < len; i++)
        addch((unsigned char)title[i]);
    /* fill rest with spaces */
    for (int i = len + 1; i < width; i++)
        addch(' ');

    if (selected) {
        if (has_color)
            attroff(COLOR_PAIR(PAIR_SELECTED));
        else
            attroff(A_REVERSE);
    }
}

/* ------------------------------------------------------------------ */
/* ncurses init / teardown                                            */
/* ------------------------------------------------------------------ */

static void tui_init(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);          /* hide hardware cursor */
    set_escdelay(25);     /* quick ESC response  */

    if (has_colors()) {
        start_color();
        has_color = 1;
        /* column headers: white text on coloured background */
        init_pair(PAIR_HEADER_TODO,  COLOR_WHITE, header_bg[COL_TUI_TODO]);
        init_pair(PAIR_HEADER_DOING, COLOR_BLACK, header_bg[COL_TUI_DOING]);
        init_pair(PAIR_HEADER_DONE,  COLOR_WHITE, header_bg[COL_TUI_DONE]);
        /* selected card: reversed video */
        init_pair(PAIR_SELECTED, COLOR_BLACK, COLOR_WHITE);
        /* status bar */
        init_pair(PAIR_STATUSBAR, COLOR_BLACK, COLOR_WHITE);
    }
}

static void tui_shutdown(void)
{
    endwin();
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

/* draw a horizontal border line at row y:
     +--------+--------+--------+      (or the ACS equivalent) */
static void draw_border_line(int y, int cw)
{
    move(y, 0);
    addch(ACS_ULCORNER);
    for (int ci = 0; ci < 3; ci++) {
        for (int j = 0; j < cw; j++)
            addch(ACS_HLINE);
        if (ci < 2)
            addch(ACS_TTEE);   /* top tee: ┬ */
    }
    addch(ACS_URCORNER);
}

static void draw_separator(int y, int cw)
{
    move(y, 0);
    addch(ACS_LTEE);    /* ├ */
    for (int ci = 0; ci < 3; ci++) {
        for (int j = 0; j < cw; j++)
            addch(ACS_HLINE);
        if (ci < 2)
            addch(ACS_PLUS);   /* ┼ */
    }
    addch(ACS_RTEE);    /* ┤ */
}

static void draw_bottom(int y, int cw)
{
    move(y, 0);
    addch(ACS_LLCORNER);
    for (int ci = 0; ci < 3; ci++) {
        for (int j = 0; j < cw; j++)
            addch(ACS_HLINE);
        if (ci < 2)
            addch(ACS_BTEE);   /* ┴ */
    }
    addch(ACS_LRCORNER);
}

/* draw the three column headers (row y, just after top border) */
static void draw_headers(int y, int cw)
{
    int x = 0;
    move(y, x++);
    addch(ACS_VLINE);

    for (int ci = 0; ci < 3; ci++) {
        int pair = PAIR_HEADER_TODO + ci;
        if (has_color) attron(COLOR_PAIR(pair));
        else           attron(A_BOLD);

        draw_centered(y, x, column_names[ci], cw);
        x += cw;

        if (has_color) attroff(COLOR_PAIR(pair));
        else           attroff(A_BOLD);

        addch(ACS_VLINE);
        x++;
    }
}

/* draw one card row across all three columns */
static void draw_card_row(int y, int cw, const Board *board, int row_idx)
{
    int x = 0;
    move(y, x++);
    addch(ACS_VLINE);

    for (int ci = 0; ci < 3; ci++) {
        const Column *col = &board->columns[ci];
        int is_selected = (ci == state.sel_col && row_idx == state.sel_card);
        const char *title = "";

        if (row_idx < col->count)
            title = col->cards[row_idx].title;

        draw_card(y, x, title, cw, is_selected);
        x += cw;
        addch(ACS_VLINE);
        x++;
    }
}

/* draw the status bar on the bottom row */
static void draw_status_bar(int rows)
{
    const char *hint = " q quit  ·  hjkl / arrows navigate";
    int len = (int)strlen(hint);

    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else           attron(A_REVERSE);

    /* fill entire line */
    for (int x = 0; x < COLS; x++) {
        int idx = x - 1;
        if (idx >= 0 && idx < len)
            addch((unsigned char)hint[idx]);
        else
            addch(' ');
    }

    if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
    else           attroff(A_REVERSE);
}

/* main draw routine */
static void tui_draw(const Board *board)
{
    erase();

    int rows = LINES;
    int cols = COLS;

    /* terminal too small */
    if (rows < MIN_ROWS || cols < MIN_COLS) {
        const char *msg = "Terminal too small. Resize to at least "
                          "60 columns × 20 rows.";
        int y = rows / 2;
        int x = (cols - (int)strlen(msg)) / 2;
        if (x < 0) x = 0;
        mvaddstr(y, x, msg);
        return;
    }

    int cw = col_width(cols);

    draw_border_line(0, cw);                    /* row 0: top border    */
    draw_headers(1, cw);                        /* row 1: headers       */
    draw_separator(2, cw);                      /* row 2: separator     */

    /* card area: rows 3 .. rows-3 (leaving bottom border + status bar) */
    int card_area_start = 3;
    int card_area_end   = rows - 2;  /* exclusive */
    int max_rows        = card_area_end - card_area_start;
    if (max_rows < 0) max_rows = 0;

    /* figure out how many card rows we need per column */
    int max_cards = 0;
    for (int ci = 0; ci < 3; ci++) {
        if (board->columns[ci].count > max_cards)
            max_cards = board->columns[ci].count;
    }
    int draw_rows = max_cards > max_rows ? max_cards : max_rows;
    if (draw_rows < 1) draw_rows = 1;

    for (int r = 0; r < draw_rows; r++) {
        draw_card_row(card_area_start + r, cw, board, r);
    }

    draw_bottom(card_area_start + draw_rows, cw);   /* bottom border */
    draw_status_bar(rows);                           /* status bar    */

    refresh();
}

/* clamp selection to valid range after column change */
static void clamp_selection(const Board *board)
{
    if (state.sel_col < 0) state.sel_col = 0;
    if (state.sel_col > 2) state.sel_col = 2;

    int count = board->columns[state.sel_col].count;
    if (count == 0) {
        state.sel_card = -1;
    } else {
        if (state.sel_card < 0) state.sel_card = 0;
        if (state.sel_card >= count) state.sel_card = count - 1;
    }
}

/* ------------------------------------------------------------------ */
/* event handling                                                     */
/* ------------------------------------------------------------------ */

static int handle_input(const Board *board, int ch)
{
    switch (ch) {
    case 'q':
    case 'Q':
        return 0;   /* signal loop to exit */

    case 'h':
    case KEY_LEFT:
        if (state.sel_col > 0)
            state.sel_col--;
        clamp_selection(board);
        return 1;

    case 'l':
    case KEY_RIGHT:
        if (state.sel_col < 2)
            state.sel_col++;
        clamp_selection(board);
        return 1;

    case 'j':
    case KEY_DOWN: {
        const Column *col = &board->columns[state.sel_col];
        if (col->count > 0) {
            if (state.sel_card < col->count - 1)
                state.sel_card++;
            /* else at bottom — stay put */
        }
        return 1;
    }

    case 'k':
    case KEY_UP:
        if (state.sel_card > 0)
            state.sel_card--;
        /* else at top — stay put */
        return 1;

    case KEY_RESIZE:
        /* handled by redrawing — nothing else needed */
        return 1;

    default:
        return 1;   /* ignore unknown keys */
    }
}

/* ------------------------------------------------------------------ */
/* public api                                                         */
/* ------------------------------------------------------------------ */

int tui_run(Board *board, const char *save_path)
{
    if (!board) return -1;

    state.sel_col  = 0;
    state.sel_card = board->columns[0].count > 0 ? 0 : -1;

    tui_init();
    tui_draw(board);

    int running = 1;
    while (running) {
        int ch = getch();
        running = handle_input(board, ch);
        if (running)
            tui_draw(board);
    }

    tui_shutdown();

    /* save board on exit */
    if (save_path)
        board_save(board, save_path);

    return 0;
}
