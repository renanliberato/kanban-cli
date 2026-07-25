#define _POSIX_C_SOURCE 200809L
#include "llm.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* constants                                                          */
/* ------------------------------------------------------------------ */

#define MAX_JOBS           3
#define OUTPUT_CHUNK       4096
#define RESULT_INIT_CAP    256

/* ------------------------------------------------------------------ */
/* internal per-job metadata (not in the public LlmJob struct)        */
/* ------------------------------------------------------------------ */

typedef struct {
    /* Accumulator for stdout — grows as we read from the pipe.
       On completion this is copied into LlmJob.result. */
    char   *output_buf;
    size_t  output_len;
    size_t  output_cap;

    /* For the fake provider: remaining poll ticks before completion.
       -1 means not a fake job. */
    int     fake_ticks;

    /* Marked true by llm_cancel; checked in llm_poll. */
    int     cancel_requested;
} JobPriv;

/* ------------------------------------------------------------------ */
/* module state                                                       */
/* ------------------------------------------------------------------ */

typedef enum { PROVIDER_OPENCODE, PROVIDER_FAKE } ProviderType;

static ProviderType  g_provider      = PROVIDER_OPENCODE;
static int           g_fake_delay    = 1;      /* poll ticks before fake completion */
static llm_spawn_fn  g_spawn_fn      = NULL;   /* set by llm_set_provider; default built-in */
static int           g_initialized   = 0;

static LlmJob  g_jobs[MAX_JOBS];
static JobPriv g_priv[MAX_JOBS];
static int     g_next_id = 1;

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

/* Return a free slot index in g_jobs, or -1 if full. */
static int find_free_slot(void)
{
    for (int i = 0; i < MAX_JOBS; i++) {
        /* Slots with id == 0 are free (never used or cleaned up). */
        if (g_jobs[i].id == 0)
            return i;
    }
    return -1;
}

/* Free the internal resources of a job at the given slot.
   Does NOT zero the public struct — caller should do that. */
static void free_job_slot(int slot)
{
    JobPriv *p = &g_priv[slot];
    free(p->output_buf);
    p->output_buf  = NULL;
    p->output_len  = 0;
    p->output_cap  = 0;
    p->fake_ticks  = -1;
    p->cancel_requested = 0;

    LlmJob *j = &g_jobs[slot];
    free(j->prompt);
    free(j->result);
    j->prompt = NULL;
    j->result = NULL;

    if (j->pipe_fd >= 0) {
        close(j->pipe_fd);
        j->pipe_fd = -1;
    }

    /* Reap zombie if still running */
    if (j->child_pid > 0) {
        /* Best-effort: kill and reap */
        kill(j->child_pid, SIGTERM);
        int status;
        waitpid(j->child_pid, &status, WNOHANG);
        j->child_pid = 0;
    }

    /* Mark slot as free */
    memset(j, 0, sizeof(LlmJob));
}

/* Grow the output buffer for a job slot. Returns 0 on success. */
static int grow_output(int slot)
{
    JobPriv *p = &g_priv[slot];
    size_t newcap = p->output_cap ? p->output_cap * 2 : RESULT_INIT_CAP;
    char *tmp = realloc(p->output_buf, newcap);
    if (!tmp) return -1;
    p->output_buf = tmp;
    p->output_cap = newcap;
    return 0;
}

/* Append a chunk of data to the output buffer. */
static int append_output(int slot, const char *data, size_t len)
{
    JobPriv *p = &g_priv[slot];
    while (p->output_len + len + 1 > p->output_cap) {
        if (grow_output(slot) != 0) return -1;
    }
    memcpy(p->output_buf + p->output_len, data, len);
    p->output_len += len;
    p->output_buf[p->output_len] = '\0';
    return 0;
}

/* Finalise the result string from accumulated output. */
static void finalise_result(int slot)
{
    JobPriv *p = &g_priv[slot];
    LlmJob *j = &g_jobs[slot];

    if (p->output_buf && p->output_len > 0) {
        /* Ensure null termination */
        if (p->output_len >= p->output_cap)
            grow_output(slot);
        p->output_buf[p->output_len] = '\0';
        j->result = xstrdup(p->output_buf);
    } else {
        j->result = xstrdup("");
    }
}

/* ------------------------------------------------------------------ */
/* default opencode provider (builds argv; fork+exec in llm_submit)   */
/* ------------------------------------------------------------------ */

static void opencode_run_provider(LlmJob *job)
{
    (void)job;
    /* This function is here as documentation / default.
       The actual fork+exec happens inside llm_submit.  The provider
       hook can replace this with a different binary/args. */
}

/* Actually build the argv array for opencode.
   Returns a NULL-terminated array that the caller must free
   (both the array and each string). */
static char **build_opencode_argv(const LlmJob *job)
{
    /* argc: opencode + run + --format + json + --dir + dir + prompt + NULL = 8 */
    char **argv = calloc(8, sizeof(char *));
    if (!argv) return NULL;

    argv[0] = xstrdup("opencode");
    argv[1] = xstrdup("run");
    argv[2] = xstrdup("--format");
    argv[3] = xstrdup("json");
    argv[4] = xstrdup("--dir");
    argv[5] = xstrdup(".");          /* g_project_dir — default to cwd */
    argv[6] = xstrdup(job->prompt);
    argv[7] = NULL;

    /* Check for allocation failures */
    for (int i = 0; i < 7; i++) {
        if (!argv[i]) {
            for (int j = 0; j < 7 && argv[j]; j++) free(argv[j]);
            free(argv);
            return NULL;
        }
    }

    return argv;
}

static void free_argv(char **argv)
{
    if (!argv) return;
    for (int i = 0; argv[i]; i++)
        free(argv[i]);
    free(argv);
}

/* ------------------------------------------------------------------ */
/* fork + exec + pipe implementation for opencode provider            */
/* ------------------------------------------------------------------ */

static int spawn_opencode(int slot)
{
    LlmJob *j = &g_jobs[slot];
    JobPriv *p = &g_priv[slot];

    char **argv = build_opencode_argv(j);
    if (!argv) return -1;

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free_argv(argv);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        free_argv(argv);
        return -1;
    }

    if (pid == 0) {
        /* ---- child ---- */
        close(pipefd[0]);                    /* close read end */
        dup2(pipefd[1], STDOUT_FILENO);      /* stdout -> pipe write */
        dup2(pipefd[1], STDERR_FILENO);      /* stderr -> pipe write */
        close(pipefd[1]);

        execvp("opencode", argv);
        /* If execvp returns, it failed. Print error to stderr
           (which is now the pipe) and _exit. */
        fprintf(stderr, "llm: execvp opencode failed: %s\n", strerror(errno));
        free_argv(argv);
        _exit(1);
    }

    /* ---- parent ---- */
    close(pipefd[1]);                        /* close write end */
    free_argv(argv);

    /* Set read end non-blocking */
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    j->child_pid = pid;
    j->pipe_fd   = pipefd[0];
    j->started_at = time(NULL);
    j->state     = LLM_RUNNING;

    p->output_len = 0;
    p->output_cap = RESULT_INIT_CAP;
    p->output_buf = malloc(RESULT_INIT_CAP);
    if (!p->output_buf) {
        close(pipefd[0]);
        j->pipe_fd = -1;
        j->state = LLM_FAILED;
        j->result = xstrdup("allocation failed");
        return -1;
    }
    p->output_buf[0] = '\0';

    return 0;
}

/* ------------------------------------------------------------------ */
/* poll helpers for opencode provider                                 */
/* ------------------------------------------------------------------ */

static int poll_opencode_job(int slot)
{
    LlmJob *j = &g_jobs[slot];
    JobPriv *p = &g_priv[slot];

    if (p->cancel_requested) {
        if (j->child_pid > 0) {
            kill(j->child_pid, SIGTERM);
        }
        if (j->pipe_fd >= 0) {
            close(j->pipe_fd);
            j->pipe_fd = -1;
        }
        /* reap the child */
        if (j->child_pid > 0) {
            int status;
            waitpid(j->child_pid, &status, WNOHANG);
            j->child_pid = 0;
        }
        finalise_result(slot);
        j->state = LLM_CANCELLED;
        return 1;  /* transitioned */
    }

    /* Check timeout */
    if (j->timeout_secs > 0) {
        time_t now = time(NULL);
        if (now - j->started_at >= j->timeout_secs) {
            if (j->child_pid > 0)
                kill(j->child_pid, SIGTERM);
            if (j->pipe_fd >= 0) {
                close(j->pipe_fd);
                j->pipe_fd = -1;
            }
            if (j->child_pid > 0) {
                int status;
                waitpid(j->child_pid, &status, WNOHANG);
                j->child_pid = 0;
            }
            finalise_result(slot);
            j->state = LLM_FAILED;
            if (!j->result) j->result = xstrdup("timeout");
            return 1;
        }
    }

    /* Read available data from pipe */
    if (j->pipe_fd >= 0) {
        char chunk[OUTPUT_CHUNK];
        ssize_t n;
        while ((n = read(j->pipe_fd, chunk, sizeof(chunk))) > 0) {
            if (append_output(slot, chunk, (size_t)n) != 0) {
                /* allocation failure — mark failed */
                close(j->pipe_fd);
                j->pipe_fd = -1;
                if (j->child_pid > 0)
                    kill(j->child_pid, SIGTERM);
                j->state = LLM_FAILED;
                j->result = xstrdup("output buffer allocation failed");
                return 1;
            }
        }
        if (n == 0) {
            /* EOF — child closed pipe. Reap the child. */
            close(j->pipe_fd);
            j->pipe_fd = -1;
            if (j->child_pid > 0) {
                int status;
                waitpid(j->child_pid, &status, 0);
                j->child_pid = 0;
            }
            finalise_result(slot);
            /* Check exit status: non-zero exit is failure */
            j->state = LLM_DONE;  /* we treat any completion as DONE for now;
                                     the result may contain an error message */
            return 1;
        }
        /* n == -1 with EAGAIN means no data available — that's fine */
    }

    return 0;  /* no transition */
}

/* ------------------------------------------------------------------ */
/* fake provider                                                      */
/* ------------------------------------------------------------------ */

static int submit_fake(int slot)
{
    LlmJob *j = &g_jobs[slot];
    JobPriv *p = &g_priv[slot];

    j->started_at = time(NULL);
    j->state      = LLM_RUNNING;
    j->child_pid  = 0;
    j->pipe_fd    = -1;
    p->fake_ticks = g_fake_delay;
    p->output_len = 0;
    p->output_cap = RESULT_INIT_CAP;
    p->output_buf = malloc(RESULT_INIT_CAP);
    if (!p->output_buf) {
        j->state = LLM_FAILED;
        j->result = xstrdup("allocation failed");
        return -1;
    }
    p->output_buf[0] = '\0';

    /* Pre-build the result: deterministic echo */
    char echo[512];
    int written = snprintf(echo, sizeof(echo),
                           "{\"result\": \"Fake response to: %s\"}",
                           j->prompt ? j->prompt : "(null)");
    if (written > 0 && (size_t)written < sizeof(echo)) {
        append_output(slot, echo, (size_t)written);
    }

    return 0;
}

static int poll_fake_job(int slot)
{
    LlmJob *j = &g_jobs[slot];
    JobPriv *p = &g_priv[slot];

    if (p->cancel_requested) {
        finalise_result(slot);
        j->state = LLM_CANCELLED;
        return 1;
    }

    /* Check timeout */
    if (j->timeout_secs > 0) {
        time_t now = time(NULL);
        if (now - j->started_at >= j->timeout_secs) {
            free(j->result);
            j->result = xstrdup("timeout");
            j->state = LLM_FAILED;
            return 1;
        }
    }

    /* Decrement fake ticks */
    if (p->fake_ticks > 0) {
        p->fake_ticks--;
    }

    if (p->fake_ticks == 0) {
        finalise_result(slot);
        j->state = LLM_DONE;
        return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* public API                                                         */
/* ------------------------------------------------------------------ */

int llm_init(void)
{
    /* Check provider selection via env var */
    const char *prov = getenv("KANBAN_LLM_PROVIDER");
    if (prov && strcmp(prov, "fake") == 0) {
        g_provider = PROVIDER_FAKE;
    } else {
        g_provider = PROVIDER_OPENCODE;
    }

    /* Fake delay — how many poll() calls before a fake job completes */
    const char *delay_str = getenv("KANBAN_LLM_FAKE_DELAY");
    if (delay_str) {
        int d = atoi(delay_str);
        if (d >= 0) g_fake_delay = d;
    }

    /* Reset all state */
    memset(g_jobs, 0, sizeof(g_jobs));
    memset(g_priv, 0, sizeof(g_priv));
    g_next_id = 1;

    /* Default spawn function */
    g_spawn_fn = opencode_run_provider;

    g_initialized = 1;
    return 0;
}

void llm_free(void)
{
    if (!g_initialized) return;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].id != 0)
            free_job_slot(i);
    }

    memset(g_jobs, 0, sizeof(g_jobs));
    memset(g_priv, 0, sizeof(g_priv));
    g_next_id    = 1;
    g_spawn_fn   = NULL;
    g_initialized = 0;
}

int llm_submit(const char *prompt, int card_id, int timeout_secs)
{
    if (!g_initialized) return -1;
    if (!prompt) return -1;

    int slot = find_free_slot();
    if (slot < 0) return -1;   /* queue full */

    LlmJob *j = &g_jobs[slot];
    JobPriv *p = &g_priv[slot];

    memset(j, 0, sizeof(LlmJob));
    memset(p, 0, sizeof(JobPriv));
    p->fake_ticks = -1;

    j->id           = g_next_id++;
    j->state        = LLM_QUEUED;
    j->prompt       = xstrdup(prompt);
    j->card_id      = card_id;
    j->timeout_secs = timeout_secs;
    j->child_pid    = 0;
    j->pipe_fd      = -1;
    j->started_at   = 0;

    if (!j->prompt) {
        free_job_slot(slot);
        return -1;
    }

    if (g_provider == PROVIDER_FAKE) {
        if (submit_fake(slot) != 0) {
            free_job_slot(slot);
            return -1;
        }
    } else {
        if (spawn_opencode(slot) != 0) {
            free_job_slot(slot);
            return -1;
        }
    }

    return j->id;
}

int llm_poll(void)
{
    if (!g_initialized) return 0;

    int transitions = 0;

    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].id == 0) continue;
        if (g_jobs[i].state != LLM_RUNNING) continue;

        if (g_provider == PROVIDER_FAKE) {
            transitions += poll_fake_job(i);
        } else {
            transitions += poll_opencode_job(i);
        }
    }

    return transitions;
}

const LlmJob *llm_get_job(int job_id)
{
    if (!g_initialized) return NULL;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].id == job_id)
            return &g_jobs[i];
    }
    return NULL;
}

int llm_cancel(int job_id)
{
    if (!g_initialized) return -1;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].id == job_id) {
            /* If not yet done/failed/cancelled, request cancellation */
            if (g_jobs[i].state == LLM_RUNNING ||
                g_jobs[i].state == LLM_QUEUED) {
                g_priv[i].cancel_requested = 1;
                /* For QUEUED jobs, transition immediately */
                if (g_jobs[i].state == LLM_QUEUED) {
                    finalise_result(i);
                    g_jobs[i].state = LLM_CANCELLED;
                }
                return 0;
            }
            return -1;  /* already in terminal state */
        }
    }
    return -1;  /* not found */
}

int llm_job_count(void)
{
    if (!g_initialized) return 0;
    int count = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].id != 0)
            count++;
    }
    return count;
}

const LlmJob *llm_job_at(int index)
{
    if (!g_initialized) return NULL;
    int seen = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (g_jobs[i].id != 0) {
            if (seen == index)
                return &g_jobs[i];
            seen++;
        }
    }
    return NULL;
}

void llm_set_provider(llm_spawn_fn fn)
{
    g_spawn_fn = fn ? fn : opencode_run_provider;
}
