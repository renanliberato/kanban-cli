#include "../../src/agent.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* simple assert-based test harness */
static int tests_run   = 0;
static int tests_pass  = 0;
static int tests_fail  = 0;

static char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

#define ASSERT_EQ(a, b, msg) do { \
    tests_run++; \
    if ((int)(a) == (int)(b)) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected %d, got %d)\n", msg, (int)(b), (int)(a)); \
    } \
} while(0)

#define ASSERT_STREQ(a, b, msg) do { \
    tests_run++; \
    if ((a) && (b) && strcmp((a), (b)) == 0) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected '%s', got '%s')\n", msg, (b) ? (b) : "(null)", (a) ? (a) : "(null)"); \
    } \
} while(0)

#define ASSERT_NOTNULL(a, msg) do { \
    tests_run++; \
    if ((a) != NULL) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected non-NULL)\n", msg); \
    } \
} while(0)

#define ASSERT_NULL(a, msg) do { \
    tests_run++; \
    if ((a) == NULL) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected NULL)\n", msg); \
    } \
} while(0)

#define ASSERT_TRUE(cond, msg) do { \
    tests_run++; \
    if ((cond)) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected true)\n", msg); \
    } \
} while(0)

/* ------------------------------------------------------------------ */
/* helpers for writing temp agent files                               */
/* ------------------------------------------------------------------ */

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(content, f);
        fclose(f);
    }
}

static void mkdir_p(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ------------------------------------------------------------------ */
/* test: frontmatter parser (internal, tested via agent_discover)     */
/* ------------------------------------------------------------------ */

static void test_discover_valid_agents(void)
{
    /* Create temp structure: /tmp/agent_test_home/.kanban/agents/ - .md files */
    const char *tmpbase = "/tmp/kanban_agent_test_valid";
    char agents_dir[512];
    snprintf(agents_dir, sizeof(agents_dir), "%s/.kanban/agents", tmpbase);
    mkdir_p(agents_dir);

    /* Write a valid comment agent */
    char path_a[512];
    snprintf(path_a, sizeof(path_a), "%s/analytical.md", agents_dir);
    write_file(path_a,
        "---\n"
        "name: analytical\n"
        "type: comment\n"
        "---\n"
        "You are a product analytics reviewer. Check if the task has "
        "sufficient analytics tracking coverage planned.\n");

    /* Write a valid description agent */
    char path_b[512];
    snprintf(path_b, sizeof(path_b), "%s/writer.md", agents_dir);
    write_file(path_b,
        "---\n"
        "type: description\n"
        "name: writer\n"
        "---\n"
        "You are a technical writer. Update the task description from "
        "the comment thread.\n");

    /* Override HOME to use our temp dir */
    setenv("HOME", "/tmp/kanban_agent_test_valid", 1);

    int count = 0;
    Agent *agents = agent_discover(NULL, &count);
    ASSERT_EQ(count, 2, "discover finds 2 agents");
    ASSERT_NOTNULL(agents, "agents array is non-NULL");

    if (agents) {
        int ai = agent_find(agents, count, "analytical");
        ASSERT_TRUE(ai >= 0, "analytical agent found");
        if (ai >= 0) {
            ASSERT_EQ(agents[ai].type, AGENT_COMMENT,
                      "analytical is comment type");
            ASSERT_TRUE(strstr(agents[ai].prompt_body, "analytics") != NULL,
                        "analytical prompt body contains analytics");
            ASSERT_TRUE(strstr(agents[ai].source_path, "analytical.md") != NULL,
                        "source path contains filename");
        }

        int bi = agent_find(agents, count, "writer");
        ASSERT_TRUE(bi >= 0, "writer agent found");
        if (bi >= 0) {
            ASSERT_EQ(agents[bi].type, AGENT_DESCRIPTION,
                      "writer is description type");
        }

        /* Lookup unknown */
        int ui = agent_find(agents, count, "nonexistent");
        ASSERT_EQ(ui, -1, "unknown agent not found");

        agent_free_list(agents, count);
    }

    /* Cleanup */
    unlink(path_a);
    unlink(path_b);
    rmdir(agents_dir);
    rmdir("/tmp/kanban_agent_test_valid/.kanban");
    rmdir(tmpbase);
}

/* ------------------------------------------------------------------ */
/* test: invalid agents are skipped                                   */
/* ------------------------------------------------------------------ */

static void test_discover_invalid_agents(void)
{
    const char *tmpbase = "/tmp/kanban_agent_test_invalid";
    char agents_dir[512];
    snprintf(agents_dir, sizeof(agents_dir), "%s/.kanban/agents", tmpbase);
    mkdir_p(agents_dir);

    /* Missing name */
    write_file("/tmp/kanban_agent_test_invalid/.kanban/agents/no_name.md",
        "---\n"
        "type: comment\n"
        "---\n"
        "No name here.\n");

    /* Missing type */
    char path_mt[512];
    snprintf(path_mt, sizeof(path_mt), "%s/.kanban/agents/no_type.md", tmpbase);
    write_file(path_mt,
        "---\n"
        "name: bad\n"
        "---\n"
        "No type.\n");

    /* Bad type value */
    char path_bt[512];
    snprintf(path_bt, sizeof(path_bt), "%s/.kanban/agents/bad_type.md", tmpbase);
    write_file(path_bt,
        "---\n"
        "name: badtype\n"
        "type: invalid\n"
        "---\n"
        "Bad type value.\n");

    /* No frontmatter at all */
    char path_nf[512];
    snprintf(path_nf, sizeof(path_nf), "%s/.kanban/agents/no_fm.md", tmpbase);
    write_file(path_nf,
        "Just some text, no frontmatter.\n");

    /* Invalid name (uppercase) */
    char path_in[512];
    snprintf(path_in, sizeof(path_in), "%s/.kanban/agents/BadName.md", tmpbase);
    write_file(path_in,
        "---\n"
        "name: BadName\n"
        "type: comment\n"
        "---\n"
        "Uppercase name.\n");

    setenv("HOME", "/tmp/kanban_agent_test_invalid", 1);

    int count = 0;
    Agent *agents = agent_discover(NULL, &count);
    ASSERT_EQ(count, 0, "discover finds 0 valid agents (all skipped)");
    ASSERT_NULL(agents, "agents array is NULL when count=0");

    /* Cleanup */
    unlink("/tmp/kanban_agent_test_invalid/.kanban/agents/no_name.md");
    unlink(path_mt);
    unlink(path_bt);
    unlink(path_nf);
    unlink(path_in);
    rmdir(agents_dir);
    rmdir("/tmp/kanban_agent_test_invalid/.kanban");
    rmdir(tmpbase);
}

/* ------------------------------------------------------------------ */
/* test: project dir overrides global on name clash                   */
/* ------------------------------------------------------------------ */

static void test_discover_precedence(void)
{
    /* Global agent */
    const char *tmp_global = "/tmp/kanban_agent_test_proj_global";
    char global_agents[512];
    snprintf(global_agents, sizeof(global_agents), "%s/.kanban/agents", tmp_global);
    mkdir_p(global_agents);
    write_file("/tmp/kanban_agent_test_proj_global/.kanban/agents/dup.md",
        "---\n"
        "name: duplicate\n"
        "type: comment\n"
        "---\n"
        "Global default prompt.\n");

    /* Project-local agent (same name, different prompt) */
    const char *tmp_proj = "/tmp/kanban_agent_test_proj_local";
    char proj_agents[512];
    snprintf(proj_agents, sizeof(proj_agents), "%s/agents", tmp_proj);
    mkdir_p(proj_agents);
    write_file("/tmp/kanban_agent_test_proj_local/agents/dup.md",
        "---\n"
        "name: duplicate\n"
        "type: description\n"
        "---\n"
        "Project-local override prompt.\n");

    setenv("HOME", "/tmp/kanban_agent_test_proj_global", 1);

    int count = 0;
    Agent *agents = agent_discover(tmp_proj, &count);
    ASSERT_EQ(count, 1, "precedence: 1 agent found");
    ASSERT_NOTNULL(agents, "agents array non-NULL");

    if (agents && count >= 1) {
        int ai = agent_find(agents, count, "duplicate");
        ASSERT_TRUE(ai >= 0, "duplicate agent found");
        if (ai >= 0) {
            ASSERT_EQ(agents[ai].type, AGENT_DESCRIPTION,
                      "project-local type wins");
            ASSERT_TRUE(strstr(agents[ai].prompt_body, "Project-local") != NULL,
                        "project-local prompt body wins");
        }
        agent_free_list(agents, count);
    }

    /* Cleanup */
    unlink("/tmp/kanban_agent_test_proj_global/.kanban/agents/dup.md");
    rmdir(global_agents);
    rmdir("/tmp/kanban_agent_test_proj_global/.kanban");
    rmdir(tmp_global);
    unlink("/tmp/kanban_agent_test_proj_local/agents/dup.md");
    rmdir(proj_agents);
    rmdir(tmp_proj);
}

/* ------------------------------------------------------------------ */
/* test: @mention scanner                                             */
/* ------------------------------------------------------------------ */

static void test_mention_scanner(void)
{
    Agent *agents = malloc(2 * sizeof(Agent));
    agents[0].name = xstrdup("analytical");
    agents[0].type = AGENT_COMMENT;
    agents[0].prompt_body = xstrdup("test");
    agents[0].source_path = xstrdup("test.md");
    agents[1].name = xstrdup("writer");
    agents[1].type = AGENT_DESCRIPTION;
    agents[1].prompt_body = xstrdup("test");
    agents[1].source_path = xstrdup("test.md");

    int ai, start, len;

    /* Test 1: @mention at start */
    ASSERT_EQ(agent_scan_mention("@analytical check this",
               agents, 2, &ai, &start, &len), 1,
              "finds @analytical at start");
    ASSERT_EQ(ai, 0, "@analytical index is 0");
    ASSERT_EQ(start, 0, "mention starts at 0");
    ASSERT_EQ(len, 11, "mention length is 11 (@ + analytical)");

    /* Test 2: @mention after text */
    ASSERT_EQ(agent_scan_mention("please @writer update this",
               agents, 2, &ai, &start, &len), 1,
              "finds @writer after text");
    ASSERT_EQ(ai, 1, "@writer index is 1");
    ASSERT_EQ(start, 7, "mention starts at 7");

    /* Test 3: @mention preceded by newline (whitespace) */
    ASSERT_EQ(agent_scan_mention("check this\n@analytical review",
               agents, 2, &ai, &start, &len), 1,
              "finds @analytical after newline");
    ASSERT_EQ(start, 11, "mention starts at 11");

    /* Test 4: @ in middle of word — not a mention */
    ASSERT_EQ(agent_scan_mention("foo@analytical bar",
               agents, 2, &ai, &start, &len), 0,
              "no match for @ in middle of word");

    /* Test 5: unknown agent name */
    ASSERT_EQ(agent_scan_mention("@unknown check",
               agents, 2, &ai, &start, &len), 0,
              "no match for unknown agent");

    /* Test 6: @mention at end */
    ASSERT_EQ(agent_scan_mention("fix this @writer",
               agents, 2, &ai, &start, &len), 1,
              "finds @writer at end");
    ASSERT_EQ(len, 7, "mention length (including @)");

    /* Test 7: only @mention */
    ASSERT_EQ(agent_scan_mention("@analytical",
               agents, 2, &ai, &start, &len), 1,
              "finds @analytical alone");

    /* Test 8: no @mention in empty body */
    ASSERT_EQ(agent_scan_mention("", agents, 2, &ai, &start, &len), 0,
              "no match in empty body");

    /* Test 9: first match only — @writer then @analytical */
    ASSERT_EQ(agent_scan_mention("@writer and @analytical",
               agents, 2, &ai, &start, &len), 1,
              "finds first match only");
    ASSERT_EQ(ai, 1, "first match is @writer");

    agent_free_list(agents, 2);
}

/* ------------------------------------------------------------------ */
/* test: prompt builder                                               */
/* ------------------------------------------------------------------ */

static void test_prompt_builder(void)
{
    Agent agent;
    agent.name = "analytical";
    agent.type = AGENT_COMMENT;
    agent.prompt_body = "You are a product analytics reviewer.";

    Card card;
    memset(&card, 0, sizeof(card));
    card.id = 42;
    card.title = "Add login tracking";
    card.description = "Implement analytics events for login flow";
    card.labels = (char *[]){ "analytics", "backend", NULL };
    card.label_count = 2;

    Comment comments[2];
    comments[0].author = "alice";
    comments[0].body = "Should we track both success and failure?";
    comments[0].created_at = "2025-01-01 10:00:00";
    comments[1].author = "bob";
    comments[1].body = "Yes, both with distinct event names.";
    comments[1].created_at = "2025-01-01 11:00:00";

    char *prompt = agent_build_prompt(&agent, &card,
                                       "myboard", "/home/user/project",
                                       "check if we need page-view tracking",
                                       comments, 2, "Doing");

    ASSERT_NOTNULL(prompt, "prompt builder returns non-NULL");

    /* Check all sections are present */
    ASSERT_TRUE(strstr(prompt, "product analytics reviewer") != NULL,
                "prompt contains default body");
    ASSERT_TRUE(strstr(prompt, "check if we need page-view tracking") != NULL,
                "prompt contains user message");
    ASSERT_TRUE(strstr(prompt, "42") != NULL,
                "prompt contains card ID");
    ASSERT_TRUE(strstr(prompt, "Add login tracking") != NULL,
                "prompt contains card title");
    ASSERT_TRUE(strstr(prompt, "analytics") != NULL,
                "prompt contains labels");
    ASSERT_TRUE(strstr(prompt, "alice") != NULL,
                "prompt contains comment author");
    ASSERT_TRUE(strstr(prompt, "bob") != NULL,
                "prompt contains second comment");
    ASSERT_TRUE(strstr(prompt, "myboard") != NULL,
                "prompt contains board name");
    ASSERT_TRUE(strstr(prompt, "Doing") != NULL,
                "prompt contains column name");
    ASSERT_TRUE(strstr(prompt, "/home/user/project") != NULL,
                "prompt contains project dir");
    ASSERT_TRUE(strstr(prompt, "comment text only") != NULL,
                "prompt has comment output instruction");

    free(prompt);

    /* Test description agent output instruction */
    agent.type = AGENT_DESCRIPTION;
    prompt = agent_build_prompt(&agent, &card,
                                 "board", NULL, "update",
                                 NULL, 0, "To Do");
    ASSERT_NOTNULL(prompt, "desc agent prompt non-NULL");
    ASSERT_TRUE(strstr(prompt, "\"title\"") != NULL,
                "desc agent prompt asks for JSON");
    ASSERT_TRUE(strstr(prompt, "\"description\"") != NULL,
                "desc agent prompt asks for description JSON");
    free(prompt);

    /* Test NULL project_dir: no project section */
    prompt = agent_build_prompt(&agent, &card, "b", NULL, "test",
                                 NULL, 0, "To Do");
    ASSERT_TRUE(strstr(prompt, "Project Context") == NULL,
                "no project section when project_dir is NULL");
    free(prompt);
}

/* ------------------------------------------------------------------ */
/* test: description result parser                                    */
/* ------------------------------------------------------------------ */

static void test_parse_description_result(void)
{
    char *title = NULL;
    char *desc = NULL;

    /* Valid JSON */
    int rc = agent_parse_description_result(
        "{\"title\": \"Updated title\", \"description\": \"Updated desc\"}",
        &title, &desc);
    ASSERT_EQ(rc, 0, "parse valid desc result returns 0");
    ASSERT_STREQ(title, "Updated title", "title parsed correctly");
    ASSERT_STREQ(desc, "Updated desc", "desc parsed correctly");
    free(title);
    free(desc);
    title = NULL; desc = NULL;

    /* Missing title */
    rc = agent_parse_description_result(
        "{\"description\": \"Only desc\"}", &title, &desc);
    ASSERT_EQ(rc, 0, "parse missing title returns 0");
    ASSERT_NULL(title, "missing title -> NULL");
    ASSERT_STREQ(desc, "Only desc", "desc parsed");
    free(desc);
    desc = NULL;

    /* Empty title */
    rc = agent_parse_description_result(
        "{\"title\": \"\", \"description\": \"desc\"}", &title, &desc);
    ASSERT_EQ(rc, 0, "parse empty title returns 0");
    ASSERT_NULL(title, "empty title -> NULL");
    ASSERT_STREQ(desc, "desc", "desc parsed for empty title");
    free(desc);
    desc = NULL;

    /* Malformed JSON */
    rc = agent_parse_description_result("{bad", &title, &desc);
    ASSERT_TRUE(rc != 0, "malformed JSON returns error");
    ASSERT_NULL(title, "title NULL on error");
    ASSERT_NULL(desc, "desc NULL on error");

    /* NULL input */
    rc = agent_parse_description_result(NULL, &title, &desc);
    ASSERT_TRUE(rc != 0, "NULL input returns error");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== test_agent ===\n\n");

    test_discover_valid_agents();
    test_discover_invalid_agents();
    test_discover_precedence();
    test_mention_scanner();
    test_prompt_builder();
    test_parse_description_result();

    printf("\n---\n%d tests: %d passed, %d failed\n",
           tests_run, tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
