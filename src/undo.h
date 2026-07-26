#ifndef UNDO_H
#define UNDO_H

/*
 * ring buffer of 20 inverse operations — memory-only, never persisted.
 *
 * after every destructive op (delete, move, title/description edit,
 * archive), push a snapshot of the affected card.  pressing 'u' pops
 * the most recent snapshot and restores the card to that state.
 */

#define UNDO_RING_SIZE 20

typedef enum {
    UNDO_DELETE,
    UNDO_MOVE,
    UNDO_EDIT_TITLE,
    UNDO_EDIT_DESC,
    UNDO_ARCHIVE
} UndoOpType;

typedef struct {
    UndoOpType type;
    int        card_id;
    int        orig_col;      /* column card was in before the op */
    int        orig_pos;      /* position within orig_col           */
    char      *snap_title;
    char      *snap_description;  /* may be NULL */
    int        snap_archived;
    /* we do NOT snapshot labels — that would be expensive and
       labels are not manipulated by the covered destructive ops */
} UndoOp;

/* push a snapshot onto the ring buffer (overwrites oldest if full) */
void undo_push(UndoOp op);

/* pop the most recent snapshot.  returns 1 on success, 0 if empty.
   caller owns the strings in *out and must free them. */
int  undo_pop(UndoOp *out);

/* discard all undos */
void undo_clear(void);

/* are there any entries? */
int  undo_is_empty(void);

/* peek at the most recent entry without popping (for flash hints) */
int  undo_peek(UndoOp *out);

#endif
