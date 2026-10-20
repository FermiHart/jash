// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Module: readline/balance — detect incomplete input constructs.
//
// Returns 0 when the accumulated line needs more input (PS2 prompt),
// 1 when it's complete and ready to evaluate.

int jash_line_is_complete(const char *line, int len);

/*
 * Count occurrences of a keyword in a line, ignoring quoted/escaped regions.
 * Uses simple byte scanning — no tokenizer needed for the keyword counts
 * we need (if/fi, for/done, while/done, case/esac).
 */
static int kw_count(const char *p, int len, const char *kw, int kwlen) {
    int c = 0;
    char inq = 0;
    for (int i = 0; i <= len - kwlen; i++) {
        if (inq) { if (inq == p[i]) inq = 0; continue; }
        if (p[i] == '"' || p[i] == '\'') { inq = p[i]; continue; }
        if (p[i] == '\\') { i++; continue; }
        /* Skip alphanumeric chars that don't start with kw */
        int match = 1;
        for (int j = 0; j < kwlen; j++)
            if (p[i+j] != kw[j]) { match = 0; break; }
        if (!match) continue;
        /* Keyword must be followed by non-alphanumeric (or EOL) to avoid
         * matching "ifconfig" when looking for "if". */
        char nxt = (i + kwlen < len) ? p[i + kwlen] : ' ';
        if ((nxt >= 'a' && nxt <= 'z') || (nxt >= 'A' && nxt <= 'Z') ||
            (nxt >= '0' && nxt <= '9') || nxt == '_')
            continue;
        c++;
        i += kwlen - 1;
    }
    return c;
}

static int has_unclosed_quotes(const char *line, int len) {
    char q = 0;
    for (int i = 0; i < len; i++) {
        if (line[i] == '\\') { i++; continue; }
        if (q) { if (line[i] == q) q = 0; }
        else if (line[i] == '\'' || line[i] == '"') q = line[i];
    }
    return q;
}

static int ends_with_backslash(const char *line, int len) {
    /* Count trailing backslashes (even number = escaped backslash, odd = continuation) */
    int i = len - 1;
    while (i >= 0 && line[i] == '\\') i--;
    int n = len - 1 - i;
    return n % 2 == 1; /* odd trailing backslashes = continuation */
}

int jash_line_is_complete(const char *line, int len) {
    if (!line || len <= 0) return 1;

    /* Backslash continuation */
    if (ends_with_backslash(line, len)) return 0;

    /* Unclosed quotes */
    if (has_unclosed_quotes(line, len)) return 0;

    /* Count keyword pairs. Incomplete if opens > closes. */
    int if_kw   = kw_count(line, len, "if",   2);
    int fi_kw   = kw_count(line, len, "fi",   2);
    int for_kw  = kw_count(line, len, "for",  3);
    int done_kw = kw_count(line, len, "done", 4);
    int while_kw= kw_count(line, len, "while",5);
    int until_kw= kw_count(line, len, "until",5);
    int case_kw = kw_count(line, len, "case", 4);
    int esac_kw = kw_count(line, len, "esac", 4);

    /* 'for', 'while', and 'until' all close with 'done' */
    int opens  = if_kw + for_kw + while_kw + until_kw + case_kw;
    int closes = fi_kw + done_kw + esac_kw;

    if (opens > closes) return 0;

    /* Dollar-parenthesis $( ... ) balancing */
    int dp_open = 0, dp_close = 0;
    for (int i = 0; i < len - 1; i++) {
        if (line[i] == '$' && line[i+1] == '(') dp_open++;
        if (line[i] == ')' && (i == 0 || line[i-1] != '$')) dp_close++;
    }
    if (dp_open > dp_close) return 0;

    /* [[ ]] balancing */
    int db_open = 0, db_close = 0;
    char in_str = 0;
    for (int i = 0; i < len - 1; i++) {
        if (line[i] == '\\') { i++; continue; }
        if (in_str) { if (line[i] == in_str) in_str = 0; continue; }
        if (line[i] == '"' || line[i] == '\'') { in_str = line[i]; continue; }
        if (line[i] == '[' && line[i+1] == '[') { db_open++; i++; }
        if (line[i] == ']' && line[i+1] == ']') { db_close++; i++; }
    }
    if (db_open > db_close) return 0;

    return 1;
}
