/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_JOURNAL_H
#define JASH_JOURNAL_H

#define JOURNAL_TAG_MAX 6
#define JOURNAL_HIST_MAX 64

typedef enum {
    JOURNAL_TAG_SESSION_START,
    JOURNAL_TAG_DOMAIN,
    JOURNAL_TAG_SWITCHED,
    JOURNAL_TAG_ERROR,
    JOURNAL_TAG_RETRY,
    JOURNAL_TAG_RESUMED,
    JOURNAL_TAG_SUDO,
} journal_tag_kind_t;

typedef struct {
    journal_tag_kind_t kind;
    char tag[JOURNAL_TAG_MAX];
    char domain[16];
    unsigned error_count;
} journal_entry_t;

typedef struct {
    journal_entry_t items[JOURNAL_HIST_MAX];
    int count;
    int cap;
} journal_tag_history_t;

void journal_init(void);
void journal_post_cmd(const char* cmd, int rc);
const char* journal_current_tag(void);
const char* journal_auto_tag(const char* cmd, int rc);
int journal_tag_history_get(journal_tag_history_t* h, int max);
void journal_reset(void);

/* Persist auto-tags to a sidecar file so journal queries work across
 * separate jash -c invocations even when the sigil module owns chain.log. */
void journal_flush(void);

#endif /* JASH_JOURNAL_H */
