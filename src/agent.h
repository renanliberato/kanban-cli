#ifndef AGENT_H
#define AGENT_H

#include "board.h"

/* ------------------------------------------------------------------ */
/* types                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    AGENT_COMMENT,
    AGENT_DESCRIPTION
} AgentType;

typedef struct {
    char     *name;        /* required, [a-z0-9-]+ */
    AgentType type;        /* "comment" or "description" */
    char     *prompt_body; /* default prompt body from md file */
    char     *source_path; /* file path it was loaded from */
} Agent;

/* ------------------------------------------------------------------ */
/* discovery                                                          */
/* ------------------------------------------------------------------ */

/*
 * Load all agents from discovery dirs.
 * project_kanban_dir: path to a .kanban/ directory discovered in cwd
 *   (may be NULL if not found). We read project_kanban_dir/agents/ - *.md first.
 * Then we read ~/.kanban/agents/ - *.md.
 * Name clash: project overrides global.
 * Invalid files (bad/missing frontmatter) are skipped with a stderr warning.
 *
 * Returns allocated Agent array on success, *count_out = count.
 * Returns NULL on alloc failure, *count_out = 0.
 * Caller must free with agent_free_list().
 */
Agent *agent_discover(const char *project_kanban_dir, int *count_out);

/* Free an agent list returned by agent_discover. */
void   agent_free_list(Agent *agents, int count);

/* Look up an agent by name. Returns index into the agents array,
   or -1 if not found. */
int    agent_find(const Agent *agents, int count, const char *name);

/* ------------------------------------------------------------------ */
/* @mention scanner                                                   */
/* ------------------------------------------------------------------ */

/*
 * Scan a comment body for the first @mention matching a known agent.
 * The mention must be at the start of the string or preceded by whitespace.
 * If found, sets *agent_idx to the agent index and *mention_start /
 * *mention_len to the position of the @name token in the body.
 * Returns 1 if a match is found, 0 otherwise.
 */
int    agent_scan_mention(const char *body,
                          const Agent *agents, int agent_count,
                          int *agent_idx,
                          int *mention_start, int *mention_len);

/*
 * Check if the body contains any @mention pattern (even for unknown agents).
 * Returns 1 if an @name pattern exists, 0 otherwise.
 * If found, *mention_start and *mention_len are set to the first @mention.
 */
int    agent_has_mention(const char *body,
                         int *mention_start, int *mention_len);

/* ------------------------------------------------------------------ */
/* prompt assembly                                                    */
/* ------------------------------------------------------------------ */

/*
 * Build a prompt string for invoking an agent.
 *
 *   agent         : the agent config (contains default prompt body + type)
 *   card          : the target card
 *   board_name    : display name of the board
 *   project_dir   : project directory path (can be NULL; stated in prompt)
 *   user_message  : the user's message (comment body with @mention stripped)
 *   comments      : full comment thread (from board_get_comments)
 *   comment_count : number of comments
 *   col_name      : the column the card is in ("To Do" / "Doing" / "Done")
 *
 * Returns a malloc'd string the caller must free, or NULL on error.
 */
char *agent_build_prompt(const Agent *agent,
                         const Card *card,
                         const char *board_name,
                         const char *project_dir,
                         const char *user_message,
                         const Comment *comments,
                         int comment_count,
                         const char *col_name);

/* ------------------------------------------------------------------ */
/* result parsing (description agents only)                           */
/* ------------------------------------------------------------------ */

/*
 * Parse the LLM result for a description-type agent.
 * Expected strict JSON: {"title": "...", "description": "..."}
 * Both fields are optional — missing fields leave the out params unchanged
 * (they are set to NULL on entry).
 * Returns 0 on success, -1 on parse failure.
 * On success, *title_out and/or *desc_out are allocated (caller must free).
 */
int    agent_parse_description_result(const char *json_str,
                                      char **title_out,
                                      char **desc_out);

#endif
