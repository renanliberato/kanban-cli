#include "enrich.h"
#include "../vendor/cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ------------------------------------------------------------------ */
/* prompt builder                                                     */
/* ------------------------------------------------------------------ */

char *enrich_build_prompt(const char *title, const char *existing_desc)
{
    if (!title) return NULL;

    /*
     * Build a prompt asking the LLM to expand the card title.
     * The prompt must be clear about the STRICT JSON output format
     * so the result parser can rely on a predictable shape.
     */
    const char *desc_part = (existing_desc && existing_desc[0])
        ? "\nCurrent description: " : "";

    /* Estimate buffer size */
    size_t title_len   = strlen(title);
    size_t desc_len    = existing_desc ? strlen(existing_desc) : 0;
    size_t extra       = 1500;  /* prompt template + JSON example */
    size_t total       = title_len + desc_len + extra + 1;

    char *prompt = malloc(total);
    if (!prompt) return NULL;

    int n = snprintf(prompt, total,
        "You are a kanban board assistant. Given a card title, "
        "expand it into a detailed description, relevant labels, "
        "and clarifying questions with proposed answers.\n\n"
        "Card title: %s\n%s%s\n\n"
        "Return ONLY a valid JSON object (no markdown, no code fences) "
        "with this exact structure:\n"
        "{\n"
        "  \"description\": \"A detailed description of the task\",\n"
        "  \"labels\": [\"label1\", \"label2\"],\n"
        "  \"questions\": [\n"
        "    {\"q\": \"Clarifying question?\", \"a\": \"Proposed answer\"}\n"
        "  ]\n"
        "}\n\n"
        "All fields are required but may be empty (empty string for "
        "description, empty arrays for labels/questions).",
        title,
        desc_part,
        existing_desc ? existing_desc : "");

    if (n < 0 || (size_t)n >= total) {
        free(prompt);
        return NULL;
    }

    return prompt;
}

/* ------------------------------------------------------------------ */
/* envelope unwrapping                                                */
/* ------------------------------------------------------------------ */

/*
 * TODO SPIKE: The opencode run --format json envelope shape is not
 * yet confirmed.  This function assumes the output may be wrapped
 * in a JSON object like {"output": "<inner json>"} or may already
 * be the plain enrichment JSON.  Update once the real opencode
 * output format is verified.
 */
char *enrich_unwrap_envelope(const char *raw_output)
{
    if (!raw_output || !raw_output[0]) return NULL;

    /* Try parsing as JSON */
    cJSON *root = cJSON_Parse(raw_output);
    if (!root) {
        /* Not valid JSON at all — return the raw text as-is (for error
           reporting in the caller) */
        return xstrdup(raw_output);
    }

    /* Check if it has an "output" key (possible envelope) */
    cJSON *output = cJSON_GetObjectItem(root, "output");
    if (cJSON_IsString(output) && output->valuestring) {
        char *inner = xstrdup(output->valuestring);
        cJSON_Delete(root);
        return inner;
    }

    /* Check if it has a "result" key (another possible envelope) */
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsString(result) && result->valuestring) {
        char *inner = xstrdup(result->valuestring);
        cJSON_Delete(root);
        return inner;
    }

    /* No envelope detected — the raw_output itself is the JSON we want.
       Return a copy. */
    cJSON_Delete(root);
    return xstrdup(raw_output);
}

/* ------------------------------------------------------------------ */
/* result parsing                                                     */
/* ------------------------------------------------------------------ */

int enrich_parse_result(const char *json_str, EnrichResult *out)
{
    if (!json_str || !out) return -1;

    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json_str);
    if (!root) return -1;

    /* description (string, optional) */
    cJSON *desc = cJSON_GetObjectItem(root, "description");
    if (cJSON_IsString(desc) && desc->valuestring && desc->valuestring[0]) {
        out->description = xstrdup(desc->valuestring);
    }

    /* labels (array of strings, optional) */
    cJSON *labels = cJSON_GetObjectItem(root, "labels");
    if (cJSON_IsArray(labels)) {
        int nlabels = cJSON_GetArraySize(labels);
        if (nlabels > 0) {
            out->labels = malloc((size_t)(nlabels + 1) * sizeof(char *));
            if (out->labels) {
                for (int i = 0; i < nlabels; i++) {
                    cJSON *l = cJSON_GetArrayItem(labels, i);
                    if (cJSON_IsString(l) && l->valuestring)
                        out->labels[out->label_count++] = xstrdup(l->valuestring);
                }
                out->labels[out->label_count] = NULL; /* NULL-terminate */
            }
        }
    }

    /* questions (array of {q, a} objects, optional) */
    cJSON *questions = cJSON_GetObjectItem(root, "questions");
    if (cJSON_IsArray(questions)) {
        int nq = cJSON_GetArraySize(questions);
        if (nq > 0) {
            out->questions = malloc((size_t)(nq + 1) * sizeof(char *));
            out->answers   = malloc((size_t)(nq + 1) * sizeof(char *));
            if (out->questions && out->answers) {
                for (int i = 0; i < nq; i++) {
                    cJSON *qa = cJSON_GetArrayItem(questions, i);
                    if (!cJSON_IsObject(qa)) continue;
                    cJSON *q = cJSON_GetObjectItem(qa, "q");
                    cJSON *a = cJSON_GetObjectItem(qa, "a");
                    if (cJSON_IsString(q) && q->valuestring)
                        out->questions[out->question_count] = xstrdup(q->valuestring);
                    else
                        out->questions[out->question_count] = xstrdup("");
                    if (cJSON_IsString(a) && a->valuestring)
                        out->answers[out->question_count] = xstrdup(a->valuestring);
                    else
                        out->answers[out->question_count] = xstrdup("");
                    out->question_count++;
                }
                out->questions[out->question_count] = NULL; /* NULL-terminate */
                out->answers[out->question_count]   = NULL;
            }
        }
    }

    cJSON_Delete(root);
    return 0;
}

void enrich_free_result(EnrichResult *r)
{
    if (!r) return;
    free(r->description);
    r->description = NULL;

    if (r->labels) {
        for (int i = 0; r->labels[i]; i++)
            free(r->labels[i]);
        free(r->labels);
        r->labels = NULL;
    }
    r->label_count = 0;

    if (r->questions) {
        for (int i = 0; r->questions[i]; i++)
            free(r->questions[i]);
        free(r->questions);
        r->questions = NULL;
    }
    if (r->answers) {
        for (int i = 0; r->answers[i]; i++)
            free(r->answers[i]);
        free(r->answers);
        r->answers = NULL;
    }
    r->question_count = 0;
}
