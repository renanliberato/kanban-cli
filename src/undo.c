#include "undo.h"
#include <stdlib.h>
#include <string.h>

/* ring buffer */
static UndoOp  g_undo_ring[UNDO_RING_SIZE];
static int     g_undo_head = 0;  /* next write index */
static int     g_undo_count = 0; /* number of live entries */

void undo_push(UndoOp op)
{
    /* if ring is full, free the oldest entry first */
    if (g_undo_count == UNDO_RING_SIZE) {
        int oldest = (g_undo_head - g_undo_count + UNDO_RING_SIZE) % UNDO_RING_SIZE;
        free(g_undo_ring[oldest].snap_title);
        free(g_undo_ring[oldest].snap_description);
        g_undo_count--;
    }

    g_undo_ring[g_undo_head] = op;
    g_undo_head = (g_undo_head + 1) % UNDO_RING_SIZE;
    g_undo_count++;
}

int undo_pop(UndoOp *out)
{
    if (g_undo_count == 0) return 0;

    int idx = (g_undo_head - 1 + UNDO_RING_SIZE) % UNDO_RING_SIZE;
    *out = g_undo_ring[idx];
    g_undo_count--;

    /* advance head backwards so push still works */
    g_undo_head = idx;

    return 1;
}

int undo_peek(UndoOp *out)
{
    if (g_undo_count == 0) return 0;

    int idx = (g_undo_head - 1 + UNDO_RING_SIZE) % UNDO_RING_SIZE;
    *out = g_undo_ring[idx];
    return 1;
}

void undo_clear(void)
{
    while (g_undo_count > 0) {
        int idx = (g_undo_head - 1 + UNDO_RING_SIZE) % UNDO_RING_SIZE;
        free(g_undo_ring[idx].snap_title);
        free(g_undo_ring[idx].snap_description);
        g_undo_count--;
        g_undo_head = idx;
    }
}

int undo_is_empty(void)
{
    return g_undo_count == 0;
}
