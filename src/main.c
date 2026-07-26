#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE    /* for usleep on Linux */
#include "board.h"
#include "board_path.h"
#include "tui.h"
#include "llm.h"
#include "enrich.h"
#include "agent.h"

#include <ctype.h>
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

static int is_subcommand(const char *arg)
{
    static const char *subs[] = {
        "add", "list", "show", "enrich", "move",
        "list-boards", "comment", "agents", NULL
    };
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

/*
 * Parse argv for board_name (-b/--board), explicit path, and subcommand.
 * Precedence: explicit path > -b name > discovery > ~/.kanban/default.db
 *
 * Sets *board_name_out to the -b value (NULL if not given).
 * Sets *subcmd_idx to the index of the subcommand (-1 if none).
 * Returns 0 for TUI mode, 1 for CLI mode (including list-boards).
 * Returns -1 on error (HOME not set).
 * In CLI mode, the resolved path is stored in *path_out (malloc'd).
 */
static int resolve_args(int argc, char **argv,
                        char **path_out, char **board_name_out,
                        int *subcmd_idx)
{
    *subcmd_idx      = -1;
    *path_out        = NULL;
    *board_name_out  = NULL;
    const char *explicit_path = NULL;

    /* First pass: find -b/--board, explicit path, and subcommand index.
       We parse left-to-right.  -b/--board sets board_name.
       The first positional that is NOT a subcommand is the explicit path.
       The first positional that IS a subcommand sets subcmd_idx. */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--board") == 0) {
            if (i + 1 < argc) {
                free(*board_name_out);
                *board_name_out = strdup(argv[i + 1]);
                i += 2;
                continue;
            } else {
                fprintf(stderr, "kanban: -b/--board requires a name\n");
                return -1;
            }
        }

        if (argv[i][0] == '-') {
            /* unknown flag — skip for now (subcommand handlers parse their own) */
            i++;
            continue;
        }

        if (is_subcommand(argv[i])) {
            *subcmd_idx = i;
            break;  /* subcommand ends our scan */
        }

        /* Positional arg that's not a subcommand — explicit path */
        if (!explicit_path)
            explicit_path = argv[i];
        i++;
    }

    /* Resolve the board path */
    if (*subcmd_idx >= 0 && strcmp(argv[*subcmd_idx], "list-boards") == 0) {
        /* list-boards doesn't need a board path */
        *path_out = NULL;
        return 1;  /* CLI mode */
    }

    *path_out = board_path_resolve(explicit_path, *board_name_out);
    if (!*path_out) {
        fprintf(stderr, "kanban: failed to resolve board path\n");
        return -1;
    }

    if (*subcmd_idx >= 0)
        return 1;  /* CLI mode */

    return 0;  /* TUI mode */
}

/* ------------------------------------------------------------------ */
/* list-boards subcommand                                             */
/* ------------------------------------------------------------------ */

static int cmd_list_boards(void)
{
    int count = 0;
    BoardInfo *boards = board_path_list(&count);

    if (!boards || count == 0) {
        printf("No boards found.\n");
        free(boards);
        return 0;
    }

    for (int i = 0; i < count; i++) {
        printf("%s", boards[i].display_name);
        if (boards[i].card_count >= 0)
            printf("  (%d cards)", boards[i].card_count);
        printf("  [%s]\n", boards[i].path);
        free(boards[i].path);
        free(boards[i].display_name);
    }
    free(boards);
    return 0;
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
            int job_id = llm_submit(prompt, id, 0);  /* 0 = use default timeout */
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
    return 0;
}

static int cmd_list(const Board *b, int argc, char **argv, int subcmd_idx)
{
    int filter_col = -1;
    int show_archived = 0;

    for (int i = subcmd_idx + 1; i < argc; i++) {
        if (strcmp(argv[i], "--col") == 0 && i + 1 < argc) {
            filter_col = col_from_name(argv[++i]);
            if (filter_col < 0) {
                fprintf(stderr, "kanban: invalid column '%s'\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--archived") == 0) {
            show_archived = 1;
        }
    }

    for (int ci = 0; ci < MAX_COLUMNS; ci++) {
        if (filter_col >= 0 && ci != filter_col) continue;
        const Column *col = &b->columns[ci];
        for (int i = 0; i < col->count; i++) {
            if (!show_archived && col->cards[i].archived)
                continue;
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

    /* comments */
    Comment *comments = NULL;
    int comment_count = 0;
    if (board_get_comments((Board *)b, id, &comments, &comment_count) == 0 && comment_count > 0) {
        printf("Comments:    %d\n\n", comment_count);
        for (int i = 0; i < comment_count; i++) {
            printf("  %s · %s\n", comments[i].author, comments[i].created_at);
            printf("  %s\n\n", comments[i].body);
        }
    } else {
        printf("Comments:    (none)\n");
    }
    board_free_comments(comments, comment_count);

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

    int job_id = llm_submit(prompt, id, 0);  /* 0 = use default timeout */
    free(prompt);

    if (job_id < 0) {
        fprintf(stderr, "kanban: failed to submit enrich job (queue full?)\n");
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
        const char *reason = "unknown";
        if (job) {
            if (job->state == LLM_FAILED && job->result && job->result[0])
                reason = job->result;
            else if (job->state == LLM_FAILED)
                reason = "timeout/error";
            else if (job->state == LLM_CANCELLED)
                reason = "cancelled";
        }
        fprintf(stderr, "kanban: enrich job failed (%s)\n", reason);
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

static int cmd_comment(Board *b, int argc, char **argv, int subcmd_idx,
                        const char *db_path)
{
    if (subcmd_idx + 2 >= argc) {
        fprintf(stderr, "Usage: kanban comment <id> \"text\"\n");
        return 1;
    }

    int id = atoi(argv[subcmd_idx + 1]);
    const char *body = argv[subcmd_idx + 2];

    if (!body || !body[0]) {
        fprintf(stderr, "kanban: comment text cannot be empty\n");
        return 1;
    }

    /* Check card exists */
    Card *card = board_get_card(b, id);
    if (!card) {
        fprintf(stderr, "kanban: card %d not found\n", id);
        return 1;
    }

    /* Determine author */
    const char *author = getenv("USER");
    if (!author || !author[0]) author = "me";

    /* Save comment */
    int rc = board_add_comment(b, id, author, body);
    if (rc != 0) {
        fprintf(stderr, "kanban: failed to add comment to card %d\n", id);
        return 1;
    }

    /* Discover agents and scan for @mention */
    char *kanban_dir = board_path_get_kanban_dir(db_path);
    int agent_count = 0;
    Agent *agents = agent_discover(kanban_dir, &agent_count);

    int agent_idx = -1, mention_start, mention_len;
    if (agents && agent_count > 0 &&
        agent_scan_mention(body, agents, agent_count,
                          &agent_idx, &mention_start, &mention_len)) {

        /* Build user message: strip the @mention from the body */
        char user_msg[2048];
        int body_len = (int)strlen(body);
        /* Copy everything before the mention */
        int pos = 0;
        if (mention_start > 0) {
            memcpy(user_msg + pos, body, (size_t)mention_start);
            pos += mention_start;
        }
        /* Copy everything after the mention */
        if (mention_start + mention_len < body_len) {
            memcpy(user_msg + pos, body + mention_start + mention_len,
                   (size_t)(body_len - mention_start - mention_len));
            pos += body_len - mention_start - mention_len;
        }
        user_msg[pos] = '\0';

        /* Trim whitespace */
        char *trimmed = user_msg;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
        if (!*trimmed) trimmed = "";

        /* Get column name */
        const char *col_name_str = "Unknown";
        for (int ci = 0; ci < MAX_COLUMNS; ci++) {
            for (int i = 0; i < b->columns[ci].count; i++) {
                if (b->columns[ci].cards[i].id == id) {
                    col_name_str = col_name(ci);
                    break;
                }
            }
        }

        /* Get comments for context */
        Comment *comments = NULL;
        int comment_count = 0;
        board_get_comments(b, id, &comments, &comment_count);

        /* Build prompt */
        char *prompt = agent_build_prompt(
            &agents[agent_idx], card,
            db_path ? board_path_display_name(db_path) : "unknown",
            kanban_dir, trimmed,
            comments, comment_count, col_name_str);
        board_free_comments(comments, comment_count);

        if (prompt) {
            /* Submit LLM job synchronously */
            llm_init();
            int job_id = llm_submit(prompt, id, 0);
            free(prompt);

            if (job_id >= 0) {
                /* Poll until done */
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
                if (job && job->state == LLM_DONE && job->result && job->result[0]) {
                    if (agents[agent_idx].type == AGENT_COMMENT) {
                        /* Add result as a comment authored by @agentname */
                        char at_author[128];
                        snprintf(at_author, sizeof(at_author), "@%s",
                                 agents[agent_idx].name);
                        board_add_comment(b, id, at_author, job->result);
                        printf("Agent @%s commented on card %d\n",
                               agents[agent_idx].name, id);
                    } else {
                        /* Description agent — parse JSON, update card */
                        char *new_title = NULL;
                        char *new_desc = NULL;
                        if (agent_parse_description_result(job->result,
                                                           &new_title, &new_desc) == 0) {
                            if (new_title)
                                board_edit_card_title(b, id, new_title);
                            if (new_desc)
                                board_set_card_description(b, id, new_desc);
                            printf("Agent @%s updated card %d\n",
                                   agents[agent_idx].name, id);
                            free(new_title);
                            free(new_desc);
                        } else {
                            fprintf(stderr,
                                    "kanban: agent @%s failed to parse result\n",
                                    agents[agent_idx].name);
                            rc = 1;
                        }
                    }
                } else {
                    const char *reason = "unknown error";
                    if (job) {
                        if (job->state == LLM_FAILED && job->result && job->result[0])
                            reason = job->result;
                        else if (job->state == LLM_FAILED)
                            reason = "timeout/error";
                        else if (job->state == LLM_CANCELLED)
                            reason = "cancelled";
                    }
                    fprintf(stderr,
                            "kanban: agent @%s failed: %s\n",
                            agents[agent_idx].name, reason);
                    rc = 1;
                }
            } else {
                fprintf(stderr, "kanban: failed to submit agent job\n");
                rc = 1;
            }
        }
    }

    if (agents) agent_free_list(agents, agent_count);
    free(kanban_dir);

    if (rc == 0)
        printf("%d\n", id);
    return rc;
}

static int cmd_agents(int argc, char **argv, int subcmd_idx,
                      const char *db_path)
{
    (void)argc; (void)argv; (void)subcmd_idx;

    char *kanban_dir = board_path_get_kanban_dir(db_path);
    int count = 0;
    Agent *agents = agent_discover(kanban_dir, &count);

    if (!agents || count == 0) {
        printf("No agents configured.\n");
        printf("Create .md files in %sagents/ or ~/.kanban/agents/\n",
               kanban_dir ? "" : "~/.kanban/");
        printf("Format:\n");
        printf("  ---\n");
        printf("  name: agent-name\n");
        printf("  type: comment|description\n");
        printf("  ---\n");
        printf("  Default prompt body...\n");
    } else {
        printf("Agents (%d):\n\n", count);
        for (int i = 0; i < count; i++) {
            printf("  %s\n", agents[i].name);
            printf("    type:   %s\n",
                   agents[i].type == AGENT_COMMENT ? "comment" : "description");
            printf("    source: %s\n", agents[i].source_path);
        }
    }

    agent_free_list(agents, count);
    free(kanban_dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    char *path       = NULL;
    char *board_name = NULL;
    int   subcmd_idx = -1;

    int mode = resolve_args(argc, argv, &path, &board_name, &subcmd_idx);

    if (mode < 0) {
        free(path);
        free(board_name);
        return 1;
    }

    /* list-boards is special — no board needed */
    if (mode == 1 && subcmd_idx >= 0 &&
        strcmp(argv[subcmd_idx], "list-boards") == 0) {
        int ret = cmd_list_boards();
        free(path);
        free(board_name);
        return ret;
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
            free(path);
            free(board_name);
            return 1;
        }

        llm_init();

        /* Derive a display name for the board */
        char *display_name = board_name ? board_name : board_path_display_name(path);
        int ret = tui_run(&b, path, display_name);

        free(display_name);
        llm_free();
        board_free(&b);
        free(path);
        free(board_name);
        return ret;
    }

    /* CLI mode */
    Board b = board_new();
    if (board_load(&b, path) != 0) {
        fprintf(stderr, "kanban: failed to load board from '%s'\n", path);
        board_free(&b);
        free(path);
        free(board_name);
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
    } else if (strcmp(subcmd, "comment") == 0) {
        ret = cmd_comment(&b, argc, argv, subcmd_idx, path);
    } else if (strcmp(subcmd, "agents") == 0) {
        ret = cmd_agents(argc, argv, subcmd_idx, path);
    } else {
        fprintf(stderr, "kanban: unknown subcommand '%s'\n", subcmd);
        ret = 1;
    }

    /* Save the board after CLI operations */
    board_save(&b, path);
    board_free(&b);
    free(path);
    free(board_name);
    return ret;
}
