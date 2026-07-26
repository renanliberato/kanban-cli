#include "tui.h"
#include "llm.h"
#include "enrich.h"
#include "db.h"
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
#define FILTER_MAX          128
#define LABEL_PICKER_MAX    64   /* max labels in picker */
#define DETAIL_MAX_WIDTH    80   /* max width for description wrapping */

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

/* detail view state */
static int  g_detail_mode   = 0;
static int  g_detail_card_id = -1;
static int  g_detail_scroll  = 0;    /* scroll offset for description */

/* label picker state */
static int  g_label_picker_mode = 0;
static int  g_label_picker_card = -1;
static int  g_label_picker_field_count = 0;
static int  g_label_picker_sel = 0;
static char g_label_picker_labels[LABEL_PICKER_MAX][64];
static int  g_label_picker_toggled[LABEL_PICKER_MAX];

/* filter state */
static char g_filter_buf[FILTER_MAX];
static int  g_filter_len = 0;
static int  g_filter_active = 0;  /* 0=no filter, 1=entering filter, 2=active filter */

/* color-pair ids */
enum {
    PAIR_HEADER_TODO = 1,
    PAIR_HEADER_DOING,
    PAIR_HEADER_DONE,
    PAIR_SELECTED,
    PAIR_STATUSBAR,
    PAIR_REVIEW_ACCEPTED,
    PAIR_REVIEW_REJECTED,
    /* label tag colors: pair ids 8..15 */
    PAIR_LABEL_0 = 8,
    PAIR_LABEL_1,
    PAIR_LABEL_2,
    PAIR_LABEL_3,
    PAIR_LABEL_4,
    PAIR_LABEL_5,
    PAIR_LABEL_6,
    PAIR_LABEL_7,
    PAIR_LABEL_COUNT = 8
};

static const char *column_names[] = { "To Do", "Doing", "Done" };
static const short header_bg[]    = { COLOR_BLUE, COLOR_YELLOW, COLOR_GREEN };

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* Deterministic label -> color map via simple hash.
   No config in v3; add configurable palette in future release. */
static int label_color(const char *label)
{
    if (!label) return 0;
    unsigned int h = 5381;
    for (const char *p = label; *p; p++)
        h = ((h << 5) + h) + (unsigned char)*p;  /* djb2 */
    return (int)(h % PAIR_LABEL_COUNT);
}

/* Draw a label tag like [bug] with the color derived from the label name.
   Returns number of columns consumed. */
static int draw_label_tag(int y, int x, const char *label)
{
    if (!label) return 0;
    int len = (int)strlen(label);
    int tagw = len + 2;  /* [label] */

    int color = label_color(label);
    if (has_color) attron(COLOR_PAIR(PAIR_LABEL_0 + color));

    move(y, x);
    addch('[');
    for (int i = 0; i < len; i++)
        addch((unsigned char)label[i]);
    addch(']');

    if (has_color) attroff(COLOR_PAIR(PAIR_LABEL_0 + color));

    return tagw;
}

/* Check if a card matches the active filter.
   Returns 1 if visible, 0 if hidden. */
static int card_matches_filter(const Board *board, const Card *card)
{
    if (g_filter_len == 0) return 1;

    /* gather searchable strings: title + description + labels */
    const char *strings[64];
    int n = 0;
    strings[n++] = card->title ? card->title : "";
    if (card->description && card->description[0])
        strings[n++] = card->description;
    for (int li = 0; li < card->label_count && n < 63; li++) {
        if (card->labels[li])
            strings[n++] = card->labels[li];
    }
    (void)board;  /* board can be used for future expansion */

    return fuzzy_match(g_filter_buf, n, strings);
}

/* Count visible cards in a column under current filter */
static int col_visible_count(const Board *board, int ci)
{
    const Column *col = &board->columns[ci];
    int visible = 0;
    for (int i = 0; i < col->count; i++) {
        if (card_matches_filter(board, &col->cards[i]))
            visible++;
    }
    return visible;
}

/* Find the nth visible card index in a column (0-based), return -1 if not found */
static int col_visible_index(const Board *board, int ci, int n)
{
    const Column *col = &board->columns[ci];
    int seen = 0;
    for (int i = 0; i < col->count; i++) {
        if (card_matches_filter(board, &col->cards[i])) {
            if (seen == n) return i;
            seen++;
        }
    }
    return -1;
}

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

static const char *make_header(const Board *board, int col_idx)
{
    static char buf[64];
    const char *name = column_names[col_idx];
    int total = board->columns[col_idx].count;

    if (g_filter_len > 0) {
        int shown = col_visible_count(board, col_idx);
        int n = snprintf(buf, sizeof(buf), "%s (%d/%d)", name, shown, total);
        if (n < 0) return name;
    } else {
        int n = snprintf(buf, sizeof(buf), "%s (%d)", name, total);
        if (n < 0) return name;
    }
    return buf;
}

static void draw_card(int y, int x, const char *title, int width, int selected,
                      const Card *card)
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

    /* Compute available space for title + labels */
    int tags_space = 0;
    if (card && card->label_count > 0) {
        /* estimate: each tag is [name] + space = len(name)+3 */
        for (int li = 0; li < card->label_count; li++) {
            int tlen = card->labels[li] ? (int)strlen(card->labels[li]) : 0;
            tags_space += tlen + 3;  /* [x] + space */
        }
    }
    int title_space = usable - tags_space;
    if (title_space < 2) title_space = 2;

    if (len <= title_space) {
        for (int i = 0; i < len; i++)
            addch((unsigned char)title[i]);
        int pad = title_space - len;
        for (int i = 0; i < pad; i++)
            addch(' ');
    } else {
        int show = title_space - 1;
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

    /* Draw label tags (after title, in the remaining space, without selection highlight) */
    if (card && card->label_count > 0 && tags_space > 0) {
        int cx = x + 1 + title_space;
        if (cx < x + width) {
            int remaining = x + width - cx;
            for (int li = 0; li < card->label_count && remaining > 3; li++) {
                const char *lname = card->labels[li];
                if (!lname || !lname[0]) continue;
                int tlen = (int)strlen(lname);
                if (tlen + 3 > remaining) {
                    /* truncated tag */
                    int show = remaining - 3;
                    if (show < 1) break;
                    draw_label_tag(y, cx, lname);
                    cx += remaining;
                    break;
                }
                draw_label_tag(y, cx, lname);
                cx += tlen + 3;
                remaining -= (tlen + 3);
                /* add a space between tags */
                move(y, cx);
                addch(' ');
                cx++;
                remaining--;
            }
        }
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

        /* Label tag colors: deterministic palette (hash mod 8).
           No config in v3 — note for future: add configurable palette. */
        init_pair(PAIR_LABEL_0, COLOR_BLACK, COLOR_CYAN);
        init_pair(PAIR_LABEL_1, COLOR_BLACK, COLOR_GREEN);
        init_pair(PAIR_LABEL_2, COLOR_BLACK, COLOR_YELLOW);
        init_pair(PAIR_LABEL_3, COLOR_BLACK, COLOR_MAGENTA);
        init_pair(PAIR_LABEL_4, COLOR_WHITE, COLOR_BLUE);
        init_pair(PAIR_LABEL_5, COLOR_WHITE, COLOR_RED);
        init_pair(PAIR_LABEL_6, COLOR_BLACK, COLOR_WHITE);
        init_pair(PAIR_LABEL_7, COLOR_WHITE, COLOR_BLACK);
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

        const char *label = make_header(board, ci);
        draw_centered(y, x, label, cw);
        x += cw;

        if (has_color) attroff(COLOR_PAIR(pair));
        else           attroff(A_BOLD);

        addch(ACS_VLINE);
        x++;
    }
}

/* Draw one row of the card grid. board and row_idx are the canonical
   (unfiltered) board indices. If filtering is active, we walk through
   columns showing only visible cards, padding with blanks. */
static void draw_card_row(int y, int cw, const Board *board, int row_idx,
                          int first_col_visible[3], int col_visible[3])
{
    (void)first_col_visible;  /* reserved for future use */
    int x = 0;
    move(y, x++);
    addch(ACS_VLINE);

    for (int ci = 0; ci < 3; ci++) {
        const Column *col = &board->columns[ci];
        int vis_idx = row_idx;
        int real_idx = col_visible_index(board, ci, vis_idx);
        int is_selected = 0;
        const Card *card = NULL;
        const char *title = "";

        if (real_idx >= 0) {
            card = &col->cards[real_idx];
            title = card->title;
            /* selection: compare against the visible index */
            if (ci == state.sel_col) {
                int active_count = col_visible[ci];
                if (active_count > 0) {
                    int sel_visible_idx = state.sel_card;
                    if (sel_visible_idx < 0) sel_visible_idx = 0;
                    if (sel_visible_idx >= active_count) sel_visible_idx = active_count - 1;
                    if (vis_idx == sel_visible_idx)
                        is_selected = 1;
                }
            }
        }

        draw_card(y, x, title, cw, is_selected, card);
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

        char left[128] = "";
        int left_len = 0;
        if (running_count > 0) {
            char sp = g_spinner_chars[g_spinner_idx % 4];
            left_len = snprintf(left, sizeof(left),
                                " %c %d job%s running  ",
                                sp, running_count,
                                running_count == 1 ? "" : "s");
        }

        /* show filter indicator if active */
        if (g_filter_len > 0) {
            int n = snprintf(left + left_len, sizeof(left) - (size_t)left_len,
                             "filter: %s  ", g_filter_buf);
            if (n > 0) left_len += n;
        }

        const char *hint =
            "q quit  |  hjkl navigate  |  a add  e edit  d del  H/L move  C-E enrich  / filter  # labels  Enter detail";
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

/* ---- label picker draw ---- */

static void draw_label_picker(void)
{
    int rows = LINES;
    int cols = COLS;

    if (rows < 8 || cols < 30) {
        mvaddstr(rows / 2, (cols - 30) / 2 > 0 ? (cols - 30) / 2 : 0,
                 "Terminal too small for label picker.");
        return;
    }

    /* Title bar */
    if (has_color) attron(A_REVERSE);
    move(0, 0);
    char title[64];
    snprintf(title, sizeof(title), " Labels - Card #%d  ", g_label_picker_card);
    for (int i = 0; i < cols; i++) addch(' ');
    mvaddstr(0, (cols - (int)strlen(title)) / 2 > 0 ? (cols - (int)strlen(title)) / 2 : 0, title);
    if (has_color) attroff(A_REVERSE);

    int y = 2;
    mvaddstr(y, 2, "Toggle labels for this card (Space/Enter):");
    y += 2;

    for (int i = 0; i < g_label_picker_field_count && y < rows - 3; i++) {
        if (i == g_label_picker_sel) {
            if (has_color) attron(A_REVERSE);
            else attron(A_BOLD);
        }

        move(y, 4);
        if (g_label_picker_toggled[i])
            addstr("[x] ");
        else
            addstr("[ ] ");

        draw_label_tag(y, 10, g_label_picker_labels[i]);

        if (i == g_label_picker_sel) {
            if (has_color) attroff(A_REVERSE);
            else attroff(A_BOLD);
        }
        y++;
    }

    /* "New label" option */
    if (y < rows - 3) {
        int is_sel = (g_label_picker_sel == g_label_picker_field_count);
        if (is_sel) {
            if (has_color) attron(A_REVERSE);
            else attron(A_BOLD);
        }
        mvaddstr(y, 4, "[+] New label...");
        if (is_sel) {
            if (has_color) attroff(A_REVERSE);
            else attroff(A_BOLD);
        }
    }

    /* Bottom hint */
    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else attron(A_REVERSE);
    const char *hint = " Space/Enter=toggle  j/k=navigate  n=new label  ESC=done ";
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

/* ---- detail view draw ---- */

static void draw_detail_view(const Board *board)
{
    erase();

    int rows = LINES;
    int cols = COLS;

    if (rows < 10 || cols < 40) {
        mvaddstr(rows / 2, (cols - 40) / 2 > 0 ? (cols - 40) / 2 : 0,
                 "Terminal too small for detail view.");
        return;
    }

    Card *card = board_get_card((Board *)board, g_detail_card_id);
    if (!card) {
        mvaddstr(rows / 2, (cols - 20) / 2, "Card not found.");
        return;
    }

    /* Title bar */
    if (has_color) attron(A_REVERSE);
    move(0, 0);
    char title[80];
    int col_idx = -1;
    for (int ci = 0; ci < 3; ci++) {
        for (int i = 0; i < board->columns[ci].count; i++) {
            if (board->columns[ci].cards[i].id == g_detail_card_id) {
                col_idx = ci;
                break;
            }
        }
        if (col_idx >= 0) break;
    }
    snprintf(title, sizeof(title), " Card #%d — %s  ",
             g_detail_card_id,
             col_idx >= 0 ? column_names[col_idx] : "?");
    for (int i = 0; i < cols; i++) addch(' ');
    mvaddstr(0, (cols - (int)strlen(title)) / 2 > 0 ? (cols - (int)strlen(title)) / 2 : 0, title);
    if (has_color) attroff(A_REVERSE);

    /* Title line */
    int y = 2;
    if (has_color) attron(A_BOLD);
    mvaddstr(y, 2, "Title: ");
    if (has_color) attroff(A_BOLD);
    {
        int remaining = cols - 10;
        int slen = (int)strlen(card->title);
        if (slen > remaining) slen = remaining - 1;
        for (int j = 0; j < slen; j++)
            addch((unsigned char)card->title[j]);
        if ((int)strlen(card->title) > remaining)
            addch('~');
    }
    y += 2;

    /* Labels row */
    if (card->label_count > 0) {
        mvaddstr(y, 2, "Labels: ");
        int cx = 10;
        for (int li = 0; li < card->label_count && cx < cols - 2; li++) {
            const char *ln = card->labels[li];
            if (!ln) continue;
            int tlen = (int)strlen(ln);
            if (cx + tlen + 3 > cols - 2) break;
            move(y, cx);
            draw_label_tag(y, cx, ln);
            cx += tlen + 3;
            move(y, cx);
            addch(' ');
            cx++;
        }
        y += 2;
    } else {
        mvaddstr(y, 2, "Labels: (none)");
        y += 2;
    }

    /* Description (scrollable) */
    y++;
    mvaddstr(y, 2, "Description:");
    y++;
    if (card->description && card->description[0]) {
        int max_view_lines = rows - y - 6;
        if (max_view_lines < 0) max_view_lines = 0;

        /* Simple word-wrap: wrap at cols-4 */
        int wrap_width = cols - 4;
        if (wrap_width < 10) wrap_width = 10;
        const char *desc = card->description;
        int desc_len = (int)strlen(desc);

        /* Collect wrapped lines */
        static char wrap_lines[512][128];
        int wrap_count = 0;
        int pos = 0;

        while (pos < desc_len && wrap_count < 500) {
            int line_len = 0;
            int line_start = pos;

            /* skip leading whitespace on new logical line */
            while (pos < desc_len && desc[pos] == '\n') pos++;

            /* find break point */
            while (pos < desc_len && desc[pos] != '\n' && line_len < wrap_width) {
                line_len++;
                pos++;
            }

            if (line_len > 0) {
                if (line_len > (int)sizeof(wrap_lines[0]) - 1)
                    line_len = (int)sizeof(wrap_lines[0]) - 1;
                memcpy(wrap_lines[wrap_count], desc + line_start, (size_t)line_len);
                wrap_lines[wrap_count][line_len] = '\0';
                wrap_count++;
            }

            /* consume the newline if we stopped on one */
            if (pos < desc_len && desc[pos] == '\n') pos++;
        }

        if (wrap_count == 0) {
            int slen = desc_len > wrap_width ? wrap_width : desc_len;
            if (slen > (int)sizeof(wrap_lines[0]) - 1)
                slen = (int)sizeof(wrap_lines[0]) - 1;
            memcpy(wrap_lines[0], desc, (size_t)slen);
            wrap_lines[0][slen] = '\0';
            wrap_count = 1;
        }

        /* Clamp scroll offset */
        if (g_detail_scroll < 0) g_detail_scroll = 0;
        if (g_detail_scroll > wrap_count - max_view_lines)
            g_detail_scroll = wrap_count - max_view_lines;
        if (g_detail_scroll < 0) g_detail_scroll = 0;

        for (int i = g_detail_scroll; i < wrap_count && (i - g_detail_scroll) < max_view_lines; i++) {
            mvaddstr(y, 4, wrap_lines[i]);
            y++;
        }

        /* Scroll indicator */
        if (wrap_count > max_view_lines) {
            move(rows - 2, cols - 20);
            addstr("(j/k scroll) ");
            int pct = (wrap_count > 0) ? (g_detail_scroll * 100 / (wrap_count - max_view_lines)) : 0;
            if (pct > 100) pct = 100;
            char pctbuf[16];
            snprintf(pctbuf, sizeof(pctbuf), "%d%%", pct);
            addstr(pctbuf);
        }
    } else {
        mvaddstr(y, 4, "No description -- press D to add, C-E to enrich");
        y++;
    }

    /* Q&A placeholder */
    y += 2;
    {
        if (has_color) attron(A_BOLD);
        mvaddstr(y, 2, "Q&A:");
        if (has_color) attroff(A_BOLD);
        y++;
        mvaddstr(y, 4, "No Q&A yet.  (coming in M6)");
        y++;
    }

    /* Timestamps */
    y += 1;
    if (card->created_at) {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "Created: %s", card->created_at);
        mvaddstr(y, 2, tbuf);
        y++;
    }
    if (card->updated_at) {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "Updated: %s", card->updated_at);
        mvaddstr(y, 2, tbuf);
        y++;
    }

    /* Card ID line */
    {
        char idbuf[32];
        snprintf(idbuf, sizeof(idbuf), "Card ID: %d", g_detail_card_id);
        mvaddstr(y, 2, idbuf);
    }

    /* Bottom hint */
    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else attron(A_REVERSE);
    const char *hint = " ESC/q=back  t=edit title  D=edit desc  l=labels  j/k/PgUp/PgDn=scroll ";
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

/* ---- filter bar draw ---- */

static void draw_filter_bar(void)
{
    int rows = LINES;
    int cols = COLS;

    move(rows - 1, 0);
    if (has_color) attron(COLOR_PAIR(PAIR_STATUSBAR));
    else attron(A_REVERSE);

    const char *prompt = "Filter: ";
    int plen = (int)strlen(prompt);
    int x = 0;
    for (int i = 0; i < plen && x < cols; i++, x++)
        addch((unsigned char)prompt[i]);
    for (int i = 0; i < g_filter_len && x < cols; i++, x++)
        addch((unsigned char)g_filter_buf[i]);

    /* cursor position */
    if (x < cols) {
        curs_set(1);
        move(rows - 1, x);
    }

    for (; x < cols; x++)
        addch(' ');

    if (has_color) attroff(COLOR_PAIR(PAIR_STATUSBAR));
    else attroff(A_REVERSE);

    refresh();
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

    if (g_detail_mode) {
        draw_detail_view(board);
        return;
    }

    if (g_label_picker_mode) {
        draw_label_picker();
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

    /* Compute visible counts per column for filtered view */
    int col_visible[3];
    int first_col_visible[3];
    for (int ci = 0; ci < 3; ci++) {
        col_visible[ci] = col_visible_count(board, ci);
        first_col_visible[ci] = 0;
    }

    int max_cards = 0;
    for (int ci = 0; ci < 3; ci++) {
        if (col_visible[ci] > max_cards)
            max_cards = col_visible[ci];
    }
    int draw_rows = max_cards > max_rows ? max_cards : max_rows;
    if (draw_rows < 1) draw_rows = 1;

    for (int r = 0; r < draw_rows; r++) {
        draw_card_row(card_area_start + r, cw, board, r,
                      first_col_visible, col_visible);
    }

    draw_bottom(card_area_start + draw_rows, cw);

    /* "no matches" hints in affected columns */
    if (g_filter_len > 0) {
        int hint_y = card_area_start + draw_rows + 1;
        if (hint_y < rows - 2) {
            int x = 0;
            move(hint_y, x++);
            addch(ACS_VLINE);
            for (int ci = 0; ci < 3; ci++) {
                if (col_visible[ci] == 0 && board->columns[ci].count > 0) {
                    const char *nomsg = "(no matches)";
                    int slen = (int)strlen(nomsg);
                    int pad = (cw - slen) / 2;
                    if (pad < 0) pad = 0;
                    for (int i = 0; i < pad; i++) addch(' ');
                    for (int i = 0; i < slen && pad + i < cw; i++)
                        addch((unsigned char)nomsg[i]);
                    for (int i = pad + slen; i < cw; i++) addch(' ');
                } else {
                    for (int i = 0; i < cw; i++) addch(' ');
                }
                addch(ACS_VLINE);
            }
        }
    }

    if (g_filter_active == 1) {
        draw_filter_bar();
    } else {
        curs_set(0);
        draw_status_bar(rows);
    }

    refresh();
}

static void clamp_selection(const Board *board)
{
    if (state.sel_col < 0) state.sel_col = 0;
    if (state.sel_col > 2) state.sel_col = 2;

    int visible = col_visible_count(board, state.sel_col);
    if (visible == 0) {
        state.sel_card = -1;
    } else {
        if (state.sel_card < 0) state.sel_card = 0;
        if (state.sel_card >= visible) state.sel_card = visible - 1;
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

/* ---- label picker management ---- */

static void label_picker_init(Board *board, int card_id)
{
    g_label_picker_mode = 1;
    g_label_picker_card = card_id;
    g_label_picker_field_count = 0;
    g_label_picker_sel = 0;

    /* Get all board labels */
    char **all_labels = NULL;
    int all_count = 0;
    board_get_all_labels(board, &all_labels, &all_count);

    /* Get this card's current labels */
    Card *card = board_get_card(board, card_id);

    for (int i = 0; i < all_count && g_label_picker_field_count < LABEL_PICKER_MAX; i++) {
        if (!all_labels[i]) continue;
        snprintf(g_label_picker_labels[g_label_picker_field_count],
                 sizeof(g_label_picker_labels[0]), "%s", all_labels[i]);

        /* Check if card has this label */
        int has = 0;
        if (card) {
            for (int li = 0; li < card->label_count; li++) {
                if (card->labels[li] && strcmp(card->labels[li], all_labels[i]) == 0) {
                    has = 1;
                    break;
                }
            }
        }
        g_label_picker_toggled[g_label_picker_field_count] = has;
        g_label_picker_field_count++;
    }

    /* Free the all_labels array */
    for (int i = 0; i < all_count; i++) free(all_labels[i]);
    free(all_labels);
}

static void label_picker_cleanup(void)
{
    g_label_picker_mode = 0;
    g_label_picker_card = -1;
    g_label_picker_field_count = 0;
}

static int handle_label_picker_input(Board *board, int ch)
{
    switch (ch) {
    case 27:  /* ESC */
        label_picker_cleanup();
        return 1;

    case 'j':
    case KEY_DOWN:
        g_label_picker_sel++;
        if (g_label_picker_sel > g_label_picker_field_count)
            g_label_picker_sel = g_label_picker_field_count;
        return 1;

    case 'k':
    case KEY_UP:
        if (g_label_picker_sel > 0)
            g_label_picker_sel--;
        return 1;

    case '\n':
    case '\r':
    case KEY_ENTER:
    case ' ':
        if (g_label_picker_sel == g_label_picker_field_count) {
            /* "New label" option */
            char buf[INPUT_MAX + 1] = "";
            if (input_line("New label: ", buf, sizeof(buf), "") == 0
                && buf[0] != '\0') {
                board_add_label(board, g_label_picker_card, buf);
                autosave(board);
                /* re-init picker with the new label */
                label_picker_cleanup();
                label_picker_init(board, g_label_picker_card);
            }
        } else {
            /* Toggle label */
            const char *lname = g_label_picker_labels[g_label_picker_sel];
            if (g_label_picker_toggled[g_label_picker_sel]) {
                board_remove_label(board, g_label_picker_card, lname);
                g_label_picker_toggled[g_label_picker_sel] = 0;
            } else {
                board_add_label(board, g_label_picker_card, lname);
                g_label_picker_toggled[g_label_picker_sel] = 1;
            }
            autosave(board);
        }
        return 1;

    case 'n':
    case 'N': {
        /* shortcut for new label */
        char buf[INPUT_MAX + 1] = "";
        if (input_line("New label: ", buf, sizeof(buf), "") == 0
            && buf[0] != '\0') {
            board_add_label(board, g_label_picker_card, buf);
            autosave(board);
            label_picker_cleanup();
            label_picker_init(board, g_label_picker_card);
        }
        return 1;
    }

    case KEY_RESIZE:
        return 1;

    default:
        return 1;
    }
}

/* ---- detail view input handler ---- */

static int handle_detail_input(Board *board, int ch)
{
    switch (ch) {
    case 27:  /* ESC */
    case 'q':
    case 'Q':
        g_detail_mode = 0;
        g_detail_card_id = -1;
        g_detail_scroll = 0;
        return 1;

    case 't':
    case 'T': {
        Card *card = board_get_card(board, g_detail_card_id);
        if (card) {
            char buf[INPUT_MAX + 1] = "";
            if (input_line("Edit title: ", buf, sizeof(buf), card->title) == 0
                && buf[0] != '\0') {
                board_edit_card_title(board, g_detail_card_id, buf);
                autosave(board);
            }
        }
        return 1;
    }

    case 'D': {
        Card *card = board_get_card(board, g_detail_card_id);
        if (card) {
            char buf[INPUT_MAX + 1] = "";
            const char *initial = card->description ? card->description : "";
            if (input_line("Edit desc: ", buf, sizeof(buf), initial) == 0
                && buf[0] != '\0') {
                board_set_card_description(board, g_detail_card_id, buf);
                autosave(board);
            }
        }
        return 1;
    }

    case 'l':
    case 'L':
        label_picker_init(board, g_detail_card_id);
        return 1;

    case 'j':
    case KEY_DOWN:
        g_detail_scroll++;
        return 1;

    case 'k':
    case KEY_UP:
        if (g_detail_scroll > 0)
            g_detail_scroll--;
        return 1;

    case KEY_NPAGE:  /* PgDn */
        g_detail_scroll += 10;
        return 1;

    case KEY_PPAGE:  /* PgUp */
        g_detail_scroll -= 10;
        if (g_detail_scroll < 0)
            g_detail_scroll = 0;
        return 1;

    case 5:  /* Ctrl+E */
        submit_enrich_job(board, g_detail_card_id);
        return 1;

    case KEY_RESIZE:
        return 1;

    default:
        return 1;
    }
}

/* ---- filter input handler ---- */

static void filter_clear(void)
{
    g_filter_buf[0] = '\0';
    g_filter_len = 0;
    g_filter_active = 0;
}

static int handle_filter_input(Board *board, int ch)
{
    (void)board;
    switch (ch) {
    case 27:  /* ESC — clear filter and exit */
        filter_clear();
        return 1;

    case '\n':
    case '\r':
    case KEY_ENTER:
        /* Keep filter active, return to nav */
        g_filter_active = 2;
        return 1;

    case KEY_BACKSPACE:
    case 127:
    case '\b':
        if (g_filter_len > 0)
            g_filter_buf[--g_filter_len] = '\0';
        return 1;

    case KEY_RESIZE:
        return 1;

    default:
        if (ch >= 32 && ch <= 126 && g_filter_len < FILTER_MAX - 1) {
            g_filter_buf[g_filter_len++] = (char)ch;
            g_filter_buf[g_filter_len] = '\0';
        }
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* event handling                                                     */
/* ------------------------------------------------------------------ */

/* Get the real array index and card ID of the currently selected
   visible card. Returns 0 on success, -1 if no card selected. */
static int get_selected_card(const Board *board, int *id_out)
{
    if (state.sel_card < 0) return -1;
    int vis_idx = state.sel_card;
    int real_idx = col_visible_index(board, state.sel_col, vis_idx);
    if (real_idx < 0) return -1;
    if (id_out)
        *id_out = board->columns[state.sel_col].cards[real_idx].id;
    return 0;
}

/* ------------------------------------------------------------------ */

static int handle_input(Board *board, int ch)
{
    /* Ctrl+E: submit enrich for selected card */
    if (ch == 5) { /* Ctrl+E */
        int card_id;
        if (get_selected_card(board, &card_id) == 0) {
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
        int visible = col_visible_count(board, state.sel_col);
        if (visible > 0 && state.sel_card < visible - 1)
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

    /* ---- detail view ---- */
    case '\n':
    case '\r':
    case KEY_ENTER: {
        int card_id;
        if (get_selected_card(board, &card_id) == 0) {
            g_detail_mode = 1;
            g_detail_card_id = card_id;
            g_detail_scroll = 0;
        }
        return 1;
    }

    /* ---- filter ---- */
    case '/':
        g_filter_active = 1;
        g_filter_buf[0] = '\0';
        g_filter_len = 0;
        return 1;

    /* ---- label picker (board view) ---- */
    case '#': {
        int card_id;
        if (get_selected_card(board, &card_id) == 0) {
            label_picker_init(board, card_id);
        }
        return 1;
    }

    /* ---- add card ---- */
    case 'a': {
        char buf[INPUT_MAX + 1] = "";
        if (input_line("Add card: ", buf, sizeof(buf), "") == 0
            && buf[0] != '\0') {
            int new_id = board_add_card(board, state.sel_col, buf);
            if (new_id > 0) {
                if (g_filter_len > 0) {
                    /* New card added while filtering: clear filter with flash */
                    filter_clear();
                    snprintf(g_flash_buf, sizeof(g_flash_buf),
                             "Card #%d added — filter cleared", new_id);
                    g_flash_until = time(NULL) + FLASH_DURATION;
                }
                state.sel_card = col_visible_count(board, state.sel_col) - 1;
                if (state.sel_card < 0) state.sel_card = 0;
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
        int card_id;
        if (get_selected_card(board, &card_id) == 0) {
            Card *card = board_get_card(board, card_id);
            if (card) {
                char buf[INPUT_MAX + 1] = "";
                if (input_line("Edit card: ", buf, sizeof(buf), card->title) == 0
                    && buf[0] != '\0') {
                    board_edit_card_title(board, card_id, buf);
                    autosave(board);
                }
            }
        }
        return 1;
    }

    /* ---- delete card ---- */
    case 'd': {
        int card_id;
        if (get_selected_card(board, &card_id) == 0) {
            if (confirm("Delete card? (y/n) ")) {
                board_delete_card(board, card_id);
                clamp_selection(board);
                autosave(board);
            }
        }
        return 1;
    }

    /* ---- move card left (shift-h or <) ---- */
    case 'H':
    case '<': {
        int card_id;
        if (get_selected_card(board, &card_id) == 0 && state.sel_col > 0) {
            int dest = state.sel_col - 1;
            if (board_move_card(board, card_id, dest) == 0) {
                state.sel_col  = dest;
                int visible = col_visible_count(board, dest);
                state.sel_card = visible > 0 ? visible - 1 : -1;
                autosave(board);
            }
        }
        return 1;
    }

    /* ---- move card right (shift-l or >) ---- */
    case 'L':
    case '>': {
        int card_id;
        if (get_selected_card(board, &card_id) == 0 && state.sel_col < 2) {
            int dest = state.sel_col + 1;
            if (board_move_card(board, card_id, dest) == 0) {
                state.sel_col  = dest;
                int visible = col_visible_count(board, dest);
                state.sel_card = visible > 0 ? visible - 1 : -1;
                autosave(board);
            }
        }
        return 1;
    }

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
    state.sel_card = col_visible_count(board, 0) > 0 ? 0 : -1;

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
            if (g_filter_active == 1) {
                /* entering filter text */
                running = handle_filter_input(board, ch);
                if (g_filter_active == 0) {
                    /* filter was cleared */
                } else if (g_filter_active == 2) {
                    /* filter committed — back to nav */
                }
            } else if (g_label_picker_mode) {
                running = handle_label_picker_input(board, ch);
            } else if (g_detail_mode) {
                running = handle_detail_input(board, ch);
            } else if (g_review_mode) {
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
