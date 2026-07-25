#include "../../src/board.h"
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

/* helper: derive .db path from a .json path for cleanup */
static char *derive_db_path(const char *json_path)
{
    size_t len = strlen(json_path);
    char *db_path = malloc(len + 2);
    if (!db_path) return NULL;
    memcpy(db_path, json_path, len - 5);  /* strip ".json" */
    memcpy(db_path + len - 5, ".db", 4);
    return db_path;
}

/* helper: clean up both .json and .db files */
static void cleanup(const char *json_path)
{
    unlink(json_path);
    char *db_path = derive_db_path(json_path);
    if (db_path) {
        unlink(db_path);
        /* also try -wal and -shm WAL files */
        size_t dblen = strlen(db_path);
        char *wal = malloc(dblen + 5);
        char *shm = malloc(dblen + 5);
        if (wal) {
            memcpy(wal, db_path, dblen);
            memcpy(wal + dblen, "-wal", 5);
            unlink(wal);
            free(wal);
        }
        if (shm) {
            memcpy(shm, db_path, dblen);
            memcpy(shm + dblen, "-shm", 5);
            unlink(shm);
            free(shm);
        }
        free(db_path);
    }
}

/* ------------------------------------------------------------------ */

static void test_add_cards(void)
{
    Board b = board_new();
    ASSERT_EQ(b.columns[COL_TODO].count, 0, "board starts empty (To Do)");
    ASSERT_EQ(b.columns[COL_DOING].count, 0, "board starts empty (Doing)");
    ASSERT_EQ(b.columns[COL_DONE].count, 0, "board starts empty (Done)");

    int id1 = board_add_card(&b, COL_TODO, "task one");
    ASSERT_EQ(id1, 1, "first card id is 1");
    ASSERT_EQ(b.columns[COL_TODO].count, 1, "To Do has 1 card after add");
    ASSERT_STREQ(b.columns[COL_TODO].cards[0].title, "task one", "card title matches");

    int id2 = board_add_card(&b, COL_DOING, "task two");
    ASSERT_EQ(id2, 2, "second card id is 2");
    ASSERT_EQ(b.columns[COL_DOING].count, 1, "Doing has 1 card after add");

    board_free(&b);
}

static void test_delete_card(void)
{
    Board b = board_new();
    board_add_card(&b, COL_TODO, "keep");
    board_add_card(&b, COL_TODO, "remove");
    ASSERT_EQ(b.columns[COL_TODO].count, 2, "To Do has 2 cards");

    int ret = board_delete_card(&b, 2);
    ASSERT_EQ(ret, 0, "delete_card returns 0 on success");
    ASSERT_EQ(b.columns[COL_TODO].count, 1, "To Do has 1 card after delete");
    ASSERT_STREQ(b.columns[COL_TODO].cards[0].title, "keep", "remaining card is 'keep'");

    /* delete non-existent card */
    ret = board_delete_card(&b, 999);
    ASSERT_EQ(ret, -1, "delete_card returns -1 for missing id");

    board_free(&b);
}

static void test_move_card(void)
{
    Board b = board_new();
    int id = board_add_card(&b, COL_TODO, "movable");
    ASSERT_EQ(b.columns[COL_TODO].count, 1, "To Do has 1 card before move");
    ASSERT_EQ(b.columns[COL_DOING].count, 0, "Doing is empty before move");

    int ret = board_move_card(&b, id, COL_DOING);
    ASSERT_EQ(ret, 0, "move_card returns 0 on success");
    ASSERT_EQ(b.columns[COL_TODO].count, 0, "To Do empty after move");
    ASSERT_EQ(b.columns[COL_DOING].count, 1, "Doing has 1 card after move");
    ASSERT_STREQ(b.columns[COL_DOING].cards[0].title, "movable", "moved card title correct");

    /* move to same column (no-op) */
    ret = board_move_card(&b, id, COL_DOING);
    ASSERT_EQ(ret, 0, "move to same column returns 0");
    ASSERT_EQ(b.columns[COL_DOING].count, 1, "count unchanged after same-col move");

    /* move non-existent card */
    ret = board_move_card(&b, 999, COL_DONE);
    ASSERT_EQ(ret, -1, "move non-existent card returns -1");

    board_free(&b);
}

static void test_move_across_all_columns(void)
{
    Board b = board_new();
    int id = board_add_card(&b, COL_TODO, "journey");

    /* forward */
    board_move_card(&b, id, COL_DOING);
    ASSERT_EQ(b.columns[COL_DOING].count, 1, "moved To Do -> Doing");
    board_move_card(&b, id, COL_DONE);
    ASSERT_EQ(b.columns[COL_DONE].count, 1, "moved Doing -> Done");
    ASSERT_STREQ(b.columns[COL_DONE].cards[0].title, "journey", "journey card in Done");

    /* backward */
    board_move_card(&b, id, COL_DOING);
    ASSERT_EQ(b.columns[COL_DONE].count, 0, "Done empty after move back to Doing");
    ASSERT_EQ(b.columns[COL_DOING].count, 1, "Doing has journey card");

    board_free(&b);
}

static void test_save_load_roundtrip(void)
{
    const char *path = "/tmp/test_kanban_roundtrip.json";
    cleanup(path);

    /* build board — use board_load to establish db connection first */
    Board b1;
    ASSERT_EQ(board_load(&b1, path), 0, "board_load empty path succeeds");

    board_add_card(&b1, COL_TODO, "alpha");
    board_add_card(&b1, COL_TODO, "beta");
    board_add_card(&b1, COL_DOING, "gamma");
    board_add_card(&b1, COL_DONE, "delta");

    ASSERT_EQ(board_save(&b1, path), 0, "board_save succeeds");

    int b1_next_id = b1.next_id;
    board_free(&b1);

    Board b2;
    ASSERT_EQ(board_load(&b2, path), 0, "board_load succeeds");

    ASSERT_EQ(b2.next_id, b1_next_id, "next_id preserved after load");
    ASSERT_EQ(b2.columns[COL_TODO].count, 2, "2 cards in To Do after load");
    ASSERT_EQ(b2.columns[COL_DOING].count, 1, "1 card in Doing after load");
    ASSERT_EQ(b2.columns[COL_DONE].count, 1, "1 card in Done after load");

    /* verify a card title */
    ASSERT_STREQ(b2.columns[COL_TODO].cards[0].title, "alpha", "card 0 title preserved");
    ASSERT_STREQ(b2.columns[COL_TODO].cards[1].title, "beta", "card 1 title preserved");

    board_free(&b2);
    cleanup(path);
}

static void test_load_missing_file(void)
{
    const char *path = "/tmp/nonexistent_kanban_test_file.json";
    cleanup(path);

    Board b;
    int ret = board_load(&b, path);
    ASSERT_EQ(ret, 0, "load missing file returns 0");
    ASSERT_EQ(b.next_id, 1, "missing file gives default next_id=1");
    ASSERT_EQ(b.columns[COL_TODO].count, 0, "missing file gives empty To Do");
    board_free(&b);
    cleanup(path);
}

static void test_save_empty_board(void)
{
    const char *path = "/tmp/test_kanban_empty.json";
    cleanup(path);

    /* use board_load to establish db connection */
    Board b;
    ASSERT_EQ(board_load(&b, path), 0, "board_load empty path succeeds");
    ASSERT_EQ(board_save(&b, path), 0, "save empty board succeeds");

    int next_id_1 = b.next_id;
    board_free(&b);

    Board b2;
    ASSERT_EQ(board_load(&b2, path), 0, "load empty board succeeds");
    ASSERT_EQ(b2.next_id, next_id_1, "empty load has same next_id");
    ASSERT_EQ(b2.columns[COL_TODO].count, 0, "empty load has no cards");

    board_free(&b2);
    cleanup(path);
}

static void test_get_card(void)
{
    Board b = board_new();
    int id = board_add_card(&b, COL_DOING, "find me");

    Card *c = board_get_card(&b, id);
    ASSERT_NOTNULL(c, "board_get_card finds existing card");
    ASSERT_STREQ(c->title, "find me", "title matches via get_card");
    ASSERT_EQ(c->id, id, "id matches via get_card");

    /* non-existent */
    Card *c2 = board_get_card(&b, 999);
    ASSERT_NULL(c2, "board_get_card returns NULL for missing id");

    board_free(&b);
}

static void test_edit_card_title(void)
{
    Board b = board_new();
    int id = board_add_card(&b, COL_TODO, "original");

    int ret = board_edit_card_title(&b, id, "updated");
    ASSERT_EQ(ret, 0, "edit_card_title returns 0 on success");

    Card *c = board_get_card(&b, id);
    ASSERT_NOTNULL(c, "card still exists after edit");
    ASSERT_STREQ(c->title, "updated", "title updated");

    /* edit non-existent card */
    ret = board_edit_card_title(&b, 999, "nope");
    ASSERT_EQ(ret, -1, "edit non-existent card returns -1");

    /* edit with NULL title */
    ret = board_edit_card_title(&b, id, NULL);
    ASSERT_EQ(ret, -1, "edit with NULL title returns -1");

    board_free(&b);
}

static void test_cards_distributed_across_columns(void)
{
    Board b = board_new();
    board_add_card(&b, COL_TODO, "t1");
    board_add_card(&b, COL_TODO, "t2");
    board_add_card(&b, COL_DOING, "d1");
    board_add_card(&b, COL_DONE, "x1");
    board_add_card(&b, COL_DONE, "x2");

    ASSERT_EQ(b.columns[COL_TODO].count, 2, "To Do: 2");
    ASSERT_EQ(b.columns[COL_DOING].count, 1, "Doing: 1");
    ASSERT_EQ(b.columns[COL_DONE].count, 2, "Done: 2");

    board_free(&b);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== test_board ===\n\n");

    test_add_cards();
    test_delete_card();
    test_move_card();
    test_move_across_all_columns();
    test_save_load_roundtrip();
    test_load_missing_file();
    test_save_empty_board();
    test_get_card();
    test_edit_card_title();
    test_cards_distributed_across_columns();

    printf("\n---\n%d tests: %d passed, %d failed\n",
           tests_run, tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
