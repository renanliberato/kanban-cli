#include "../../src/undo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* portable xdup (c99 compat) */
static char *xdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, s, len + 1);
    return copy;
}

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

#define ASSERT_STR_EQ(a, b, msg) do { \
    tests_run++; \
    const char *_a = (a); const char *_b = (b); \
    if ((_a && _b && strcmp(_a, _b) == 0) || (_a == NULL && _b == NULL)) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected '%s', got '%s')\n", msg, \
               _b ? _b : "(null)", _a ? _a : "(null)"); \
    } \
} while(0)

static void test_empty_undo(void)
{
    ASSERT_EQ(undo_is_empty(), 1, "ring starts empty");
    UndoOp op;
    ASSERT_EQ(undo_pop(&op), 0, "pop on empty returns 0");
}

static void test_push_pop_one(void)
{
    UndoOp op;
    memset(&op, 0, sizeof(op));
    op.type = UNDO_DELETE;
    op.card_id = 42;
    op.orig_col = 1;
    op.orig_pos = 3;
    op.snap_title = xdup("hello");
    op.snap_description = xdup("world");

    undo_push(op);
    ASSERT_EQ(undo_is_empty(), 0, "ring not empty after push");

    UndoOp out;
    ASSERT_EQ(undo_pop(&out), 1, "pop returns 1");
    ASSERT_EQ(out.type, UNDO_DELETE, "type preserved");
    ASSERT_EQ(out.card_id, 42, "card_id preserved");
    ASSERT_EQ(out.orig_col, 1, "orig_col preserved");
    ASSERT_EQ(out.orig_pos, 3, "orig_pos preserved");
    ASSERT_STR_EQ(out.snap_title, "hello", "title preserved");
    ASSERT_STR_EQ(out.snap_description, "world", "description preserved");

    free(out.snap_title);
    free(out.snap_description);
    ASSERT_EQ(undo_is_empty(), 1, "ring empty after pop");
}

static void test_ring_wraparound(void)
{
    /* push 25 items (ring size is 20) */
    for (int i = 0; i < 25; i++) {
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type = UNDO_MOVE;
        op.card_id = i;
        op.snap_title = xdup("x");
        undo_push(op);
    }

    /* oldest 5 should have been evicted */
    UndoOp out;
    int count = 0;
    while (undo_pop(&out)) {
        free(out.snap_title);
        count++;
    }
    ASSERT_EQ(count, 20, "ring caps at 20 (oldest evicted)");
}

static void test_multiple_ops_fifo(void)
{
    for (int i = 0; i < 3; i++) {
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type = UNDO_EDIT_TITLE;
        op.card_id = 100 + i;
        op.snap_title = xdup("t");
        undo_push(op);
    }

    /* pops should be LIFO — most recent first */
    UndoOp out;
    ASSERT_EQ(undo_pop(&out), 1, "pop 1");
    ASSERT_EQ(out.card_id, 102, "most recent is card 102");
    free(out.snap_title);

    ASSERT_EQ(undo_pop(&out), 1, "pop 2");
    ASSERT_EQ(out.card_id, 101, "second is card 101");
    free(out.snap_title);

    ASSERT_EQ(undo_pop(&out), 1, "pop 3");
    ASSERT_EQ(out.card_id, 100, "oldest is card 100");
    free(out.snap_title);

    ASSERT_EQ(undo_is_empty(), 1, "empty after all pops");
}

static void test_clear(void)
{
    for (int i = 0; i < 5; i++) {
        UndoOp op;
        memset(&op, 0, sizeof(op));
        op.type = UNDO_ARCHIVE;
        op.card_id = i;
        op.snap_title = xdup("t");
        undo_push(op);
    }
    ASSERT_EQ(undo_is_empty(), 0, "not empty before clear");
    undo_clear();
    ASSERT_EQ(undo_is_empty(), 1, "empty after clear");
}

static void test_edit_desc_snapshot(void)
{
    UndoOp op;
    memset(&op, 0, sizeof(op));
    op.type = UNDO_EDIT_DESC;
    op.card_id = 7;
    op.snap_title = NULL;
    op.snap_description = xdup("long description text");

    undo_push(op);

    UndoOp out;
    ASSERT_EQ(undo_pop(&out), 1, "pop desc edit");
    ASSERT_EQ(out.type, UNDO_EDIT_DESC, "type is EDIT_DESC");
    ASSERT_STR_EQ(out.snap_title, NULL, "title is NULL for desc edit");
    ASSERT_STR_EQ(out.snap_description, "long description text", "desc preserved");
    free(out.snap_description);
}

static void test_archive_undo(void)
{
    UndoOp op;
    memset(&op, 0, sizeof(op));
    op.type = UNDO_ARCHIVE;
    op.card_id = 99;
    op.snap_archived = 0;

    undo_push(op);

    UndoOp out;
    ASSERT_EQ(undo_pop(&out), 1, "pop archive");
    ASSERT_EQ(out.type, UNDO_ARCHIVE, "type is ARCHIVE");
    ASSERT_EQ(out.card_id, 99, "card_id preserved");
}

int main(void)
{
    printf("=== test_undo ===\n\n");

    test_empty_undo();
    test_push_pop_one();
    test_ring_wraparound();
    test_multiple_ops_fifo();
    test_clear();
    test_edit_desc_snapshot();
    test_archive_undo();

    printf("\n--- results: %d run, %d passed, %d failed ---\n",
           tests_run, tests_pass, tests_fail);
    return tests_fail ? 1 : 0;
}
