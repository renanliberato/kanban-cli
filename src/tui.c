#include "tui.h"
#include "llm.h"
#include "enrich.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* autosave helper (forward declaration)                              */
/* ------------------------------------------------------------------ */

static void autosave(const Board *board);
/* ------------------------------------------------------------------ */

#define MIN_ROWS            20
#define MIN_COLS            60
#define INPUT_MAX           128
#define FLASH_DURATION       2    /* seconds flash message stays visible */
#define REVIEW_FIELD_MAX    32   /* max fields in review screen */

enum { COL_TUI_TODO = 0, COL_TUI_DOING = 1, COL_TUI_DONE = 2 };

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

static TuiState    state;
static int         has_color = 0;
static const char *g_save_path = NULL;
static Board      *g_board = NULL;  /* kept for review-mode access */

/* flash message: shown briefly in the status bar after an event */
static char   g_flash_buf[128];
static time_t g_flash_until = 0;

/* spinner state for job-activity indicator */
static int    g_spinner_idx = 0;
static const char g_spinner_chars[] = "|/-\\";

/* enrich review mode state */
static int  g_review_mode  = 0;
static int  g_review_card_id = -1;
static int  g_review_fields = 0;
static int  g_review_sel    = 0;          /* selected field index (0 = Confirm button) */
static int  g_review_accepted[REVIEW_FIELD_MAX];  /* 0=rejected, 1=accepted */
struct review_field {
    int  type;     /* 0=description, 1=label, 2=question */
    char text[256];
    char extra[256]; /* answer text for questions */
};
static struct review_field g_review_items[REVIEW_FIELD_MAX];

static int  g_enrich_job_id = -1;  /* job we're waiting for in review */

/* color-pair ids */
enum {
    PAIR_HEADER_TODO = 1,
    PAIR_HEADER_DOING,
    PAIR_HEADER_DONE,
    PAIR_SELECTED,
    PAIR_STATUSBAR,
    PAIR_REVIEW_ACCEPTED,
    PAIR_REVIEW_REJECTED
};

static const char *column_names[] = { "To Do", "Doing", "Done" };
static const short header_bg[]    = { COLOR_BLUE, COLOR_YELLOW, COLOR_GREEN };

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static int col_width(int total_w)
{
    int w = (total_w - 4) / 3;
    return w < 6 ? 6 : w;
}

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

static const char *make_header(const char *name, int count)
{
    static char buf[64];
    int n = snprintf(buf, sizeof(buf), "%s (%d)", name, count);
    if (n < 0) return name;
    return buf;
}

static void draw_card(int y, int x, const char *title, int width, int selected)
{
    if (!title) title = "";
    int len = (int)strlen(title);
    int usable = width - 1;

    move(y, x);
    if (selected && has_color)
        attron(COLOR_PAIR(PAIR_SELECTED));
    else if (selected)
        attron(A_REVERSE);

    addch(' ');

    if (len <= usable) {
        for (int i = 0; i < len; i++)
            addch((unsigned char)title[i]);
        for (int i = len + 1; i < width; i++)
            addch(' ');
    } else {
        int show = usable - 1;
        if (show < 0) show = 0;
        for (int i = 0; i < show; i++)
            addch((unsigned char)title[i]);
        addch('~');
    }

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
    curs_set(0);

    if (has_colors()) {
        start_color();
        has_color = 1;
        init_pair(PAIR_HEADER_TODO,  COLOR_WHITE, header_bg[COL_TUI_TODO]);
        init_pair(PAIR_HEADER_DOING, COLOR_BLACK, header_bg[COL_TUI_DOING]);
        init_pair(PAIR_HEADER_DONE,  COLOR_WHITE, header_bg[COL_TUI_DONE]);
        init_pair(PAIR_SELECTED, COLOR_BLACK, COLOR_WHITE);
        init_pair(PAIR_STATUSBAR, COLOR_BLACK, COLOR_WHITE);
        init_pair(PAIR_REVIEW_ACCEPTED, COLOR_GREEN, COLOR_BLACK);
        init_pair(PAIR_REVIEW_REJECTED, COLOR_RED, COLOR_BLACK);
    }
}

static void tui_shutdown(void)
{
    endwin();
}

/* ------------------------------------------------------------------ */
/* drawing                                                            */
/* ------------------------------------------------------------------ */

static void draw_border_line(int y, int cw)
{
    move(y, 0);
    addch(ACS_ULCORNER);
    for (int ci = 0; ci < 3; ci++) {
        for (int j = 0; j < cw; j++)
            addch(ACS_HLINE);
        if (ci < 2)
            addch(ACS_TTEE);
    }
    addch(ACS_URCORNER);
}

static void draw_separator(int y, int cw)
{
    move(y, 0);
    addch(ACS_LTEE);
    for (int ci = 0; ci < 3; ci++) {
        for (int j = 0; j < cw; j++)
            addch(ACS_HLINE);
        if (ci < 2)
            addch(ACS_PLUS);
    }
    addch(ACS_RTEE);
}

static void draw_bottom(int y, int cw)
{
    move(y, 0);
    addch(ACS_LLCORNER);
    for (int ci = 0; ci < 3; ci++) {
        for (int j = 0; j < cw; j++)
            addch(ACS_HLINE);
        if (ci < 2)
            addch(ACS_BTEE);
    }
    addch(ACS_LRCORNER);
}

static void draw_headers(int y, int cw, const Board *board)
{
    int x = 0;
    move(y, x++);
    addch(ACS_VLINE);

    for (int ci = 0; ci < 3; ci++) {
        int pair = PAIR_HEADER_TODO + ci;
        if (has_color) attron(COLOR_PAIR(pair));
        else           attron(A_BOLD);

        const char *label = make_header(column_names[ci],
                                        board->columns[ci].count);
        draw_centered(y, x, label, cw);
        x += cw;

        if (has_color) attroff(COLOR_PAIR(pair));
        else           attroff(A_BOLD);

        addch(ACS_VLINE);
        x++;
    }
}

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

static void draw_status_bar(int rows)
{
    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else           attron(A_REVERSE);

    int x = 0;
    int cols = COLS;

    if (g_flash_until && time(NULL) < g_flash_until) {
        int flen = (int)strlen(g_flash_buf);
        addch(' '); x++;
        for (int i = 0; i < flen && x < cols; i++, x++)
            addch((unsigned char)g_flash_buf[i]);
        for (; x < cols; x++)
            addch(' ');
    } else {
        int job_count = llm_job_count();
        int running_count = 0;
        for (int i = 0; i < job_count; i++) {
            const LlmJob *j = llm_job_at(i);
            if (j && j->state == LLM_RUNNING)
                running_count++;
        }

        char left[64] = "";
        int left_len = 0;
        if (running_count > 0) {
            char sp = g_spinner_chars[g_spinner_idx % 4];
            left_len = snprintf(left, sizeof(left),
                                " %c %d job%s running  ",
                                sp, running_count,
                                running_count == 1 ? "" : "s");
        }

        const char *hint =
            "q quit  |  hjkl navigate  |  a add  e edit  d del  H/L move  C-E enrich";
        int hint_len = (int)strlen(hint);

        int available = cols - left_len;
        if (available < 10) {
            for (int i = 0; i < left_len && x < cols; i++, x++)
                addch((unsigned char)left[i]);
            for (; x < cols; x++)
                addch(' ');
        } else {
            for (int i = 0; i < left_len && x < cols; i++, x++)
                addch((unsigned char)left[i]);

            int hint_pad = available > hint_len
                           ? (available - hint_len) / 2 : 0;
            for (int i = 0; i < hint_pad && x < cols; i++, x++)
                addch(' ');

            for (int i = 0; i < hint_len && x < cols; i++, x++)
                addch((unsigned char)hint[i]);

            for (; x < cols; x++)
                addch(' ');
        }
    }

    if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
    else           attroff(A_REVERSE);
}

/* ---- review screen draw ---- */

static void draw_review_screen(void)
{
    erase();

    int rows = LINES;
    int cols = COLS;

    if (rows < 10 || cols < 40) {
        mvaddstr(rows / 2, (cols - 40) / 2 > 0 ? (cols - 40) / 2 : 0,
                 "Terminal too small for review.");
        return;
    }

    /* Title bar */
    if (has_color) attron(A_REVERSE);
    move(0, 0);
    char title[64];
    snprintf(title, sizeof(title), " AI Enrich - Card #%d  ", g_review_card_id);
    for (int i = 0; i < cols; i++) addch(' ');
    mvaddstr(0, (cols - (int)strlen(title)) / 2, title);
    if (has_color) attroff(A_REVERSE);

    /* Field list */
    int y = 2;
    for (int i = 0; i < g_review_fields; i++) {
        if (y >= rows - 2) break;
        int is_sel = (i == g_review_sel);
        int accepted = g_review_accepted[i];

        if (is_sel) {
            if (has_color) attron(A_REVERSE);
            else attron(A_BOLD);
        }

        /* Checkmark */
        move(y, 2);
        if (accepted)
            addstr("[x] ");
        else
            addstr("[ ] ");

        /* Field content */
        struct review_field *f = &g_review_items[i];
        switch (f->type) {
        case 0: /* description */
            addstr("Description: ");
            {
                int remaining = cols - 20;
                int slen = (int)strlen(f->text);
                if (slen > remaining)
                    slen = remaining - 3;
                for (int j = 0; j < slen; j++)
                    addch((unsigned char)f->text[j]);
                if ((int)strlen(f->text) > remaining)
                    addstr("...");
            }
            break;
        case 1: /* label */
            addstr("Label: ");
            addstr(f->text);
            break;
        case 2: /* question */
            addstr("Q: ");
            {
                int remaining = (cols - 10) / 2;
                int slen = (int)strlen(f->text);
                if (slen > remaining) slen = remaining - 2;
                for (int j = 0; j < slen; j++)
                    addch((unsigned char)f->text[j]);
                if ((int)strlen(f->text) > remaining)
                    addstr("..");
            }
            addstr("  A: ");
            {
                int remaining = (cols - 16) / 2;
                int slen = (int)strlen(f->extra);
                if (slen > remaining) slen = remaining - 2;
                for (int j = 0; j < slen; j++)
                    addch((unsigned char)f->extra[j]);
                if ((int)strlen(f->extra) > remaining)
                    addstr("..");
            }
            break;
        }

        if (is_sel) {
            if (has_color) attroff(A_REVERSE);
            else attroff(A_BOLD);
        }
        y++;
    }

    /* Confirm button */
    y++;
    if (y < rows - 2) {
        int is_sel = (g_review_sel == g_review_fields);
        if (is_sel) {
            if (has_color) attron(A_REVERSE);
            else attron(A_BOLD);
        }
        mvaddstr(y, (cols - 20) / 2, "  [ Confirm & Apply ]  ");
        if (is_sel) {
            if (has_color) attroff(A_REVERSE);
            else attroff(A_BOLD);
        }
    }

    /* Bottom hint */
    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else attron(A_REVERSE);
    const char *hint = " Enter/Space=toggle  j/k=navigate  c=confirm  ESC=discard ";
    for (int i = 0; i < cols; i++) {
        if (i < (int)strlen(hint))
            addch((unsigned char)hint[i]);
        else
            addch(' ');
    }
    if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
    else attroff(A_REVERSE);

    refresh();
}

/* ---- review helpers ---- */

/* Build review fields from an EnrichResult */
static void review_build_fields(const EnrichResult *er)
{
    g_review_fields = 0;

    /* Description (always present, may be NULL) */
    if (er->description) {
        struct review_field *f = &g_review_items[g_review_fields];
        f->type = 0;
        snprintf(f->text, sizeof(f->text), "%s", er->description);
        f->extra[0] = '\0';
        g_review_accepted[g_review_fields] = 1; /* default: accepted */
        g_review_fields++;
    }

    /* Labels */
    for (int i = 0; er->labels && er->labels[i]; i++) {
        if (g_review_fields >= REVIEW_FIELD_MAX) break;
        struct review_field *f = &g_review_items[g_review_fields];
        f->type = 1;
        snprintf(f->text, sizeof(f->text), "%s", er->labels[i]);
        f->extra[0] = '\0';
        g_review_accepted[g_review_fields] = 1;
        g_review_fields++;
    }

    /* Questions */
    for (int i = 0; er->questions && er->questions[i]; i++) {
        if (g_review_fields >= REVIEW_FIELD_MAX) break;
        struct review_field *f = &g_review_items[g_review_fields];
        f->type = 2;
        snprintf(f->text, sizeof(f->text), "%s", er->questions[i]);
        snprintf(f->extra, sizeof(f->extra), "%s",
                 er->answers && er->answers[i] ? er->answers[i] : "");
        g_review_accepted[g_review_fields] = 1;
        g_review_fields++;
    }

    g_review_sel = 0;
}

/* Apply accepted fields to the card */
static void review_apply(Board *board)
{
    EnrichResult er;
    memset(&er, 0, sizeof(er));

    /* Collect accepted fields back into an EnrichResult for application */
    for (int i = 0; i < g_review_fields; i++) {
        if (!g_review_accepted[i]) continue;
        struct review_field *f = &g_review_items[i];
        switch (f->type) {
        case 0: /* description */
            board_set_card_description(board, g_review_card_id, f->text);
            break;
        case 1: /* label */
            board_add_label(board, g_review_card_id, f->text);
            break;
        case 2: /* question — store as Q&A? Not implemented in card model yet.
                   For now, skip. */
            break;
        }
    }

    autosave(board);
}

static void review_cleanup(void)
{
    g_review_mode = 0;
    g_review_card_id = -1;
    g_review_fields = 0;
}

/* ---- review input handler ---- */

static int handle_review_input(Board *board, int ch)
{
    switch (ch) {
    case 'q':  /* fall through: q in review cancels (some users expect this) */
    case 27:   /* ESC — discard all */
        review_cleanup();
        return 1;

    case 'j':
    case KEY_DOWN:
        g_review_sel++;
        if (g_review_sel > g_review_fields)
            g_review_sel = g_review_fields; /* clamp to Confirm button */
        return 1;

    case 'k':
    case KEY_UP:
        if (g_review_sel > 0)
            g_review_sel--;
        return 1;

    case '\n':
    case '\r':
    case KEY_ENTER:
    case ' ':
        if (g_review_sel == g_review_fields) {
            /* Confirm button — apply and exit */
            review_apply(board);
            review_cleanup();
        } else {
            /* Toggle acceptance */
            g_review_accepted[g_review_sel] = !g_review_accepted[g_review_sel];
        }
        return 1;

    case 'c':
    case 'C':
        /* Confirm directly */
        review_apply(board);
        review_cleanup();
        return 1;

    case KEY_RESIZE:
        return 1;

    default:
        return 1;
    }
}

/* ---- main draw (board + status bar) ---- */

static void tui_draw(const Board *board)
{
    if (g_review_mode) {
        draw_review_screen();
        return;
    }

    erase();

    int rows = LINES;
    int cols = COLS;

    if (rows < MIN_ROWS || cols < MIN_COLS) {
        const char *msg = "Terminal too small. Resize to at least "
                          "60 columns x 20 rows.";
        int y = rows / 2;
        int x = (cols - (int)strlen(msg)) / 2;
        if (x < 0) x = 0;
        mvaddstr(y, x, msg);
        return;
    }

    int cw = col_width(cols);

    draw_border_line(0, cw);
    draw_headers(1, cw, board);
    draw_separator(2, cw);

    int card_area_start = 3;
    int card_area_end   = rows - 2;
    int max_rows        = card_area_end - card_area_start;
    if (max_rows < 0) max_rows = 0;

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

    draw_bottom(card_area_start + draw_rows, cw);
    draw_status_bar(rows);

    refresh();
}

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
/* autosave helper                                                    */
/* ------------------------------------------------------------------ */

static void autosave(const Board *board)
{
    if (g_save_path)
        board_save(board, g_save_path);
}

/* ------------------------------------------------------------------ */
/* input mode: inline text editor on the status bar                   */
/* ------------------------------------------------------------------ */

static void draw_input_bar(const char *prompt, const char *buf, int buf_len)
{
    int rows = LINES;
    int cols = COLS;
    int plen = (int)strlen(prompt);

    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else           attron(A_REVERSE);

    int x = 0;
    for (int i = 0; i < plen && x < cols; i++, x++)
        addch((unsigned char)prompt[i]);
    for (int i = 0; i < buf_len && x < cols; i++, x++)
        addch((unsigned char)buf[i]);
    for (; x < cols; x++)
        addch(' ');

    if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
    else           attroff(A_REVERSE);
}

static int input_line(const char *prompt, char *buf, size_t bufsz,
                      const char *initial)
{
    size_t initial_len = initial ? strlen(initial) : 0;
    if (initial_len >= bufsz) initial_len = bufsz - 1;
    memcpy(buf, initial, initial_len);
    buf[initial_len] = '\0';
    int len = (int)initial_len;

    int prompt_len = (int)strlen(prompt);

    curs_set(1);

    int rows = LINES;

    while (1) {
        draw_input_bar(prompt, buf, len);
        move(rows - 1, prompt_len + len);
        refresh();

        int ch = getch();

        if (ch == ERR)
            continue;

        if (ch == 27) {
            curs_set(0);
            return -1;
        }
        if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            curs_set(0);
            return 0;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
            if (len > 0)
                buf[--len] = '\0';
            continue;
        }
        if (ch == KEY_RESIZE) {
            rows = LINES;
            continue;
        }
        if (ch >= 32 && ch <= 126 && len < (int)bufsz - 1) {
            buf[len++] = (char)ch;
            buf[len]   = '\0';
        }
    }
}

/* ------------------------------------------------------------------ */
/* confirm mode: y/n prompt on the status bar                         */
/* ------------------------------------------------------------------ */

static int confirm(const char *prompt)
{
    int rows = LINES;
    int cols = COLS;
    int plen = (int)strlen(prompt);

    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else           attron(A_REVERSE);

    for (int i = 0; i < plen && i < cols; i++)
        addch((unsigned char)prompt[i]);
    for (int i = plen; i < cols; i++)
        addch(' ');

    if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
    else           attroff(A_REVERSE);

    refresh();

    while (1) {
        int ch = getch();
        if (ch == ERR)
            continue;
        if (ch == 'y' || ch == 'Y') return 1;
        if (ch == 'n' || ch == 'N' || ch == 27) return 0;
        if (ch == KEY_RESIZE) {
            rows = LINES;
            cols = COLS;
            plen = (int)strlen(prompt);
            move(rows - 1, 0);
            if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
            else           attron(A_REVERSE);
            for (int i = 0; i < plen && i < cols; i++)
                addch((unsigned char)prompt[i]);
            for (int i = plen; i < cols; i++)
                addch(' ');
            if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
            else           attroff(A_REVERSE);
            refresh();
        }
    }
}

/* ------------------------------------------------------------------ */
/* enrich: submit an enrichment job for a card                        */
/* ------------------------------------------------------------------ */

static void submit_enrich_job(Board *board, int card_id)
{
    Card *card = board_get_card(board, card_id);
    if (!card) return;

    char *prompt = enrich_build_prompt(card->title, card->description);
    if (!prompt) return;

    int job_id = llm_submit(prompt, card_id, 60);
    free(prompt);

    if (job_id >= 0) {
        g_enrich_job_id = job_id;
        snprintf(g_flash_buf, sizeof(g_flash_buf),
                 "Enriching card #%d...", card_id);
        g_flash_until = time(NULL) + FLASH_DURATION;
    }
}

/* ------------------------------------------------------------------ */
/* event handling                                                     */
/* ------------------------------------------------------------------ */

static int handle_input(Board *board, int ch)
{
    /* Ctrl+E: submit enrich for selected card */
    if (ch == 5) { /* Ctrl+E */
        if (state.sel_card >= 0) {
            const Column *col = &board->columns[state.sel_col];
            int card_id = col->cards[state.sel_card].id;
            submit_enrich_job(board, card_id);
        }
        return 1;
    }

    switch (ch) {

    /* ---- quit ---- */
    case 'q':
    case 'Q':
        return 0;

    /* ---- navigation ---- */
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
        if (col->count > 0 && state.sel_card < col->count - 1)
            state.sel_card++;
        return 1;
    }

    case 'k':
    case KEY_UP:
        if (state.sel_card > 0)
            state.sel_card--;
        return 1;

    case KEY_RESIZE:
        return 1;

    /* ---- add card ---- */
    case 'a': {
        char buf[INPUT_MAX + 1] = "";
        if (input_line("Add card: ", buf, sizeof(buf), "") == 0
            && buf[0] != '\0') {
            int new_id = board_add_card(board, state.sel_col, buf);
            if (new_id > 0) {
                state.sel_card =
                    board->columns[state.sel_col].count - 1;
                autosave(board);
                /* Flash hint for Ctrl+E */
                snprintf(g_flash_buf, sizeof(g_flash_buf),
                         "Card #%d added - C-E to enrich with AI", new_id);
                g_flash_until = time(NULL) + FLASH_DURATION;
            }
        }
        return 1;
    }

    /* ---- edit card ---- */
    case 'e': {
        const Column *col = &board->columns[state.sel_col];
        if (state.sel_card >= 0 && state.sel_card < col->count) {
            char buf[INPUT_MAX + 1] = "";
            const char *old_title = col->cards[state.sel_card].title;
            int card_id = col->cards[state.sel_card].id;
            if (input_line("Edit card: ", buf, sizeof(buf), old_title) == 0
                && buf[0] != '\0') {
                board_edit_card_title(board, card_id, buf);
                autosave(board);
            }
        }
        return 1;
    }

    /* ---- delete card ---- */
    case 'd': {
        const Column *col = &board->columns[state.sel_col];
        if (state.sel_card >= 0 && state.sel_card < col->count) {
            if (confirm("Delete card? (y/n) ")) {
                int card_id = col->cards[state.sel_card].id;
                board_delete_card(board, card_id);
                clamp_selection(board);
                autosave(board);
            }
        }
        return 1;
    }

    /* ---- move card left (shift-h or <) ---- */
    case 'H':
    case '<':
        if (state.sel_card >= 0 && state.sel_col > 0) {
            int card_id =
                board->columns[state.sel_col]
                    .cards[state.sel_card].id;
            int dest = state.sel_col - 1;
            if (board_move_card(board, card_id, dest) == 0) {
                state.sel_col  = dest;
                state.sel_card = board->columns[dest].count - 1;
                autosave(board);
            }
        }
        return 1;

    /* ---- move card right (shift-l or >) ---- */
    case 'L':
    case '>':
        if (state.sel_card >= 0 && state.sel_col < 2) {
            int card_id =
                board->columns[state.sel_col]
                    .cards[state.sel_card].id;
            int dest = state.sel_col + 1;
            if (board_move_card(board, card_id, dest) == 0) {
                state.sel_col  = dest;
                state.sel_card = board->columns[dest].count - 1;
                autosave(board);
            }
        }
        return 1;

    default:
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* check enrich job completion and transition to review               */
/* ------------------------------------------------------------------ */

static void check_enrich_completion(void)
{
    (void)g_board;  /* may be used in future */

    if (g_enrich_job_id < 0) return;

    const LlmJob *job = llm_get_job(g_enrich_job_id);
    if (!job) {
        g_enrich_job_id = -1;
        return;
    }

    if (job->state == LLM_FAILED || job->state == LLM_CANCELLED) {
        snprintf(g_flash_buf, sizeof(g_flash_buf),
                 "Enrich job failed");
        g_flash_until = time(NULL) + FLASH_DURATION;
        g_enrich_job_id = -1;
        return;
    }

    if (job->state != LLM_DONE) return;

    /* Job completed — parse result and enter review mode */
    g_enrich_job_id = -1;

    if (!job->result) {
        snprintf(g_flash_buf, sizeof(g_flash_buf),
                 "Enrich job returned empty result");
        g_flash_until = time(NULL) + FLASH_DURATION;
        return;
    }

    char *inner = enrich_unwrap_envelope(job->result);
    if (!inner) {
        snprintf(g_flash_buf, sizeof(g_flash_buf),
                 "Failed to unwrap enrich result");
        g_flash_until = time(NULL) + FLASH_DURATION;
        return;
    }

    EnrichResult er;
    if (enrich_parse_result(inner, &er) != 0) {
        snprintf(g_flash_buf, sizeof(g_flash_buf),
                 "Failed to parse enrich result");
        g_flash_until = time(NULL) + FLASH_DURATION;
        free(inner);
        return;
    }
    free(inner);

    /* Enter review mode */
    g_review_mode = 1;
    g_review_card_id = job->card_id;
    review_build_fields(&er);
    enrich_free_result(&er);
}

/* ------------------------------------------------------------------ */
/* public api                                                         */
/* ------------------------------------------------------------------ */

int tui_run(Board *board, const char *save_path)
{
    if (!board) return -1;

    g_save_path = save_path;
    g_board = board;

    state.sel_col  = 0;
    state.sel_card = board->columns[0].count > 0 ? 0 : -1;

    /* flash message helper */
    #define SET_FLASH(fmt, ...) do { \
        snprintf(g_flash_buf, sizeof(g_flash_buf), fmt, ##__VA_ARGS__); \
        g_flash_until = time(NULL) + FLASH_DURATION; \
    } while(0)

    tui_init();

    timeout(100);

    tui_draw(board);

    int running  = 1;
    int dirty    = 0;
    int prev_job_count = llm_job_count();

    while (running) {
        int ch = getch();

        if (ch != ERR) {
            if (g_review_mode) {
                running = handle_review_input(board, ch);
            } else {
                running = handle_input(board, ch);
            }
            dirty = 1;
        }

        /* Poll LLM jobs — non-blocking */
        int transitions = llm_poll();
        if (transitions > 0) {
            int job_count = llm_job_count();
            for (int i = 0; i < job_count; i++) {
                const LlmJob *j = llm_job_at(i);
                if (!j) continue;
                if (j->state == LLM_DONE) {
                    SET_FLASH("Job #%d done (card #%d)",
                              j->id, j->card_id);
                } else if (j->state == LLM_FAILED) {
                    SET_FLASH("Job #%d failed", j->id);
                } else if (j->state == LLM_CANCELLED) {
                    SET_FLASH("Job #%d cancelled", j->id);
                }
            }

            /* Check for enrich job completion */
            check_enrich_completion();

            dirty = 1;
        }

        /* Advance spinner if jobs are running */
        {
            int running_count = 0;
            int job_count = llm_job_count();
            for (int i = 0; i < job_count; i++) {
                const LlmJob *j = llm_job_at(i);
                if (j && j->state == LLM_RUNNING)
                    running_count++;
            }
            if (running_count > 0) {
                g_spinner_idx++;
                dirty = 1;
            }
        }

        /* Detect job count change */
        {
            int cur = llm_job_count();
            if (cur != prev_job_count) {
                dirty = 1;
                prev_job_count = cur;
            }
        }

        /* Expire flash message */
        if (g_flash_until && time(NULL) >= g_flash_until) {
            g_flash_until = 0;
            dirty = 1;
        }

        if (dirty) {
            tui_draw(board);
            dirty = 0;
        }
    }

    #undef SET_FLASH

    tui_shutdown();

    if (save_path)
        board_save(board, save_path);

    return 0;
}
