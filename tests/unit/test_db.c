#include "../../src/db.h"
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
    if ((a) == (b)) { \
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

static void cleanup(const char *db_path)
{
    unlink(db_path);
    /* also -wal and -shm */
    size_t len = strlen(db_path);
    char *wal = malloc(len + 5);
    char *shm = malloc(len + 5);
    if (wal) {
        memcpy(wal, db_path, len);
        memcpy(wal + len, "-wal", 5);
        unlink(wal);
        free(wal);
    }
    if (shm) {
        memcpy(shm, db_path, len);
        memcpy(shm + len, "-shm", 5);
        unlink(shm);
        free(shm);
    }
}

/* ------------------------------------------------------------------ */

static void test_fresh_db_creation(void)
{
    const char *path = "/tmp/test_db_fresh.db";
    cleanup(path);

    db_t *db = db_open(path);
    ASSERT_NOTNULL(db, "db_open creates a new database");
    ASSERT_TRUE(db_has_migration(db, 1), "schema v1 migration recorded");

    /* load empty board from fresh db */
    int next_id = 0;
    int col_counts[3] = {0, 0, 0};
    int *ids[3] = {NULL, NULL, NULL};
    char **titles[3] = {NULL, NULL, NULL};

    int rc = db_load_board(db, &next_id, col_counts, ids, titles,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    ASSERT_EQ(rc, 0, "db_load_board succeeds on fresh db");
    ASSERT_EQ(next_id, 1, "fresh db has next_id=1");
    ASSERT_EQ(col_counts[0], 0, "fresh db: To Do empty");
    ASSERT_EQ(col_counts[1], 0, "fresh db: Doing empty");
    ASSERT_EQ(col_counts[2], 0, "fresh db: Done empty");

    db_close(db);
    cleanup(path);
}

static void test_schema_version_recorded(void)
{
    const char *path = "/tmp/test_db_schema.db";
    cleanup(path);

    db_t *db = db_open(path);
    ASSERT_NOTNULL(db, "db_open succeeds for schema test");
    ASSERT_TRUE(db_has_migration(db, 1), "schema v1 is recorded after db_open");

    db_close(db);

    /* reopen — migration should still be there */
    db = db_open(path);
    ASSERT_NOTNULL(db, "reopen succeeds");
    ASSERT_TRUE(db_has_migration(db, 1), "schema v1 persists across reopen");

    db_close(db);
    cleanup(path);
}

static void test_crud_roundtrip(void)
{
    const char *path = "/tmp/test_db_crud.db";
    cleanup(path);

    db_t *db = db_open(path);
    ASSERT_NOTNULL(db, "db_open succeeds");

    /* add cards via incremental API */
    ASSERT_EQ(db_add_card(db, 0, 1, "alpha"), 0, "db_add_card: alpha in To Do");
    ASSERT_EQ(db_add_card(db, 0, 2, "beta"), 0, "db_add_card: beta in To Do");
    ASSERT_EQ(db_add_card(db, 1, 3, "gamma"), 0, "db_add_card: gamma in Doing");
    ASSERT_EQ(db_add_card(db, 2, 4, "delta"), 0, "db_add_card: delta in Done");

    /* edit a card */
    ASSERT_EQ(db_edit_card_title(db, 2, "beta-edited"), 0, "db_edit_card_title succeeds");

    /* move a card */
    ASSERT_EQ(db_move_card(db, 3, 2), 0, "db_move_card: gamma to Done");

    /* delete a card */
    ASSERT_EQ(db_delete_card(db, 4), 0, "db_delete_card succeeds");

    /* now load and verify */
    int next_id = 0;
    int col_counts[3] = {0, 0, 0};
    int *ids[3] = {NULL, NULL, NULL};
    char **titles[3] = {NULL, NULL, NULL};

    int rc = db_load_board(db, &next_id, col_counts, ids, titles,
                           NULL, NULL, NULL, NULL, NULL, NULL);
    ASSERT_EQ(rc, 0, "db_load_board succeeds after mutations");

    /* next_id should be max(id)+1 = 4+1 = 5 */
    ASSERT_EQ(next_id, 5, "next_id computed correctly");

    /* To Do: alpha (id=1), beta-edited (id=2) */
    ASSERT_EQ(col_counts[0], 2, "To Do has 2 cards");
    ASSERT_EQ(ids[0][0], 1, "card 0 id=1");
    ASSERT_STREQ(titles[0][0], "alpha", "card 0 title 'alpha'");
    ASSERT_EQ(ids[0][1], 2, "card 1 id=2");
    ASSERT_STREQ(titles[0][1], "beta-edited", "card 1 title 'beta-edited'");

    /* Doing: empty (gamma moved) */
    ASSERT_EQ(col_counts[1], 0, "Doing empty after move");

    /* Done: gamma (id=3) — delta was deleted */
    ASSERT_EQ(col_counts[2], 1, "Done has 1 card");
    ASSERT_EQ(ids[2][0], 3, "card in Done id=3");
    ASSERT_STREQ(titles[2][0], "gamma", "card in Done title 'gamma'");

    /* cleanup */
    for (int ci = 0; ci < 3; ci++) {
        for (int i = 0; i < col_counts[ci]; i++)
            free(titles[ci][i]);
        free(titles[ci]);
        free(ids[ci]);
    }

    db_close(db);
    cleanup(path);
}

static void test_reopen_persistence(void)
{
    const char *path = "/tmp/test_db_reopen.db";
    cleanup(path);

    /* create + add cards */
    db_t *db = db_open(path);
    ASSERT_NOTNULL(db, "db_open succeeds");
    db_add_card(db, 0, 1, "hello");
    db_add_card(db, 2, 2, "world");

    /* verify data is there before closing */
    int next_id = 0;
    int col_counts[3] = {0, 0, 0};
    int *ids[3] = {NULL, NULL, NULL};
    char **titles[3] = {NULL, NULL, NULL};
    db_load_board(db, &next_id, col_counts, ids, titles,
                  NULL, NULL, NULL, NULL, NULL, NULL);
    ASSERT_EQ(col_counts[0], 1, "first open: To Do has 1 card");
    ASSERT_STREQ(titles[0][0], "hello", "first open: card is 'hello'");
    for (int ci = 0; ci < 3; ci++) {
        for (int i = 0; i < col_counts[ci]; i++) free(titles[ci][i]);
        free(titles[ci]); free(ids[ci]);
    }
    db_close(db);

    /* reopen and verify */
    db = db_open(path);
    ASSERT_NOTNULL(db, "reopen succeeds");

    next_id = 0;
    memset(col_counts, 0, sizeof(col_counts));
    memset(ids, 0, sizeof(ids));
    memset(titles, 0, sizeof(titles));
    db_load_board(db, &next_id, col_counts, ids, titles,
                  NULL, NULL, NULL, NULL, NULL, NULL);

    ASSERT_EQ(next_id, 3, "reopen: next_id=3");
    ASSERT_EQ(col_counts[0], 1, "reopen: To Do has 1 card");
    ASSERT_EQ(col_counts[2], 1, "reopen: Done has 1 card");
    ASSERT_STREQ(titles[0][0], "hello", "reopen: card 'hello' persists");
    ASSERT_STREQ(titles[2][0], "world", "reopen: card 'world' persists");

    for (int ci = 0; ci < 3; ci++) {
        for (int i = 0; i < col_counts[ci]; i++) free(titles[ci][i]);
        free(titles[ci]); free(ids[ci]);
    }

    db_close(db);
    cleanup(path);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    setbuf(stdout, NULL);  /* unbuffered output for debugging */
    printf("=== test_db ===\n\n");

    test_fresh_db_creation();
    test_schema_version_recorded();
    test_crud_roundtrip();
    test_reopen_persistence();

    printf("\n---\n%d tests: %d passed, %d failed\n",
           tests_run, tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
