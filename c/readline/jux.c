// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash interactive UX (jux) helpers — history-ranked autosuggest/fuzzy/abbr config.

#include "jux.h"
#include <core/history.h>
#include <core/env.h>
#include <core/predict.h>
#include <liquid/jash_liquid.h>
#include <parser/eval.h>
#include <theme/io.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdio.h>

static int starts_with(const char* s, const char* p) {
    while (*p) { if (*s++ != *p++) return 0; }
    return 1;
}

static int streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static void line_prefix(const jash_line_t* L, char* out, unsigned cap) {
    unsigned n = L->cur < cap - 1 ? L->cur : cap - 1;
    for (unsigned i = 0; i < n; i++) out[i] = L->buf[i];
    out[n] = 0;
}

static int is_io_path_cmd_name(const char* cmd) {
    return streq(cmd, "ls") || streq(cmd, "cat") || streq(cmd, "less") ||
           streq(cmd, "more") || streq(cmd, "head") || streq(cmd, "tail") ||
           streq(cmd, "wc") || streq(cmd, "grep") || streq(cmd, "tar");
}

static int suppress_path_autosuggest(const char* prefix) {
    char cmd[32];
    unsigned i = 0;
    while (*prefix == ' ' || *prefix == '\t') prefix++;
    while (prefix[i] && prefix[i] != ' ' && prefix[i] != '\t' && i + 1 < sizeof(cmd)) {
        cmd[i] = prefix[i];
        i++;
    }
    cmd[i] = 0;
    if (!is_io_path_cmd_name(cmd)) return 0;
    return prefix[i] == ' ' || prefix[i] == '\t';
}

static const char* abbr_lookup(const jash_state_t* s, const char* name) {
    for (uint32_t i = 0; i < s->abbr_local_count; i++) {
        if (strcmp(s->abbr_local[i].key, name) == 0)
            return s->abbr_local[i].val;
    }
    for (uint32_t i = 0; i < s->abbr_global_count; i++) {
        if (strcmp(s->abbr_global[i].key, name) == 0)
            return s->abbr_global[i].val;
    }
    return 0;
}

/* Silent writer for rc-line evaluation: rc files should not pollute the
 * startup prompt. side-effects (env var sets, alias registration) still
 * happen via jash_eval_line; only stdout/stderr output is dropped. */
static long rc_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx; (void)buf; return (long)n;
}

static void load_jashrc_into(jash_state_t* s, const char* path,
                              int is_local) {
    long fd = sys_open(path, 0, 0);
    if (fd < 0) return;
    char buf[8192];
    long n = sys_read((int)fd, buf, sizeof(buf) - 1);
    sys_close((int)fd);
    if (n <= 0) return;
    buf[n] = 0;

    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;

        if (strncmp(line, "abbr ", 5) == 0) {
            char* body = line + 5;
            while (*body == ' ' || *body == '\t') body++;

            int line_is_local = 0;
            int line_is_global = 0;
            if (strncmp(body, "--local ", 8) == 0) { line_is_local = 1; body += 8; }
            else if (strncmp(body, "--global ", 9) == 0) { line_is_global = 1; body += 9; }

            while (*body == ' ' || *body == '\t') body++;

            char* name = body;
            while (*body && *body != ' ' && *body != '\t') body++;
            if (*body) *body++ = 0;
            while (*body == ' ' || *body == '\t') body++;

            if (!*name || !*body) continue;

            int store_local = is_local || line_is_local;
            if (!store_local && !line_is_global && !is_local) store_local = 0;

            if (store_local) {
                if (s->abbr_local_count < JASH_ABBR_LOCAL_MAX) {
                    strncpy(s->abbr_local[s->abbr_local_count].key, name,
                            JASH_ABBR_KEY_LEN - 1);
                    s->abbr_local[s->abbr_local_count].key[JASH_ABBR_KEY_LEN - 1] = '\0';
                    strncpy(s->abbr_local[s->abbr_local_count].val, body,
                            JASH_ABBR_VAL_LEN - 1);
                    s->abbr_local[s->abbr_local_count].val[JASH_ABBR_VAL_LEN - 1] = '\0';
                    s->abbr_local_count++;
                }
            } else {
                if (s->abbr_global_count < JASH_ABBR_GLOBAL_MAX) {
                    strncpy(s->abbr_global[s->abbr_global_count].key, name,
                            JASH_ABBR_KEY_LEN - 1);
                    s->abbr_global[s->abbr_global_count].key[JASH_ABBR_KEY_LEN - 1] = '\0';
                    strncpy(s->abbr_global[s->abbr_global_count].val, body,
                            JASH_ABBR_VAL_LEN - 1);
                    s->abbr_global[s->abbr_global_count].val[JASH_ABBR_VAL_LEN - 1] = '\0';
                    s->abbr_global_count++;
                }
            }
        } else if (strncmp(line, "set ", 4) == 0) {
            char* key = line + 4;
            while (*key == ' ' || *key == '\t') key++;
            char* val = key;
            while (*val && *val != ' ' && *val != '\t') val++;
            if (*val) *val++ = 0;
            while (*val == ' ' || *val == '\t') val++;
            if (streq(key, "jash_autosuggest")) jash_env_set(s, "JASH_AUTOSUGGEST", val);
            if (streq(key, "fish_color_autosuggestion")) jash_env_set(s, "JASH_AUTOSUGGEST_COLOR", val);
        } else {
            /* Any other line is a shell command: export, alias, source, var=val,
             * etc. Without this, ~/.jashrc with `export PATH=...` was silently
             * dropped and the user wondered why their tools weren't on PATH.
             * Use a no-op writer — rc output (e.g. accidental echo in .jashrc)
             * is intentionally suppressed at startup to keep the prompt clean.
             * F E R M I ∞ H A R T */
            jash_eval_line(s, rc_writer, 0, line);
        }
    }
}

void jash_jux_load_config(jash_state_t* s) {
    if (!s || jash_env_get(s, "JASH_JUX_LOADED")) return;
    jash_env_set(s, "JASH_JUX_LOADED", "1");
    if (!jash_env_get(s, "JASH_AUTOSUGGEST")) jash_env_set(s, "JASH_AUTOSUGGEST", "1");
    if (!jash_env_get(s, "JASH_AUTOSUGGEST_COLOR")) jash_env_set(s, "JASH_AUTOSUGGEST_COLOR", "\033[90m");
#ifdef JASH_TINALINUX
    /* PID 1 owns migration of startup policy; no executable rc on the guest. */
    return;
#endif

    const char* home = jash_env_get(s, "HOME");
    if (home) {
        char path[512];
        snprintf(path, sizeof(path), "%s/.jashrc", home);
        load_jashrc_into(s, path, 0);
    }

    load_jashrc_into(s, "./.jashrc", 1);
}

int jash_jux_autosuggest(jash_state_t* s, const jash_line_t* L,
                             char* out, unsigned cap) {
    if (!s || !L || !out || cap == 0) return 0;
    out[0] = 0;
    const char* enabled = jash_env_get(s, "JASH_AUTOSUGGEST");
    if (enabled && (streq(enabled, "0") || streq(enabled, "off"))) return 0;
    if (L->cur != L->len || L->len == 0) return 0;

    char prefix[JASH_LINE_MAX];
    line_prefix(L, prefix, sizeof(prefix));
    if (suppress_path_autosuggest(prefix)) return 0;
    uint32_t hc = jash_history_count(s);
    const char* best = 0;
    for (uint32_t i = hc; i > 0; i--) {
        const char* h = jash_history_at(s, i - 1);
        if (h && starts_with(h, prefix) && !streq(h, prefix)) {
            best = h;
            break;
        }
    }
    if (best) {
        /* Check predictions for a potentially better match */
        char pred_ctx[PREDICT_CONTEXT_LEN];
        predict_make_context(pred_ctx, sizeof(pred_ctx), s->cwd);
        const char* predicted = predict_best(&s->predictions, pred_ctx);
        if (predicted && starts_with(predicted, prefix) && !streq(predicted, prefix)) {
            const char* psuffix = predicted + strlen(prefix);
            strncpy(out, psuffix, cap - 1);
            out[cap - 1] = 0;
            return out[0] != 0;
        }

        const char* suffix = best + strlen(prefix);
        strncpy(out, suffix, cap - 1);
        out[cap - 1] = 0;
        return out[0] != 0;
    }

    /* ADR-0033 Wave 2: fall back to Liquid Reasoning Engine suggestion. */
    char brain_sug[JASH_LINE_MAX];
    if (jash_liquid_autosuggest(prefix, s->cwd, brain_sug, sizeof(brain_sug))) {
        strncpy(out, brain_sug, cap - 1);
        out[cap - 1] = 0;
        return out[0] != 0;
    }

    return 0;
}

int jash_jux_accept_suggestion(jash_line_t* L, const char* suggestion) {
    if (!L || !suggestion || !*suggestion || L->cur != L->len) return 0;
    int changed = 0;
    while (*suggestion && L->len + 1 < JASH_LINE_MAX) {
        L->buf[L->len++] = *suggestion++;
        L->cur = L->len;
        changed = 1;
    }
    L->buf[L->len] = 0;
    return changed;
}

int jash_jux_expand_abbr(jash_state_t* s, jash_line_t* L) {
    if (!s || !L || L->cur != L->len || L->len == 0) return 0;

    uint32_t end = L->len;
    while (end > 0 && (L->buf[end - 1] == ' ' || L->buf[end - 1] == '\t')) end--;
    uint32_t start = end;
    while (start > 0 && L->buf[start - 1] != ' ' && L->buf[start - 1] != '\t') start--;

    if (start == 0) {
        char name[JASH_ABBR_KEY_LEN];
        uint32_t len = end - start;
        if (len > 0 && len < sizeof(name)) {
            for (uint32_t i = 0; i < len; i++) name[i] = L->buf[start + i];
            name[len] = 0;

            const char* val = abbr_lookup(s, name);
            if (val && *val) {
                uint32_t tail_start = end;
                while (tail_start < L->len &&
                       (L->buf[tail_start] == ' ' || L->buf[tail_start] == '\t'))
                    tail_start++;
                uint32_t tail_len = L->len - tail_start;

                L->len = 0;
                L->cur = 0;
                uint32_t vi = 0;
                while (val[vi] && L->len + 1 < JASH_LINE_MAX)
                    L->buf[L->len++] = val[vi++];

                if (tail_len > 0 && L->len + 1 < JASH_LINE_MAX) {
                    L->buf[L->len++] = ' ';
                    for (uint32_t i = 0; i < tail_len && L->len + 1 < JASH_LINE_MAX; i++)
                        L->buf[L->len++] = L->buf[tail_start + i];
                }

                L->cur = L->len;
                L->buf[L->len] = 0;
                return 1;
            }
        }
    }

    if (start > 0 && end > start) {
        char first_ch = L->buf[start];
        if (first_ch == '/') {
            char word[JASH_ABBR_KEY_LEN];
            uint32_t wlen = end - start;
            if (wlen > 0 && wlen < sizeof(word)) {
                for (uint32_t i = 0; i < wlen; i++) word[i] = L->buf[start + i];
                word[wlen] = 0;

                for (uint32_t i = 0; i < s->abbr_local_count; i++) {
                    uint32_t kn = strlen(s->abbr_local[i].key);
                    if (kn > 0 && strncmp(word, s->abbr_local[i].key, kn) == 0 &&
                        word[kn] == '\0') {
                        const char* val = s->abbr_local[i].val;
                        uint32_t tail_start = end;
                        while (tail_start < L->len &&
                               (L->buf[tail_start] == ' ' || L->buf[tail_start] == '\t'))
                            tail_start++;
                        uint32_t tail_len = L->len - tail_start;

                        uint32_t new_len = start;
                        uint32_t vi = 0;
                        while (val[vi] && new_len + 1 < JASH_LINE_MAX)
                            L->buf[new_len++] = val[vi++];

                        if (tail_len > 0 && new_len + 1 < JASH_LINE_MAX) {
                            L->buf[new_len++] = ' ';
                            for (uint32_t j = 0; j < tail_len && new_len + 1 < JASH_LINE_MAX; j++)
                                L->buf[new_len++] = L->buf[tail_start + j];
                        }

                        L->len = new_len;
                        L->cur = L->len;
                        L->buf[L->len] = 0;
                        return 1;
                    }
                }
                for (uint32_t i = 0; i < s->abbr_global_count; i++) {
                    uint32_t kn = strlen(s->abbr_global[i].key);
                    if (kn > 0 && strncmp(word, s->abbr_global[i].key, kn) == 0 &&
                        word[kn] == '\0') {
                        const char* val = s->abbr_global[i].val;
                        uint32_t tail_start = end;
                        while (tail_start < L->len &&
                               (L->buf[tail_start] == ' ' || L->buf[tail_start] == '\t'))
                            tail_start++;
                        uint32_t tail_len = L->len - tail_start;

                        uint32_t new_len = start;
                        uint32_t vi = 0;
                        while (val[vi] && new_len + 1 < JASH_LINE_MAX)
                            L->buf[new_len++] = val[vi++];

                        if (tail_len > 0 && new_len + 1 < JASH_LINE_MAX) {
                            L->buf[new_len++] = ' ';
                            for (uint32_t j = 0; j < tail_len && new_len + 1 < JASH_LINE_MAX; j++)
                                L->buf[new_len++] = L->buf[tail_start + j];
                        }

                        L->len = new_len;
                        L->cur = L->len;
                        L->buf[L->len] = 0;
                        return 1;
                    }
                }
            }
        }
    }

    return 0;
}

static int fuzzy_score(const char* text, const char* q) {
    if (!q || !*q) return 0;
    int score = 0, streak = 0;
    const char* t = text;
    while (*q) {
        int found = 0;
        while (*t) {
            char a = *t, b = *q;
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a == b) { found = 1; t++; break; }
            streak = 0; t++;
        }
        if (!found) return -1;
        streak++;
        score += 10 + streak * 3;
        q++;
    }
    return score;
}

const char* jash_jux_fuzzy_history(jash_state_t* s, const char* query,
                                      int previous_index) {
    if (!s || !query || !*query) return NULL;
    uint32_t hc = jash_history_count(s);
    int best_score = -1, best_idx = -1;
    for (uint32_t i = 0; i < hc; i++) {
        if ((int)i == previous_index) continue;
        const char* h = jash_history_at(s, i);
        int fs = fuzzy_score(h, query);
        if (fs < 0) continue;
        int score = fs + (int)i; /* recency boost */
        if (score > best_score) { best_score = score; best_idx = (int)i; }
    }
    return best_idx >= 0 ? jash_history_at(s, (uint32_t)best_idx) : NULL;
}
