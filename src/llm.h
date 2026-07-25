#ifndef LLM_H
#define LLM_H

#include <sys/types.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* types                                                              */
/* ------------------------------------------------------------------ */

typedef enum {
    LLM_QUEUED,
    LLM_RUNNING,
    LLM_DONE,
    LLM_FAILED,
    LLM_CANCELLED
} LlmJobState;

typedef struct {
    int          id;            /* unique job id */
    LlmJobState  state;
    char        *prompt;        /* what was sent */
    char        *result;        /* raw stdout (or error message) */
    int          card_id;       /* associated card, -1 if none */
    pid_t        child_pid;     /* 0 when not running (or fake) */
    int          pipe_fd;       /* read end of stdout pipe, -1 when not running */
    time_t       started_at;
    int          timeout_secs;
} LlmJob;

/* ------------------------------------------------------------------ */
/* lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* Initialise the LLM subsystem. Reads KANBAN_LLM_PROVIDER env var
   ("opencode" or "fake"; default "opencode") and configures the
   provider accordingly. Returns 0 on success, -1 on error. */
int  llm_init(void);

/* Free all resources held by the LLM subsystem (cancel running jobs,
   free buffers). Safe to call even if llm_init was never called. */
void llm_free(void);

/* ------------------------------------------------------------------ */
/* job management                                                     */
/* ------------------------------------------------------------------ */

/* Submit a new job.  The provider is invoked asynchronously according
   to the active provider type.  The prompt is copied internally.
   card_id is associated with the job (-1 for none).
   timeout_secs is the wall-clock deadline (0 = no timeout).
   Returns the job id on success, -1 if the queue is full or on error.
   Max concurrent jobs: 3 (compile-time constant). */
int  llm_submit(const char *prompt, int card_id, int timeout_secs);

/* Poll running jobs — non-blocking.
   Reads from pipes, reaps children, enforces timeouts.
   Returns the number of jobs that transitioned state during this call
   (i.e., newly DONE, FAILED, or CANCELLED). Call from the main loop. */
int  llm_poll(void);

/* ------------------------------------------------------------------ */
/* access / control                                                   */
/* ------------------------------------------------------------------ */

/* Return a pointer to the job with the given id, or NULL. */
const LlmJob *llm_get_job(int job_id);

/* Cancel a job: sends SIGTERM to the child process (if any) and
   marks the job CANCELLED.  For fake jobs, marks CANCELLED immediately.
   Returns 0 on success, -1 if the job id is unknown. */
int  llm_cancel(int job_id);

/* Return the current number of jobs (any state). */
int  llm_job_count(void);

/* Return the job at the given index (0..job_count-1), or NULL.
   Order is insertion order; the array is not compacted eagerly. */
const LlmJob *llm_job_at(int index);

/* ------------------------------------------------------------------ */
/* provider hook (internal, swapped for testing)                      */
/* ------------------------------------------------------------------ */

/* A spawn function is called by the llm subsystem to execute the
   provider for a given job.  The default provider builds an argv for
   `opencode run`.  A fake provider can be installed for testing. */
typedef void (*llm_spawn_fn)(LlmJob *job);

/* Replace the current provider spawn function.
   This only affects the "opencode" provider path — the fake provider
   (selected via env var) bypasses the spawn function entirely. */
void llm_set_provider(llm_spawn_fn fn);

#endif
