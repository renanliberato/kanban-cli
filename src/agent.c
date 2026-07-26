#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "../vendor/cJSON.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

static char *xstrndup(const char *s, size_t n)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    if (n < len) len = n;
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

/* Return the kanban home directory: $HOME/.kanban.
   Returns malloc'd string. */
static char *kanban_home_dir(void)
{
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    size_t len = strlen(home) + 8;  /* "/.kanban" */
    char *path = malloc(len + 1);
    if (!path) { path = xstrdup("/tmp/.kanban"); return path; }
    snprintf(path, len + 1, "%s/.kanban", home);
    return path;
}

/* ------------------------------------------------------------------ */
/* frontmatter parser                                                 */
/* ------------------------------------------------------------------ */

/*
 * Parse frontmatter from a markdown string.
 * Format:
 *   ---\n
 *   key: value\n
 *   ...\n
 *   ---\n
 *   body...
 *
 * Returns 0 on success, -1 if no valid frontmatter found.
 * On success, fills *name_out (allocated), *type_str_out (allocated),
 * and *body_out (allocated; may be NULL/empty).
 * Caller frees all three.
 */
static int parse_frontmatter(const char *content,
                             char **name_out,
                             char **type_str_out,
                             char **body_out)
{
    *name_out = NULL;
    *type_str_out = NULL;
    *body_out = NULL;

    if (!content || !content[0]) return -1;

    /* Must start with "---\n" */
    if (strncmp(content, "---\n", 4) != 0) return -1;

    /* Find closing "---" */
    const char *close = strstr(content + 4, "\n---");
    if (!close) return -1;

    /* Extract the frontmatter block (between opening ---\n and closing \n---) */
    const char *fm_start = content + 4;
    const char *fm_end = close;

    /* Parse key: value lines */
    const char *line_start = fm_start;
    while (line_start < fm_end) {
        /* Find end of this line */
        const char *line_end = strchr(line_start, '\n');
        if (!line_end || line_end > fm_end) line_end = fm_end;

        const char *colon = NULL;
        for (const char *p = line_start; p < line_end; p++) {
            if (*p == ':') { colon = p; break; }
        }

        if (colon) {
            /* Extract key: trim trailing whitespace before colon */
            const char *key_end = colon;
            while (key_end > line_start && isspace((unsigned char)*(key_end - 1)))
                key_end--;
            size_t key_len = (size_t)(key_end - line_start);
            char *key = xstrndup(line_start, key_len);

            /* Extract value: skip colon + whitespace */
            const char *val = colon + 1;
            while (val < line_end && isspace((unsigned char)*val)) val++;
            size_t val_len = (size_t)(line_end - val);
            /* Trim trailing whitespace from value */
            while (val_len > 0 && isspace((unsigned char)val[val_len - 1]))
                val_len--;
            char *value = xstrndup(val, val_len);

            if (key && value) {
                if (strcmp(key, "name") == 0) {
                    free(*name_out);
                    *name_out = value;
                    value = NULL;  /* ownership transferred */
                } else if (strcmp(key, "type") == 0) {
                    free(*type_str_out);
                    *type_str_out = value;
                    value = NULL;
                }
            }
            free(key);
            free(value);
        }

        line_start = line_end + 1;
    }

    /* Validate: both name and type must be present */
    if (!*name_out || !*type_str_out) {
        free(*name_out); *name_out = NULL;
        free(*type_str_out); *type_str_out = NULL;
        return -1;
    }

    /* Extract body: everything after "\n---\n" */
    const char *body_start = close + 4;  /* skip \n---\n */
    if (*body_start == '\n') body_start++; /* skip leading newline */
    if (*body_start) {
        *body_out = xstrdup(body_start);
    } else {
        *body_out = xstrdup("");
    }

    return 0;
}

/* Validate agent name: [a-z0-9-]+ */
static int is_valid_agent_name(const char *name)
{
    if (!name || !name[0]) return 0;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '-'))
            return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* agent loading from a directory                                     */
/* ------------------------------------------------------------------ */
/* agent loading from a directory                                     */
/* ------------------------------------------------------------------ */

/*
 * Load agents from a single directory: <dir>/agents/ ( .md files)
 * Existing agents in *list may be overridden on name clash.
 * Returns 0 on success, -1 on dir open failure (which is not an error — dir may not exist).
 */
static int load_agents_from_dir(const char *kanban_dir,
                                 Agent **list, int *count, int *cap)
{
    size_t dir_len = strlen(kanban_dir);
    size_t agents_path_len = dir_len + 8;  /* "/agents" */
    char *agents_dir = malloc(agents_path_len + 1);
    if (!agents_dir) return -1;
    snprintf(agents_dir, agents_path_len + 1, "%s/agents", kanban_dir);

    DIR *d = opendir(agents_dir);
    if (!d) {
        free(agents_dir);
        return 0;  /* no agents dir — not an error */
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len <= 3) continue;
        if (strcmp(entry->d_name + name_len - 3, ".md") != 0) continue;

        /* Build full path */
        size_t plen = agents_path_len + 1 + name_len + 1;
        char *full_path = malloc(plen);
        if (!full_path) continue;
        snprintf(full_path, plen, "%s/%s", agents_dir, entry->d_name);

        /* Read file */
        FILE *f = fopen(full_path, "r");
        if (!f) {
            fprintf(stderr, "agent: warning: cannot open '%s'\n", full_path);
            free(full_path);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsize < 0) fsize = 0;
        /* Cap at 64KB for safety */
        if (fsize > 65536) fsize = 65536;
        char *content = malloc((size_t)fsize + 1);
        if (!content) {
            fclose(f);
            free(full_path);
            continue;
        }
        size_t read_len = fread(content, 1, (size_t)fsize, f);
        content[read_len] = '\0';
        fclose(f);

        /* Parse frontmatter */
        char *name = NULL;
        char *type_str = NULL;
        char *body = NULL;
        if (parse_frontmatter(content, &name, &type_str, &body) != 0) {
            fprintf(stderr, "agent: warning: skipping '%s' — bad or missing frontmatter\n",
                    entry->d_name);
            free(content);
            free(full_path);
            free(name);
            free(type_str);
            free(body);
            continue;
        }
        free(content);

        /* Validate name */
        if (!is_valid_agent_name(name)) {
            fprintf(stderr, "agent: warning: skipping '%s' — invalid name '%s' (must be [a-z0-9-]+)\n",
                    entry->d_name, name);
            free(name);
            free(type_str);
            free(body);
            free(full_path);
            continue;
        }

        /* Parse type */
        AgentType atype;
        if (strcmp(type_str, "comment") == 0) {
            atype = AGENT_COMMENT;
        } else if (strcmp(type_str, "description") == 0) {
            atype = AGENT_DESCRIPTION;
        } else {
            fprintf(stderr, "agent: warning: skipping '%s' — unknown type '%s' (must be comment or description)\n",
                    entry->d_name, type_str);
            free(name);
            free(type_str);
            free(body);
            free(full_path);
            continue;
        }

        /* Check for name clash — if already exists, replace (last-loaded wins for project dir) */
        int existing = -1;
        for (int i = 0; i < *count; i++) {
            if (strcmp((*list)[i].name, name) == 0) {
                existing = i;
                break;
            }
        }

        if (existing >= 0) {
            /* Replace existing */
            free((*list)[existing].name);
            free((*list)[existing].prompt_body);
            free((*list)[existing].source_path);
            (*list)[existing].name = name;
            (*list)[existing].type = atype;
            (*list)[existing].prompt_body = body ? body : xstrdup("");
            (*list)[existing].source_path = full_path;
        } else {
            /* Grow list if needed */
            if (*count >= *cap) {
                int newcap = *cap ? *cap * 2 : 4;
                Agent *tmp = realloc(*list, (size_t)newcap * sizeof(Agent));
                if (!tmp) {
                    free(name); free(type_str); free(body); free(full_path);
                    closedir(d);
                    free(agents_dir);
                    return -1;
                }
                *list = tmp;
                *cap = newcap;
            }

            Agent *a = &(*list)[*count];
            a->name = name;
            a->type = atype;
            a->prompt_body = body ? body : xstrdup("");
            a->source_path = full_path;
            (*count)++;
        }

        free(type_str);
    }

    closedir(d);
    free(agents_dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* public API — discovery                                             */
/* ------------------------------------------------------------------ */

Agent *agent_discover(const char *project_kanban_dir, int *out_count)
{
    if (!out_count) return NULL;

    Agent *list = NULL;
    int count = 0;
    int cap = 0;

    /* 1. Project agents first (./.kanban/agents/ - .md) — these win on clash */
    if (project_kanban_dir) {
        if (load_agents_from_dir(project_kanban_dir, &list, &count, &cap) != 0) {
            agent_free_list(list, count);
            *out_count = 0;
            return NULL;
        }
    }

    /* 2. Global agents (~/.kanban/agents/ - .md) — project already loaded, so
          these only fill in names not yet seen */
    char *home_kanban = kanban_home_dir();
    if (home_kanban) {
        /* In load_agents_from_dir, name-clash replaces. We want project-first
           precedence, so we DON'T want global to override project. Let's modify:
           we need a mode that skips existing names. Actually, since project is
           loaded first, we can just load global with the understanding that
           existing names won't be replaced. But load_agents_from_dir replaces
           on name clash... So we need a flag.

           Simplest fix: load global into a separate temp list, then merge only
           names not already present. */
        Agent *global_list = NULL;
        int global_count = 0;
        int global_cap = 0;
        if (load_agents_from_dir(home_kanban, &global_list, &global_count, &global_cap) == 0) {
            for (int i = 0; i < global_count; i++) {
                /* Check if name already exists */
                int dup = 0;
                for (int j = 0; j < count; j++) {
                    if (strcmp(list[j].name, global_list[i].name) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    /* Grow if needed */
                    if (count >= cap) {
                        int newcap = cap ? cap * 2 : 4;
                        Agent *tmp = realloc(list, (size_t)newcap * sizeof(Agent));
                        if (!tmp) break;
                        list = tmp;
                        cap = newcap;
                    }
                    list[count].name = xstrdup(global_list[i].name);
                    list[count].type = global_list[i].type;
                    list[count].prompt_body = xstrdup(global_list[i].prompt_body);
                    list[count].source_path = xstrdup(global_list[i].source_path);
                    count++;
                }
            }
            /* Free global list (but not the strings — we dup'd them) */
            for (int i = 0; i < global_count; i++) {
                free(global_list[i].name);
                free(global_list[i].prompt_body);
                free(global_list[i].source_path);
            }
            free(global_list);
        }
        free(home_kanban);
    }

    *out_count = count;
    return list;
}

void agent_free_list(Agent *agents, int count)
{
    if (!agents) return;
    for (int i = 0; i < count; i++) {
        free(agents[i].name);
        free(agents[i].prompt_body);
        free(agents[i].source_path);
    }
    free(agents);
}

int agent_find(const Agent *agents, int count, const char *name)
{
    if (!agents || !name) return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(agents[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* @mention scanner                                                   */
/* ------------------------------------------------------------------ */

int agent_scan_mention(const char *body,
                        const Agent *agents, int agent_count,
                        int *agent_idx,
                        int *mention_start, int *mention_len)
{
    if (!body || !agents || agent_count <= 0) return 0;

    const char *p = body;
    while (*p) {
        /* Find '@' that is at start or preceded by whitespace */
        if (*p == '@') {
            int at_start = (p == body) || isspace((unsigned char)*(p - 1));
            if (!at_start) { p++; continue; }

            /* Extract the name: after '@', match [a-z0-9-]+ */
            const char *name_start = p + 1;
            const char *name_end = name_start;
            while (*name_end && (
                   (*name_end >= 'a' && *name_end <= 'z') ||
                   (*name_end >= '0' && *name_end <= '9') ||
                   *name_end == '-'))
                name_end++;

            size_t name_len = (size_t)(name_end - name_start);
            if (name_len == 0) { p++; continue; }

            char *name = xstrndup(name_start, name_len);
            int found = agent_find(agents, agent_count, name);
            free(name);

            if (found >= 0) {
                *agent_idx = found;
                *mention_start = (int)(p - body);
                *mention_len = (int)(name_end - p);
                return 1;
            }
        }
        p++;
    }

    return 0;
}

int agent_has_mention(const char *body,
                      int *mention_start, int *mention_len)
{
    if (!body) return 0;

    const char *p = body;
    while (*p) {
        if (*p == '@') {
            int at_start = (p == body) || isspace((unsigned char)*(p - 1));
            if (at_start) {
                const char *name_start = p + 1;
                const char *name_end = name_start;
                while (*name_end && (
                       (*name_end >= 'a' && *name_end <= 'z') ||
                       (*name_end >= '0' && *name_end <= '9') ||
                       *name_end == '-'))
                    name_end++;

                if (name_end > name_start) {
                    *mention_start = (int)(p - body);
                    *mention_len = (int)(name_end - p);
                    return 1;
                }
            }
        }
        p++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* prompt assembly                                                    */
/* ------------------------------------------------------------------ */

/* Forward declaration */
static void append_str(char **buf, size_t *len, size_t *cap, const char *s);
static void append_fmt(char **buf, size_t *len, size_t *cap,
                       const char *fmt, ...);

char *agent_build_prompt(const Agent *agent,
                         const Card *card,
                         const char *board_name,
                         const char *project_dir,
                         const char *user_message,
                         const Comment *comments,
                         int comment_count,
                         const char *col_name)
{
    if (!agent || !card) return NULL;

    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;

    /* ---- Default prompt body ---- */
    if (agent->prompt_body && agent->prompt_body[0]) {
        append_str(&buf, &len, &cap, agent->prompt_body);
        append_str(&buf, &len, &cap, "\n\n");
    }

    /* ---- User message ---- */
    append_str(&buf, &len, &cap, "## User Request\n\n");
    if (user_message && user_message[0]) {
        append_str(&buf, &len, &cap, user_message);
    } else {
        append_str(&buf, &len, &cap, "(no specific message)");
    }
    append_str(&buf, &len, &cap, "\n\n");

    /* ---- Task context ---- */
    append_str(&buf, &len, &cap, "## Task Context\n\n");
    append_fmt(&buf, &len, &cap, "**ID:** %d\n", card->id);
    append_fmt(&buf, &len, &cap, "**Title:** %s\n",
               card->title ? card->title : "(none)");
    append_fmt(&buf, &len, &cap, "**Description:** %s\n",
               card->description && card->description[0]
                   ? card->description : "(none)");

    /* Labels */
    append_str(&buf, &len, &cap, "**Labels:** ");
    if (card->label_count == 0) {
        append_str(&buf, &len, &cap, "(none)");
    } else {
        for (int i = 0; i < card->label_count; i++) {
            if (i > 0) append_str(&buf, &len, &cap, ", ");
            append_str(&buf, &len, &cap, card->labels[i]);
        }
    }
    append_str(&buf, &len, &cap, "\n");

    /* Comment thread */
    append_fmt(&buf, &len, &cap, "**Comments (%d):**\n", comment_count);
    if (comment_count == 0) {
        append_str(&buf, &len, &cap, "(no comments)\n");
    } else {
        for (int i = 0; i < comment_count; i++) {
            append_fmt(&buf, &len, &cap, "- **%s** (%s): %s\n",
                       comments[i].author ? comments[i].author : "?",
                       comments[i].created_at ? comments[i].created_at : "?",
                       comments[i].body ? comments[i].body : "");
        }
    }
    append_str(&buf, &len, &cap, "\n");

    /* ---- Board context ---- */
    append_str(&buf, &len, &cap, "## Board Context\n\n");
    append_fmt(&buf, &len, &cap, "**Board:** %s\n",
               board_name ? board_name : "unknown");
    append_fmt(&buf, &len, &cap, "**Column:** %s\n",
               col_name ? col_name : "unknown");
    append_str(&buf, &len, &cap, "**Columns:** To Do, Doing, Done\n");

    /* ---- Project context ---- */
    if (project_dir && project_dir[0]) {
        append_str(&buf, &len, &cap, "\n## Project Context\n\n");
        append_fmt(&buf, &len, &cap,
                   "The project directory is `%s`. You may inspect files "
                   "in this directory if needed.\n", project_dir);
    }

    /* ---- Output instruction (type-specific) ---- */
    append_str(&buf, &len, &cap, "\n## Output Instruction\n\n");
    if (agent->type == AGENT_COMMENT) {
        append_str(&buf, &len, &cap,
                   "Respond with the comment text only. "
                   "Do not include any JSON, formatting, or markup — "
                   "just the plain comment text.\n");
    } else {
        append_str(&buf, &len, &cap,
                   "Respond with strict JSON only:\n"
                   "{\"title\": \"...\", \"description\": \"...\"}\n"
                   "Include both fields. The title should be a concise "
                   "summary; the description should be comprehensive.\n");
    }

    return buf;
}

/* ------------------------------------------------------------------ */
/* result parsing (description agents)                                */
/* ------------------------------------------------------------------ */

int agent_parse_description_result(const char *json_str,
                                    char **title_out,
                                    char **desc_out)
{
    if (!json_str || !title_out || !desc_out) return -1;

    /* Initialise outputs to NULL — caller's responsibility to check */
    *title_out = NULL;
    *desc_out = NULL;

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    const cJSON *t = cJSON_GetObjectItem(root, "title");
    if (cJSON_IsString(t) && t->valuestring && t->valuestring[0])
        *title_out = xstrdup(t->valuestring);

    const cJSON *d = cJSON_GetObjectItem(root, "description");
    if (cJSON_IsString(d) && d->valuestring && d->valuestring[0])
        *desc_out = xstrdup(d->valuestring);

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------ */
/* internal: string builder                                           */
/* ------------------------------------------------------------------ */

static void append_str(char **buf, size_t *len, size_t *cap, const char *s)
{
    if (!s) return;
    size_t slen = strlen(s);
    if (slen == 0) {
        /* Empty string: ensure buf exists */
        if (!*buf) {
            *cap = 1;
            *buf = malloc(*cap);
            if (*buf) (*buf)[0] = '\0';
        }
        return;
    }

    if (!*buf || *len + slen + 1 > *cap) {
        size_t newcap = *cap ? *cap * 2 : 256;
        while (newcap < *len + slen + 1) newcap *= 2;
        char *tmp = realloc(*buf, newcap);
        if (!tmp) return;
        *buf = tmp;
        *cap = newcap;
    }

    memcpy(*buf + *len, s, slen);
    *len += slen;
    (*buf)[*len] = '\0';
}

#include <stdarg.h>

static void append_fmt(char **buf, size_t *len, size_t *cap,
                        const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    /* Try to format into a stack buffer first */
    char stack[512];
    int needed = vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);

    if (needed < 0) return;

    if ((size_t)needed < sizeof(stack)) {
        append_str(buf, len, cap, stack);
    } else {
        /* Stack too small — allocate */
        va_start(ap, fmt);
        char *big = malloc((size_t)needed + 1);
        if (!big) { va_end(ap); return; }
        vsnprintf(big, (size_t)needed + 1, fmt, ap);
        va_end(ap);
        append_str(buf, len, cap, big);
        free(big);
    }
}
