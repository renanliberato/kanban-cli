#ifndef ENRICH_H
#define ENRICH_H

/*
 * AI Enrich module for kanban cards.
 *
 * Provides prompt building, result parsing, and envelope unwrapping
 * for the LLM-based enrichment flow.  The prompt asks the LLM to
 * expand a card title into a description, labels, and clarifying
 * questions.
 */

/* ------------------------------------------------------------------ */
/* EnrichResult — parsed output from the LLM                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char  *description;       /* proposed description, may be NULL */
    char **labels;            /* NULL-terminated array of label strings */
    int    label_count;
    char **questions;         /* NULL-terminated array of "q" strings */
    char **answers;           /* NULL-terminated array of "a" strings */
    int    question_count;
} EnrichResult;

/* ------------------------------------------------------------------ */
/* prompt builder                                                     */
/* ------------------------------------------------------------------ */

/*
 * Build an enrichment prompt for the given card title and optional
 * existing description.  Returns a malloc'd string the caller must
 * free, or NULL on allocation failure.
 *
 * The prompt instructs the LLM to return STRICT JSON:
 *   {"description": "...", "labels": ["...", ...],
 *    "questions": [{"q": "...", "a": "..."}, ...]}
 */
char *enrich_build_prompt(const char *title, const char *existing_desc);

/* ------------------------------------------------------------------ */
/* result parsing                                                     */
/* ------------------------------------------------------------------ */

/*
 * Unwrap the LLM provider envelope from a raw result string.
 * Returns a malloc'd string containing just the inner JSON payload,
 * or NULL if the envelope cannot be parsed.
 *
 * TODO SPIKE: The opencode run --format json envelope shape is not
 * yet confirmed.  This function currently expects that the output
 * may be wrapped in a JSON object with an "output" key.  If the
 * output is already a plain JSON object without an envelope, it is
 * returned as-is.  Update this function once the real opencode
 * output format is verified.
 */
char *enrich_unwrap_envelope(const char *raw_output);

/*
 * Parse an enrichment JSON result into an EnrichResult struct.
 * Returns 0 on success, -1 on parse failure.
 * On success, the caller must call enrich_free_result() to release
 * allocated memory.
 *
 * Expected JSON shape (after envelope unwrapping):
 *   {"description": "...", "labels": ["...", ...],
 *    "questions": [{"q": "...", "a": "..."}, ...]}
 *
 * All fields are optional — missing fields result in NULL/empty.
 * Malformed JSON or wrong types are treated as parse failure.
 */
int  enrich_parse_result(const char *json_str, EnrichResult *out);

/* Free all memory held by an EnrichResult.  Safe to call on
   zero-initialized structs (set by enrich_parse_result). */
void enrich_free_result(EnrichResult *r);

#endif
