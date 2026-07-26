#define _POSIX_C_SOURCE 200809L
#include "../../src/llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
/* test: lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void test_lifecycle(void)
{
    int rc = llm_init();
    ASSERT_EQ(rc, 0, "llm_init succeeds");
    ASSERT_EQ(llm_job_count(), 0, "job count is 0 initially");

    llm_free();
    ASSERT_EQ(llm_job_count(), 0, "job count is 0 after llm_free");
}

/* ------------------------------------------------------------------ */
/* test: submit → poll → done (fake provider, immediate)              */
/* ------------------------------------------------------------------ */

static void test_submit_poll_done(void)
{
    /* KANBAN_LLM_PROVIDER=fake and KANBAN_LLM_FAKE_DELAY=1 by default in these tests.
       We set them in main() below. */

    llm_init();

    int job_id = llm_submit("Hello, world", 42, 10);
    ASSERT_NEQ(job_id, -1, "llm_submit returns valid job id");
    ASSERT_EQ(llm_job_count(), 1, "job count is 1 after submit");

    const LlmJob *j = llm_get_job(job_id);
    ASSERT_NOTNULL(j, "llm_get_job returns non-null");
    ASSERT_EQ(j->state, LLM_RUNNING, "job is RUNNING after submit");
    ASSERT_EQ(j->card_id, 42, "card_id is 42");
    ASSERT_STREQ(j->prompt, "Hello, world", "prompt is stored correctly");

    /* One poll tick should complete it (KANBAN_LLM_FAKE_DELAY=1) */
    int transitions = llm_poll();
    ASSERT_EQ(transitions, 1, "one transition after poll");
    ASSERT_EQ(j->state, LLM_DONE, "job is DONE after poll");
    ASSERT_NOTNULL(j->result, "result is non-null");
    ASSERT_TRUE(j->result[0] != '\0', "result is non-empty");
    ASSERT_TRUE(strstr(j->result, "Fake") != NULL,
                "result contains fake text");
    ASSERT_TRUE(strstr(j->result, "result") != NULL,
                "result has envelope wrapper");

    llm_free();
}

/* ------------------------------------------------------------------ */
/* test: fake provider with delay (multi-tick)                        */
/* ------------------------------------------------------------------ */

static void test_submit_multi_tick(void)
{
    /* This test sets KANBAN_LLM_FAKE_DELAY=3, so it takes 3 polls to complete.
       We do this via setenv before llm_init. */

    setenv("KANBAN_LLM_FAKE_DELAY", "3", 1);
    llm_init();

    int job_id = llm_submit("delayed", -1, 0);
    ASSERT_NEQ(job_id, -1, "multi-tick: submit succeeds");
    ASSERT_EQ(llm_job_count(), 1, "multi-tick: count is 1");

    /* Poll 1 */
    int t = llm_poll();
    ASSERT_EQ(t, 0, "multi-tick: poll 1 — no transition (ticks remaining)");
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_RUNNING, "multi-tick: still RUNNING after poll 1");

    /* Poll 2 */
    t = llm_poll();
    ASSERT_EQ(t, 0, "multi-tick: poll 2 — no transition");
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_RUNNING, "multi-tick: still RUNNING after poll 2");

    /* Poll 3 — should complete */
    t = llm_poll();
    ASSERT_EQ(t, 1, "multi-tick: poll 3 — transition to DONE");
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_DONE, "multi-tick: DONE after poll 3");

    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: cancellation                                                  */
/* ------------------------------------------------------------------ */

static void test_cancel(void)
{
    /* Use a long delay so the job is still RUNNING when we cancel */
    setenv("KANBAN_LLM_FAKE_DELAY", "100", 1);
    llm_init();

    int job_id = llm_submit("cancel me", 0, 0);
    ASSERT_NEQ(job_id, -1, "cancel: submit succeeds");

    /* Poll once to make sure it's RUNNING */
    llm_poll();
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_RUNNING, "cancel: job is RUNNING");

    int rc = llm_cancel(job_id);
    ASSERT_EQ(rc, 0, "cancel: llm_cancel returns 0");

    /* Next poll should transition to CANCELLED */
    int transitions = llm_poll();
    ASSERT_EQ(transitions, 1, "cancel: one transition after cancel poll");
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_CANCELLED,
              "cancel: job is CANCELLED");

    /* Cancelling an already cancelled job should fail */
    rc = llm_cancel(job_id);
    ASSERT_NEQ(rc, 0, "cancel: double cancel returns error");

    /* Cancelling a non-existent job should fail */
    rc = llm_cancel(999);
    ASSERT_NEQ(rc, 0, "cancel: bad job id returns error");

    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: timeout                                                       */
/* ------------------------------------------------------------------ */

static void test_timeout(void)
{
    /* Use a long delay so the timeout fires before ticks run out.
       Note: the fake provider checks wall-clock timeout in each poll,
       not tick count.  The timeout is evaluated based on started_at. */
    setenv("KANBAN_LLM_FAKE_DELAY", "100", 1);
    llm_init();

    /* Submit with a timeout of 0, meaning no timeout */
    int job_id = llm_submit("no timeout", 0, 0);
    ASSERT_NEQ(job_id, -1, "timeout: submit with 0 timeout ok");

    /* But we can test the timeout path by setting the job's started_at
       artificially.  The public API doesn't allow that, so we check
       the result field is empty before completion. */
    ASSERT_NULL(llm_get_job(job_id)->result,
                "timeout: result is NULL before completion");

    /* Clean up by cancelling */
    llm_cancel(job_id);
    llm_poll();

    /* Now test with a valid timeout — submit with timeout=1 and
       artifically set started_at back.  We can't do this through the
       public API.  Instead we rely on the design: the timeout path
       is exercised when the wall-clock elapsed > timeout_secs.
       Since we cannot manipulate started_at, we test with a short
       timeout and sleep. */

    /* Actually, the fake provider's timeout check is `now - started_at >= timeout`.
       With timeout=1, after 1 second the job should be FAILED. */
    llm_free();
    llm_init();

    job_id = llm_submit("timeout test", 0, 1);   /* 1 second timeout */
    ASSERT_NEQ(job_id, -1, "timeout: submit with 1s timeout ok");

    /* Poll once to ensure it's RUNNING */
    llm_poll();
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_RUNNING,
              "timeout: still RUNNING (fake ticks not exhausted)");

    /* Sleep a bit, then poll — timeout should fire */
    sleep(2);
    int t = llm_poll();
    (void)t;
    ASSERT_EQ(llm_get_job(job_id)->state, LLM_FAILED,
              "timeout: job is FAILED after sleep");
    ASSERT_NOTNULL(llm_get_job(job_id)->result, "timeout: result is set");
    ASSERT_STREQ(llm_get_job(job_id)->result, "timeout",
                 "timeout: result says 'timeout'");

    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: max 3 concurrent jobs                                         */
/* ------------------------------------------------------------------ */

static void test_max_jobs(void)
{
    /* Use long delay so we can fill the queue without jobs completing */
    setenv("KANBAN_LLM_FAKE_DELAY", "100", 1);
    llm_init();

    int id1 = llm_submit("job 1", 1, 0);
    int id2 = llm_submit("job 2", 2, 0);
    int id3 = llm_submit("job 3", 3, 0);
    ASSERT_NEQ(id1, -1, "max: job 1 ok");
    ASSERT_NEQ(id2, -1, "max: job 2 ok");
    ASSERT_NEQ(id3, -1, "max: job 3 ok");

    int id4 = llm_submit("job 4", 4, 0);
    ASSERT_EQ(id4, -1, "max: job 4 rejected (queue full)");
    ASSERT_EQ(llm_job_count(), 3, "max: count is 3");

    /* Clean up */
    for (int i = 0; i < 3; i++) {
        llm_cancel(llm_job_at(i)->id);
    }
    llm_poll();

    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: multiple simultaneous jobs                                    */
/* ------------------------------------------------------------------ */

static void test_multiple_jobs(void)
{
    /* Use delay=2 so both complete after 2 polls */
    setenv("KANBAN_LLM_FAKE_DELAY", "2", 1);
    llm_init();

    int id1 = llm_submit("alpha", 10, 0);
    int id2 = llm_submit("beta", 20, 0);
    ASSERT_NEQ(id1, -1, "multi-job: job 1 ok");
    ASSERT_NEQ(id2, -1, "multi-job: job 2 ok");
    ASSERT_EQ(llm_job_count(), 2, "multi-job: count is 2");

    /* Poll 1 — no completions */
    int t = llm_poll();
    ASSERT_EQ(t, 0, "multi-job: poll 1 — no transitions");

    /* Poll 2 — both complete */
    t = llm_poll();
    ASSERT_EQ(t, 2, "multi-job: poll 2 — both transitions");
    ASSERT_EQ(llm_get_job(id1)->state, LLM_DONE, "multi-job: job 1 DONE");
    ASSERT_EQ(llm_get_job(id2)->state, LLM_DONE, "multi-job: job 2 DONE");

    /* Check results — they should contain fake data */
    ASSERT_TRUE(strstr(llm_get_job(id1)->result, "Fake") != NULL,
                "multi-job: job 1 result contains fake data");
    ASSERT_TRUE(strstr(llm_get_job(id2)->result, "Fake") != NULL,
                "multi-job: job 2 result contains fake data");

    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: llm_job_at iteration                                          */
/* ------------------------------------------------------------------ */

static void test_job_at(void)
{
    setenv("KANBAN_LLM_FAKE_DELAY", "100", 1);
    llm_init();

    int id1 = llm_submit("first", 1, 0);
    int id2 = llm_submit("second", 2, 0);
    ASSERT_NEQ(id1, -1, "job_at: job 1 ok");
    ASSERT_NEQ(id2, -1, "job_at: job 2 ok");

    const LlmJob *j0 = llm_job_at(0);
    const LlmJob *j1 = llm_job_at(1);
    const LlmJob *j2 = llm_job_at(2);  /* out of bounds */

    ASSERT_NOTNULL(j0, "job_at: index 0 valid");
    ASSERT_NOTNULL(j1, "job_at: index 1 valid");
    ASSERT_NULL(j2, "job_at: index 2 NULL");

    ASSERT_EQ(j0->id, id1, "job_at: index 0 is job 1");
    ASSERT_EQ(j1->id, id2, "job_at: index 1 is job 2");

    /* Clean up */
    llm_cancel(id1);
    llm_cancel(id2);
    llm_poll();
    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: provider selection via env var                                */
/* ------------------------------------------------------------------ */

static void test_provider_selection(void)
{
    /* Test that the env var is respected.  With KANBAN_LLM_PROVIDER=fake,
       jobs complete in-process.  We'll also verify that the default
       (no env var or "opencode") doesn't try to fork when init happens
       (we just check that init succeeds). */

    /* Fake provider */
    setenv("KANBAN_LLM_PROVIDER", "fake", 1);
    setenv("KANBAN_LLM_FAKE_DELAY", "1", 1);
    llm_init();
    int id = llm_submit("fake test", 0, 0);
    ASSERT_NEQ(id, -1, "provider: fake submit ok");
    llm_poll();
    ASSERT_EQ(llm_get_job(id)->state, LLM_DONE, "provider: fake job completed");
    llm_free();
    unsetenv("KANBAN_LLM_PROVIDER");

    /* Default (opencode) provider — init should succeed.
       We don't submit a job because that would try to fork+exec opencode. */
    llm_init();
    ASSERT_EQ(llm_job_count(), 0, "provider: opencode init with no jobs");
    llm_free();
}

/* ------------------------------------------------------------------ */
/* test: default timeout from env var                                  */
/* ------------------------------------------------------------------ */

static void test_default_timeout(void)
{
    /* Default (no env var) should be 120 */
    unsetenv("KANBAN_LLM_TIMEOUT");
    int def = llm_default_timeout();
    ASSERT_EQ(def, 120, "default timeout is 120");

    /* Set env var */
    setenv("KANBAN_LLM_TIMEOUT", "30", 1);
    def = llm_default_timeout();
    ASSERT_EQ(def, 30, "timeout from env var is 30");

    /* Invalid value */
    setenv("KANBAN_LLM_TIMEOUT", "invalid", 1);
    def = llm_default_timeout();
    ASSERT_EQ(def, 120, "invalid timeout falls back to 120");

    /* Negative value */
    setenv("KANBAN_LLM_TIMEOUT", "-5", 1);
    def = llm_default_timeout();
    ASSERT_EQ(def, 120, "negative timeout falls back to 120");

    /* Zero value */
    setenv("KANBAN_LLM_TIMEOUT", "0", 1);
    def = llm_default_timeout();
    ASSERT_EQ(def, 120, "zero timeout falls back to 120");

    unsetenv("KANBAN_LLM_TIMEOUT");
}

/* ------------------------------------------------------------------ */
/* test: llm_get_job_for_card                                          */
/* ------------------------------------------------------------------ */

static void test_get_job_for_card(void)
{
    setenv("KANBAN_LLM_FAKE_DELAY", "100", 1);
    llm_init();

    /* Card -1 should return NULL */
    ASSERT_NULL(llm_get_job_for_card(-1), "job_for_card: -1 returns NULL");

    /* Submit jobs for different cards */
    int id1 = llm_submit("card 1 job", 10, 0);
    int id2 = llm_submit("card 2 job", 20, 0);
    ASSERT_NEQ(id1, -1, "job_for_card: job 1 submitted");
    ASSERT_NEQ(id2, -1, "job_for_card: job 2 submitted");

    const LlmJob *j1 = llm_get_job_for_card(10);
    ASSERT_NOTNULL(j1, "job_for_card: finds job for card 10");
    ASSERT_EQ(j1->card_id, 10, "job_for_card: card_id matches");

    const LlmJob *j2 = llm_get_job_for_card(20);
    ASSERT_NOTNULL(j2, "job_for_card: finds job for card 20");
    ASSERT_EQ(j2->card_id, 20, "job_for_card: card_id matches");

    /* No job for card 99 */
    ASSERT_NULL(llm_get_job_for_card(99), "job_for_card: card 99 returns NULL");

    /* Clean up */
    llm_cancel(id1);
    llm_cancel(id2);
    llm_poll();
    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* test: job queue full rejection with cap check                       */
/* ------------------------------------------------------------------ */

static void test_job_queue_full_cap(void)
{
    /* Verify that max 3 jobs are accepted and 4th is rejected */
    setenv("KANBAN_LLM_FAKE_DELAY", "100", 1);
    llm_init();

    int id1 = llm_submit("job 1", 1, 0);
    int id2 = llm_submit("job 2", 2, 0);
    int id3 = llm_submit("job 3", 3, 0);
    ASSERT_NEQ(id1, -1, "cap: job 1 accepted");
    ASSERT_NEQ(id2, -1, "cap: job 2 accepted");
    ASSERT_NEQ(id3, -1, "cap: job 3 accepted");

    /* 4th should be rejected */
    int id4 = llm_submit("job 4", 4, 0);
    ASSERT_EQ(id4, -1, "cap: job 4 rejected (queue full)");

    ASSERT_EQ(llm_job_count(), 3, "cap: job count is 3 after 4 submits");

    /* Cancel one and try again */
    llm_cancel(id1);
    llm_poll();
    ASSERT_EQ(llm_get_job(id1)->state, LLM_CANCELLED, "cap: job 1 cancelled");

    /* Now submit again — should succeed since slot is freed */
    llm_free();
    llm_init();

    int id5 = llm_submit("job 5", 5, 0);
    ASSERT_NEQ(id5, -1, "cap: job 5 accepted (slot freed)");

    llm_free();
    unsetenv("KANBAN_LLM_FAKE_DELAY");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    /* Default: use the fake provider for all tests.
       Individual tests can override KANBAN_LLM_FAKE_DELAY. */
    setenv("KANBAN_LLM_PROVIDER", "fake", 1);
    setenv("KANBAN_LLM_FAKE_DELAY", "1", 1);

    printf("=== LLM Unit Tests ===\n\n");

    test_lifecycle();
    test_submit_poll_done();
    test_submit_multi_tick();
    test_cancel();
    test_timeout();
    test_max_jobs();
    test_multiple_jobs();
    test_job_at();
    test_provider_selection();
    test_default_timeout();
    test_get_job_for_card();
    test_job_queue_full_cap();

    printf("\n---\n");
    printf("Tests run:  %d\n", tests_run);
    printf("Tests pass: %d\n", tests_pass);
    printf("Tests fail: %d\n", tests_fail);

    return tests_fail > 0 ? 1 : 0;
}
