#include "../../src/enrich.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* simple assert-based test harness */
static int tests_run   = 0;
static int tests_pass  = 0;
static int tests_fail  = 0;

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

#define ASSERT_NEQ(a, b, msg) do { \
    tests_run++; \
    if ((int)(a) != (int)(b)) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected != %d, got %d)\n", msg, (int)(b), (int)(a)); \
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
/* test: prompt builder creates a valid prompt                        */
/* ------------------------------------------------------------------ */

static void test_prompt_builder(void)
{
    char *prompt = enrich_build_prompt("Fix login bug", NULL);
    ASSERT_NOTNULL(prompt, "prompt builder returns non-NULL");
    ASSERT_TRUE(strstr(prompt, "Fix login bug") != NULL,
                "prompt contains title");
    ASSERT_TRUE(strstr(prompt, "description") != NULL,
                "prompt asks for description");
    ASSERT_TRUE(strstr(prompt, "labels") != NULL,
                "prompt asks for labels");
    ASSERT_TRUE(strstr(prompt, "questions") != NULL,
                "prompt asks for questions");
    free(prompt);

    /* with existing description */
    prompt = enrich_build_prompt("Task", "Already has description");
    ASSERT_NOTNULL(prompt, "prompt with desc returns non-NULL");
    ASSERT_TRUE(strstr(prompt, "Already has description") != NULL,
                "prompt includes existing description");
    free(prompt);

    /* NULL title */
    ASSERT_NULL(enrich_build_prompt(NULL, NULL),
                "prompt builder returns NULL for NULL title");
}

/* ------------------------------------------------------------------ */
/* test: envelope unwrapping                                          */
/* ------------------------------------------------------------------ */

static void test_unwrap_envelope(void)
{
    /* Plain JSON (no envelope) -> returned as-is */
    char *unwrapped = enrich_unwrap_envelope("{\"key\":\"value\"}");
    ASSERT_NOTNULL(unwrapped, "unwrap plain JSON returns non-NULL");
    ASSERT_STREQ(unwrapped, "{\"key\":\"value\"}",
                 "unwrap plain JSON returns same string");
    free(unwrapped);

    /* Envelope with "result" key */
    unwrapped = enrich_unwrap_envelope(
        "{\"result\": \"{\\\"description\\\":\\\"hello\\\"}\"}");
    ASSERT_NOTNULL(unwrapped, "unwrap result envelope returns non-NULL");
    ASSERT_STREQ(unwrapped, "{\"description\":\"hello\"}",
                 "unwrap extracts result key");
    free(unwrapped);

    /* Envelope with "output" key */
    unwrapped = enrich_unwrap_envelope(
        "{\"output\": \"{\\\"labels\\\":[\\\"bug\\\"]}\"}");
    ASSERT_NOTNULL(unwrapped, "unwrap output envelope returns non-NULL");
    ASSERT_STREQ(unwrapped, "{\"labels\":[\"bug\"]}",
                 "unwrap extracts output key");
    free(unwrapped);

    /* Malformed JSON -> returns a copy of the raw text */
    unwrapped = enrich_unwrap_envelope("not json at all");
    ASSERT_NOTNULL(unwrapped, "unwrap malformed returns non-NULL");
    ASSERT_STREQ(unwrapped, "not json at all",
                 "unwrap returns raw text for malformed JSON");
    free(unwrapped);

    /* NULL/empty input */
    ASSERT_NULL(enrich_unwrap_envelope(NULL),
                "unwrap NULL returns NULL");
    ASSERT_NULL(enrich_unwrap_envelope(""),
                "unwrap empty string returns NULL");
}

/* ------------------------------------------------------------------ */
/* test: enrich_parse_result — valid JSON                             */
/* ------------------------------------------------------------------ */

static void test_parse_valid(void)
{
    const char *json =
        "{"
        "\"description\": \"Implement OAuth login flow\","
        "\"labels\": [\"auth\", \"backend\"],"
        "\"questions\": ["
        "  {\"q\": \"Which OAuth provider?\", \"a\": \"Google\"},"
        "  {\"q\": \"Scope required?\", \"a\": \"email profile\"}"
        "]"
        "}";

    EnrichResult r;
    int rc = enrich_parse_result(json, &r);
    ASSERT_EQ(rc, 0, "parse valid JSON returns 0");
    ASSERT_STREQ(r.description, "Implement OAuth login flow",
                 "description parsed correctly");
    ASSERT_EQ(r.label_count, 2, "2 labels parsed");
    ASSERT_STREQ(r.labels[0], "auth", "first label is 'auth'");
    ASSERT_STREQ(r.labels[1], "backend", "second label is 'backend'");
    ASSERT_NULL(r.labels[2], "labels array is NULL-terminated");
    ASSERT_EQ(r.question_count, 2, "2 questions parsed");
    ASSERT_STREQ(r.questions[0], "Which OAuth provider?",
                 "first question correct");
    ASSERT_STREQ(r.answers[0], "Google", "first answer correct");
    ASSERT_STREQ(r.questions[1], "Scope required?",
                 "second question correct");
    ASSERT_STREQ(r.answers[1], "email profile", "second answer correct");
    ASSERT_NULL(r.questions[2], "questions array is NULL-terminated");
    ASSERT_NULL(r.answers[2], "answers array is NULL-terminated");
    enrich_free_result(&r);
}

/* ------------------------------------------------------------------ */
/* test: enrich_parse_result — empty fields                           */
/* ------------------------------------------------------------------ */

static void test_parse_empty_fields(void)
{
    const char *json =
        "{"
        "\"description\": \"\","
        "\"labels\": [],"
        "\"questions\": []"
        "}";

    EnrichResult r;
    int rc = enrich_parse_result(json, &r);
    ASSERT_EQ(rc, 0, "parse empty fields returns 0");
    ASSERT_NULL(r.description, "empty description -> NULL");
    ASSERT_EQ(r.label_count, 0, "empty labels -> 0 count");
    ASSERT_NULL(r.labels, "empty labels -> NULL array");
    ASSERT_EQ(r.question_count, 0, "empty questions -> 0 count");
    enrich_free_result(&r);
}

/* ------------------------------------------------------------------ */
/* test: enrich_parse_result — missing fields                         */
/* ------------------------------------------------------------------ */

static void test_parse_missing_fields(void)
{
    /* Only description, no labels or questions */
    const char *json = "{\"description\": \"Just a description\"}";

    EnrichResult r;
    int rc = enrich_parse_result(json, &r);
    ASSERT_EQ(rc, 0, "parse missing fields returns 0");
    ASSERT_STREQ(r.description, "Just a description",
                 "description parsed");
    ASSERT_EQ(r.label_count, 0, "missing labels -> 0");
    ASSERT_EQ(r.question_count, 0, "missing questions -> 0");
    enrich_free_result(&r);

    /* Only labels, no description or questions */
    json = "{\"labels\": [\"single\"]}";
    rc = enrich_parse_result(json, &r);
    ASSERT_EQ(rc, 0, "parse labels-only returns 0");
    ASSERT_NULL(r.description, "missing description -> NULL");
    ASSERT_EQ(r.label_count, 1, "1 label");
    ASSERT_STREQ(r.labels[0], "single", "label correct");
    ASSERT_EQ(r.question_count, 0, "no questions");
    enrich_free_result(&r);
}

/* ------------------------------------------------------------------ */
/* test: enrich_parse_result — malformed JSON                         */
/* ------------------------------------------------------------------ */

static void test_parse_malformed(void)
{
    /* Malformed JSON */
    EnrichResult r;
    int rc = enrich_parse_result("{bad json", &r);
    ASSERT_NEQ(rc, 0, "parse malformed JSON returns error");
    enrich_free_result(&r);

    /* NULL input */
    rc = enrich_parse_result(NULL, &r);
    ASSERT_NEQ(rc, 0, "parse NULL returns error");

    /* Empty string */
    rc = enrich_parse_result("", &r);
    ASSERT_NEQ(rc, 0, "parse empty string returns error");

    /* Wrong types: description is a number */
    rc = enrich_parse_result("{\"description\": 42}", &r);
    ASSERT_EQ(rc, 0, "parse wrong types still succeeds (ignores wrong type)");
    ASSERT_NULL(r.description, "non-string description -> NULL");
    enrich_free_result(&r);
}

/* ------------------------------------------------------------------ */
/* test: end-to-end: unwrap + parse                                   */
/* ------------------------------------------------------------------ */

static void test_unwrap_and_parse(void)
{
    /* Simulate the full pipeline: raw LLM output -> unwrap -> parse */
    const char *raw_output =
        "{\"result\": \""
        "{\\\"description\\\":\\\"Refactor auth module\\\","
        "\\\"labels\\\":[\\\"refactor\\\",\\\"tech-debt\\\"],"
        "\\\"questions\\\":["
        "{\\\"q\\\":\\\"Which files?\\\",\\\"a\\\":\\\"auth.c, auth.h\\\"}"
        "]}\""
        "}";

    char *inner = enrich_unwrap_envelope(raw_output);
    ASSERT_NOTNULL(inner, "unwrap in pipeline succeeds");

    EnrichResult r;
    int rc = enrich_parse_result(inner, &r);
    ASSERT_EQ(rc, 0, "parse in pipeline succeeds");
    ASSERT_STREQ(r.description, "Refactor auth module",
                 "pipeline description correct");
    ASSERT_EQ(r.label_count, 2, "pipeline 2 labels");
    ASSERT_STREQ(r.labels[0], "refactor", "pipeline first label");
    ASSERT_EQ(r.question_count, 1, "pipeline 1 question");
    ASSERT_STREQ(r.questions[0], "Which files?", "pipeline question");

    enrich_free_result(&r);
    free(inner);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== test_enrich ===\n\n");

    test_prompt_builder();
    test_unwrap_envelope();
    test_parse_valid();
    test_parse_empty_fields();
    test_parse_missing_fields();
    test_parse_malformed();
    test_unwrap_and_parse();

    printf("\n---\n%d tests: %d passed, %d failed\n",
           tests_run, tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
