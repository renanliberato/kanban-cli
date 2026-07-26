#define _POSIX_C_SOURCE 200809L
#include "../../src/board_path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

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

#define ASSERT_STREQARR(a, b_arr, msg) do { \
    tests_run++; \
    if ((a) && strcmp((a), (b_arr)) == 0) { \
        tests_pass++; \
        printf("PASS: %s\n", msg); \
    } else { \
        tests_fail++; \
        printf("FAIL: %s (expected '%s', got '%s')\n", msg, (b_arr), (a) ? (a) : "(null)"); \
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

/* helper: mkdir -p for tests */
static void mkdirs(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof(buf), "mkdir -p %s", path);
    system(buf);
}

/* ------------------------------------------------------------------ */
/* Test: explicit path always wins                                     */
/* ------------------------------------------------------------------ */

static void test_explicit_path_wins(void)
{
    char *path = board_path_resolve("/tmp/explicit.db", "boardname");
    ASSERT_NOTNULL(path, "explicit path is non-null");
    ASSERT_STREQ(path, "/tmp/explicit.db", "explicit path returned as-is");
    free(path);
}

/* ------------------------------------------------------------------ */
/* Test: -b name resolves to ~/.kanban/<name>.db                       */
/* ------------------------------------------------------------------ */

static void test_board_name_resolution(void)
{
    /* Set HOME to a known temp path */
    char tmp_home[256];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/kanban_test_home_%d", getpid());
    mkdirs(tmp_home);
    setenv("HOME", tmp_home, 1);

    /* Change cwd to somewhere away from the test home dir so discovery fails */
    chdir("/tmp");

    char *path = board_path_resolve(NULL, "myproject");
    ASSERT_NOTNULL(path, "board name resolution returns non-null");

    char expected[1024];
    snprintf(expected, sizeof(expected), "%s/.kanban/myproject.db", tmp_home);
    ASSERT_STREQARR(path, expected, "board name resolves to ~/.kanban/<name>.db");
    free(path);

    /* Clean up */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_home);
    system(cmd);
}

/* ------------------------------------------------------------------ */
/* Test: default board resolves to ~/.kanban/default.db                */
/* ------------------------------------------------------------------ */

static void test_default_resolution(void)
{
    char tmp_home[256];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/kanban_test_default_%d", getpid());
    mkdirs(tmp_home);
    setenv("HOME", tmp_home, 1);
    chdir("/tmp");

    char *path = board_path_resolve(NULL, NULL);
    ASSERT_NOTNULL(path, "default resolution returns non-null");

    char expected[1024];
    snprintf(expected, sizeof(expected), "%s/.kanban/default.db", tmp_home);
    ASSERT_STREQARR(path, expected, "default resolves to ~/.kanban/default.db");
    free(path);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_home);
    system(cmd);
}

/* ------------------------------------------------------------------ */
/* Test: discovery — .kanban/ in cwd                                   */
/* ------------------------------------------------------------------ */

static void test_discovery_finds_local_kanban(void)
{
    char tmp_home[256];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/kanban_test_disc_home_%d", getpid());
    mkdirs(tmp_home);
    setenv("HOME", tmp_home, 1);

    /* Create project dir with .kanban/ */
    char proj_dir[512];
    snprintf(proj_dir, sizeof(proj_dir), "/tmp/kanban_test_proj_%d", getpid());
    mkdirs(proj_dir);

    char kanban_dir[1024];
    snprintf(kanban_dir, sizeof(kanban_dir), "%s/.kanban", proj_dir);
    mkdirs(kanban_dir);

    /* chdir into the project dir */
    chdir(proj_dir);

    /* Without -b, should discover .kanban/ and use default.db */
    char *path = board_path_resolve(NULL, NULL);
    ASSERT_NOTNULL(path, "discovery resolution returns non-null");

    char expected[1024];
    snprintf(expected, sizeof(expected), "%s/.kanban/default.db", proj_dir);
    ASSERT_STREQARR(path, expected, "discovery resolves to ./.kanban/default.db");
    free(path);

    /* With -b name, should use discovery dir + board name */
    char *path2 = board_path_resolve(NULL, "feature-x");
    ASSERT_NOTNULL(path2, "discovery + board name returns non-null");

    char expected2[1024];
    snprintf(expected2, sizeof(expected2), "%s/.kanban/feature-x.db", proj_dir);
    ASSERT_STREQARR(path2, expected2, "discovery + board name uses local .kanban/");
    free(path2);

    /* Explicit path still wins */
    char *path3 = board_path_resolve("/tmp/override.db", "feature-x");
    ASSERT_STREQ(path3, "/tmp/override.db", "explicit path wins over discovery + board name");
    free(path3);

    /* Clean up */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s", tmp_home, proj_dir);
    system(cmd);
}

/* ------------------------------------------------------------------ */
/* Test: display name                                                  */
/* ------------------------------------------------------------------ */

static void test_display_name(void)
{
    /* Standard ~/.kanban/default.db — extracts "default" */
    char *name = board_path_display_name("/home/user/.kanban/default.db");
    ASSERT_NOTNULL(name, "display name non-null");
    ASSERT_STREQ(name, "default", "display name extracts 'default'");
    free(name);

    /* Local project board */
    char *name2 = board_path_display_name("/tmp/project/.kanban/backend.db");
    ASSERT_NOTNULL(name2, "local display name non-null");
    ASSERT_STREQ(name2, "backend", "display name extracts 'backend'");
    free(name2);

    /* Arbitrary path */
    char *name3 = board_path_display_name("/some/random/path.db");
    ASSERT_NOTNULL(name3, "arbitrary display name non-null");
    ASSERT_STREQ(name3, "path", "arbitrary path extracts filename");
    free(name3);
}

/* ------------------------------------------------------------------ */
/* Test: discovery stops at $HOME                                      */
/* ------------------------------------------------------------------ */

static void test_discovery_stops_at_home(void)
{
    char tmp_home[256];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/kanban_test_stop_home_%d", getpid());
    mkdirs(tmp_home);

    /* Create .kanban/ in HOME so that walking up from a subdir finds it */
    char home_kanban[1024];
    snprintf(home_kanban, sizeof(home_kanban), "%s/.kanban", tmp_home);
    mkdirs(home_kanban);

    setenv("HOME", tmp_home, 1);

    /* Create a subdir and cd into it */
    char subdir[1024];
    snprintf(subdir, sizeof(subdir), "%s/work/project", tmp_home);
    mkdirs(subdir);
    chdir(subdir);

    /* Discovery should walk up to find the .kanban/ at HOME */
    char *path = board_path_resolve(NULL, "work-board");
    ASSERT_NOTNULL(path, "discovery stops at home returns non-null");

    char expected[1024];
    snprintf(expected, sizeof(expected), "%s/.kanban/work-board.db", tmp_home);
    ASSERT_STREQARR(path, expected, "discovery walks up to home's .kanban/");
    free(path);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_home);
    system(cmd);
}

/* ------------------------------------------------------------------ */
/* Test: list known boards                                             */
/* ------------------------------------------------------------------ */

static void test_list_boards(void)
{
    char tmp_home[256];
    snprintf(tmp_home, sizeof(tmp_home), "/tmp/kanban_test_list_%d", getpid());
    mkdirs(tmp_home);

    char home_kanban[1024];
    snprintf(home_kanban, sizeof(home_kanban), "%s/.kanban", tmp_home);
    mkdirs(home_kanban);

    setenv("HOME", tmp_home, 1);

    /* Create a few .db files (empty sqlite dbs — just touch)
       Actually we need valid sqlite files for count_cards_in_db to work.
       Let's create minimal valid db files using the kanban binary itself. */
    char cmd[2048];

    /* We'll just touch the files — they won't be valid sqlite but
       count_cards_in_db will return -1, which is fine for testing.
       The list function still includes them. */
    snprintf(cmd, sizeof(cmd), "touch %s/.kanban/project-a.db %s/.kanban/project-b.db",
             tmp_home, tmp_home);
    system(cmd);

    /* Change to /tmp so discovery doesn't pick up anything */
    chdir("/tmp");

    int count = 0;
    BoardInfo *boards = board_path_list(&count);
    ASSERT_TRUE(count >= 2, "list boards returns at least 2 entries");

    /* Check that our boards appear */
    int found_a = 0, found_b = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(boards[i].display_name, "project-a") == 0) found_a++;
        if (strcmp(boards[i].display_name, "project-b") == 0) found_b++;
        free(boards[i].path);
        free(boards[i].display_name);
    }
    ASSERT_TRUE(found_a > 0, "list includes project-a");
    ASSERT_TRUE(found_b > 0, "list includes project-b");
    free(boards);

    char rm_cmd[1024];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", tmp_home);
    system(rm_cmd);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("=== Board Path Unit Tests ===\n\n");

    test_explicit_path_wins();
    test_board_name_resolution();
    test_default_resolution();
    test_discovery_finds_local_kanban();
    test_display_name();
    test_discovery_stops_at_home();
    test_list_boards();

    printf("\n---\n");
    printf("Tests run:  %d\n", tests_run);
    printf("Tests pass: %d\n", tests_pass);
    printf("Tests fail: %d\n", tests_fail);

    return tests_fail > 0 ? 1 : 0;
}
