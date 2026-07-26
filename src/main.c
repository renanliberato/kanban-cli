#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE    /* for usleep on Linux */
#include "board.h"
#include "tui.h"
#include "llm.h"
#include "enrich.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static char *default_board_path(void)
{
    const char *home = getenv("HOME");
    if (!home) return NULL;

    size_t len  = strlen(home) + strlen("/.kanban/default.db") + 1;
    char  *path = malloc(len);
    if (!path) return NULL;

    snprintf(path, len, "%s/.kanban/default.db", home);
    return path;
}

static int is_subcommand(const char *arg)
{
    static const char *subs[] = {"add", "list", "show", "enrich", "move", NULL};
    for (int i = 0; subs[i]; i++) {
        if (strcmp(arg, subs[i]) == 0) return 1;
    }
    return 0;
}

static int col_from_name(const char *name)
{
    if (!name) return -1;
    if (strcmp(name, "todo")  == 0) return COL_TODO;
    if (strcmp(name, "doing") == 0) return COL_DOING;
    if (strcmp(name, "done")  == 0) return COL_DONE;
    return -1;
}

static const char *col_name(int col)
{
    switch (col) {
    case COL_TODO:  return "To Do";
    case COL_DOING: return "Doing";
    case COL_DONE:  return "Done";
    default:        return "Unknown";
    }
}

/* ------------------------------------------------------------------ */
/* resolve path + subcommand from argv                                */
/* ------------------------------------------------------------------ */

static int resolve_args(int argc, char **argv,
                        const char **path_out, char **allocated_out,
                        int *subcmd_idx)
{
    *subcmd_idx = -1;
    *allocated_out = NULL;

    if (argc <= 1) {
        /* no arguments → TUI mode with default path */
        *allocated_out = default_board_path();
        *path_out = *allocated_out;
        if (!*path_out) { fprintf(stderr, "kanban: HOME not set\n"); return -1; }
        return 0;  /* TUI mode */
    }

    /* Check if argv[1] is a subcommand */
    if (is_subcommand(argv[1])) {
        *subcmd_idx = 1;
        *allocated_out = default_board_path();
        *path_out = *allocated_out;
        if (!*path_out) { fprintf(stderr, "kanban: HOME not set\n"); return -1; }
        return 1;  /* CLI mode */
    }

    /* argv[1] might be a path or a subcommand */
    if (argc >= 3 && is_subcommand(argv[2])) {
        /* argv[1] is path, argv[2] is subcommand */
        *path_out = argv[1];
        *subcmd_idx = 2;
        return 1;  /* CLI mode */
    }

    /* Otherwise argv[1] is the board path (TUI mode) */
    *path_out = argv[1];
    return 0;  /* TUI mode */
}

/* ------------------------------------------------------------------ */
/* CLI subcommand implementations                                     */
/* ------------------------------------------------------------------ */

static int cmd_add(Board *b, int argc, char **argv, int subcmd_idx)
{
    int col       = COL_TODO;   /* default */
    int use_ai    = 0;
    const char *title   = NULL;
    const char *desc    = NULL;

    /* Parse flags starting from subcmd_idx + 1.
       Positional arg: the first non-flag argument is the title. */
    int i = subcmd_idx + 1;

    /* If the next arg is a title (not starting with --), grab it first */
    if (i < argc && argv[i][0] != '-') {
        title = argv[i];
        i++;
    }

    for (; i < argc; i++) {
        if (strcmp(argv[i], "--col") == 0 && i + 1 < argc) {
            col = col_from_name(argv[++i]);
            if (col < 0) {
                fprintf(stderr, "kanban: invalid column '%s' (use todo, doing, or done)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--ai") == 0) {
            use_ai = 1;
        } else if (strcmp(argv[i], "--desc") == 0 && i + 1 < argc) {
            desc = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "kanban: unknown flag '%s'\n", argv[i]);
            return 1;
        } else if (!title) {
            title = argv[i];
        }
    }

    if (!title) {
        fprintf(stderr, "Usage: kanban add \"title\" [--col todo|doing|done] [--ai] [--desc \"text\"]\n");
        return 1;
    }

    int id = board_add_card(b, col, title);
    if (id < 0) {
        fprintf(stderr, "kanban: failed to add card\n");
        return 1;
    }

    /* Apply description if provided */
    if (desc && desc[0]) {
        board_set_card_description(b, id, desc);
    }

    /* AI enrich flow (synchronous, auto-accept).
       Non-interactive CLI cannot present a review screen, so accepted
       enrich results are applied directly.  The TUI flow (Ctrl+E) is
       the human-in-the-loop path. */
    if (use_ai) {
        /* Quick initialise LLM if not already done */
        llm_init();

        char *prompt = enrich_build_prompt(title, desc);
        if (!prompt) {
            fprintf(stderr, "kanban: failed to build enrich prompt\n");
        } else {
            int job_id = llm_submit(prompt, id, 60);
            free(prompt);

            if (job_id < 0) {
                fprintf(stderr, "kanban: failed to submit enrich job\n");
            } else {
                /* Poll synchronously until job completes */
                while (1) {
                    const LlmJob *job = llm_get_job(job_id);
                    if (!job) break;
                    if (job->state == LLM_DONE || job->state == LLM_FAILED ||
                        job->state == LLM_CANCELLED)
                        break;
                    usleep(100000);  /* 100ms */
                    llm_poll();
                }

                const LlmJob *job = llm_get_job(job_id);
                if (job && job->state == LLM_DONE && job->result) {
                    char *inner = enrich_unwrap_envelope(job->result);
                    if (inner) {
                        EnrichResult er;
                        if (enrich_parse_result(inner, &er) == 0) {
                            /* Auto-accept: apply to card */
                            if (er.description)
                                board_set_card_description(b, id, er.description);
                            for (int li = 0; li < er.label_count; li++)
                                board_add_label(b, id, er.labels[li]);
                            enrich_free_result(&er);
                        }
                        free(inner);
                    }
                }
            }
        }
    }

    /* Print the new card id */
    printf("%d\n", id);

    /* Save the board */
    /* We need to open a db handle if not already present.
       board_save needs a path, use the default_db_path concept. */
    return 0;
}

static int cmd_list(const Board *b, int argc, char **argv, int subcmd_idx)
{
    int filter_col = -1;

    for (int i = subcmd_idx + 1; i < argc; i++) {
        if (strcmp(argv[i], "--col") == 0 && i + 1 < argc) {
            filter_col = col_from_name(argv[++i]);
            if (filter_col < 0) {
                fprintf(stderr, "kanban: invalid column '%s'\n", argv[i]);
                return 1;
            }
        }
    }

    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        if (filter_col >= 0 && ci != filter_col) continue;
        const Column *col = &b->columns[ci];
        for (int i = 0; i < col->count; i++) {
            printf("%d %s %s\n", col->cards[i].id,
                   col_name(ci), col->cards[i].title);
        }
    }

    return 0;
}

static int cmd_show(const Board *b, int argc, char **argv, int subcmd_idx)
{
    if (subcmd_idx + 1 >= argc) {
        fprintf(stderr, "Usage: kanban show <id>\n");
        return 1;
    }

    int id = atoi(argv[subcmd_idx + 1]);
    Card *card = board_get_card(b, id);
    if (!card) {
        fprintf(stderr, "kanban: card %d not found\n", id);
        return 1;
    }

    printf("ID:          %d\n", card->id);
    printf("Title:       %s\n", card->title);
    printf("Description: %s\n", card->description ? card->description : "(none)");
    printf("Labels:      ");
    if (card->label_count == 0) {
        printf("(none)\n");
    } else {
        for (int i = 0; i < card->label_count; i++) {
            if (i > 0) printf(", ");
            printf("%s", card->labels[i]);
        }
        printf("\n");
    }
    printf("Created:     %s\n", card->created_at ? card->created_at : "unknown");
    printf("Updated:     %s\n", card->updated_at ? card->updated_at : "unknown");
    printf("Archived:    %s\n", card->archived ? "yes" : "no");

    return 0;
}

static int cmd_enrich(Board *b, int argc, char **argv, int subcmd_idx)
{
    if (subcmd_idx + 1 >= argc) {
        fprintf(stderr, "Usage: kanban enrich <id>\n");
        return 1;
    }

    int id = atoi(argv[subcmd_idx + 1]);
    Card *card = board_get_card(b, id);
    if (!card) {
        fprintf(stderr, "kanban: card %d not found\n", id);
        return 1;
    }

    llm_init();

    char *prompt = enrich_build_prompt(card->title, card->description);
    if (!prompt) {
        fprintf(stderr, "kanban: failed to build enrich prompt\n");
        return 1;
    }

    int job_id = llm_submit(prompt, id, 60);
    free(prompt);

    if (job_id < 0) {
        fprintf(stderr, "kanban: failed to submit enrich job\n");
        return 1;
    }

    /* Poll synchronously */
    while (1) {
        const LlmJob *job = llm_get_job(job_id);
        if (!job) break;
        if (job->state == LLM_DONE || job->state == LLM_FAILED ||
            job->state == LLM_CANCELLED)
            break;
        usleep(100000);
        llm_poll();
    }

    const LlmJob *job = llm_get_job(job_id);
    if (!job || job->state != LLM_DONE || !job->result) {
        fprintf(stderr, "kanban: enrich job failed (%s)\n",
                job ? (job->state == LLM_FAILED ? "timeout/error" : "cancelled") : "unknown");
        return 1;
    }

    /* Unwrap and print the proposed enrichment JSON */
    char *inner = enrich_unwrap_envelope(job->result);
    if (!inner) {
        fprintf(stderr, "kanban: failed to unwrap enrich result\n");
        return 1;
    }

    /* Print the parsed result as JSON for scriptability */
    EnrichResult er;
    if (enrich_parse_result(inner, &er) == 0) {
        printf("{\n");
        printf("  \"description\": \"%s\"", er.description ? er.description : "");
        printf(",\n");
        printf("  \"labels\": [");
        for (int i = 0; i < er.label_count; i++) {
            if (i > 0) printf(", ");
            printf("\"%s\"", er.labels[i]);
        }
        printf("],\n");
        printf("  \"questions\": [\n");
        for (int i = 0; i < er.question_count; i++) {
            if (i > 0) printf(",\n");
            printf("    {\"q\": \"%s\", \"a\": \"%s\"}",
                   er.questions[i], er.answers[i]);
        }
        printf("\n  ]\n");
        printf("}\n");
        enrich_free_result(&er);
    } else {
        /* Just print the raw unwrapped output */
        printf("%s\n", inner);
    }

    free(inner);
    return 0;
}

static int cmd_move(Board *b, int argc, char **argv, int subcmd_idx)
{
    if (subcmd_idx + 2 >= argc) {
        fprintf(stderr, "Usage: kanban move <id> <col>\n");
        return 1;
    }

    int id  = atoi(argv[subcmd_idx + 1]);
    int col = col_from_name(argv[subcmd_idx + 2]);
    if (col < 0) {
        fprintf(stderr, "kanban: invalid column '%s' (use todo, doing, or done)\n",
                argv[subcmd_idx + 2]);
        return 1;
    }

    int rc = board_move_card(b, id, col);
    if (rc != 0) {
        fprintf(stderr, "kanban: failed to move card %d to %s\n", id, col_name(col));
        return 1;
    }

    printf("Moved card %d to %s\n", id, col_name(col));
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *path   = NULL;
    char       *allocated_path = NULL;
    int         subcmd_idx     = -1;

    int mode = resolve_args(argc, argv, &path, &allocated_path, &subcmd_idx);

    if (mode < 0) {
        free(allocated_path);
        return 1;
    }

    if (mode == 0) {
        /* TUI mode */
        /* Set ESC delay to 50ms for responsive ESC key while allowing
           arrow-key sequences to be recognised (portable: ncurses reads
           this env var during initscr). */
        setenv("ESCDELAY", "50", 1);

        Board b = board_new();
        if (board_load(&b, path) != 0) {
            fprintf(stderr, "kanban: failed to load board from '%s'\n", path);
            board_free(&b);
            free(allocated_path);
            return 1;
        }

        llm_init();

        int ret = tui_run(&b, path);

        llm_free();
        board_free(&b);
        free(allocated_path);
        return ret;
    }

    /* CLI mode */
    Board b = board_new();
    if (board_load(&b, path) != 0) {
        fprintf(stderr, "kanban: failed to load board from '%s'\n", path);
        board_free(&b);
        free(allocated_path);
        return 1;
    }

    const char *subcmd = argv[subcmd_idx];
    int ret = 0;

    if (strcmp(subcmd, "add") == 0) {
        ret = cmd_add(&b, argc, argv, subcmd_idx);
    } else if (strcmp(subcmd, "list") == 0) {
        ret = cmd_list(&b, argc, argv, subcmd_idx);
    } else if (strcmp(subcmd, "show") == 0) {
        ret = cmd_show(&b, argc, argv, subcmd_idx);
    } else if (strcmp(subcmd, "enrich") == 0) {
        ret = cmd_enrich(&b, argc, argv, subcmd_idx);
    } else if (strcmp(subcmd, "move") == 0) {
        ret = cmd_move(&b, argc, argv, subcmd_idx);
    } else {
        fprintf(stderr, "kanban: unknown subcommand '%s'\n", subcmd);
        ret = 1;
    }

    /* Save the board after CLI operations */
    board_save(&b, path);
    board_free(&b);
    free(allocated_path);
    return ret;
}
