// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Persistent History + Obsidian Integration

#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscalls.h>   /* jash_tmpdir() */
#include <time.h>
#include <unistd.h>

#define MAX_HISTORY 1000
#define MAX_CMD_LEN 4096
#define HISTORY_FILE ".jash_history"
#define OBSIDIAN_DIR ".jash_obsidian"

typedef struct {
    char* commands[MAX_HISTORY];
    int count;
    int current;
    char* path;
} history_t;

static history_t g_history = {0};
static int g_loaded = 0;       /* set once history has been initialized */
static int g_file_loaded = 0;  /* set once entries were read from disk */
static int g_persisted = 0;    /* entries already written to the history file */
static int g_trim_pending = 0; /* in-memory trim dropped entries: file needs
                                * a full rewrite, not a pure append */

// Get home directory
static const char* get_home(void) {
    const char* h = getenv("HOME");
    return (h && *h) ? h : jash_tmpdir();  /* BUG-J1: respect TMPDIR */
}

// Load history entries from the history file (best-effort)
static void history_load_file(void) {
    FILE* f = fopen(g_history.path, "r");
    if (f) {
        char line[MAX_CMD_LEN];
        while (fgets(line, sizeof(line), f) && g_history.count < MAX_HISTORY) {
            // Remove newline
            size_t len = strlen(line);
            if (len > 0 && line[len-1] == '\n') {
                line[len-1] = '\0';
            }
            g_history.commands[g_history.count++] = strdup(line);
        }
        fclose(f);
    }
    /* If fopen failed: no history loaded, start fresh. This is normal
     * on first boot or when tmpfs is full. F E R M I ∞ H A R T */

    /* Everything now in memory came from the file — already persisted. */
    g_persisted = g_history.count;
}

// Initialize history (idempotent: a second call is a no-op).
// Only the path is set up here; the file itself is read lazily on the
// first history read/add/clear, so a `-c` run that never touches history
// pays no file IO at all.
void history_init(void) {
    if (g_loaded) return;
    g_loaded = 1;

    g_history.count = 0;
    g_history.current = 0;
    g_persisted = 0;

    // Create history file path
    const char* home = get_home();
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", home, HISTORY_FILE);
    g_history.path = strdup(path);

    // Create .jash directory (best-effort; Beyond tmpfs may be full)
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/.jash", home);
    mkdir(dir, 0755);
}

// Read the on-disk entries at most once per process, on first use.
// (graceful if unavailable — Beyond tmpfs may be read-only or full;
// we simply start with empty history)
static void history_ensure_loaded(void) {
    if (g_file_loaded) return;
    if (!g_loaded) history_init();
    if (!g_history.path) return;
    g_file_loaded = 1;
    history_load_file();
    g_history.current = g_history.count;
}

// Save history to file.
// Incremental: appends only entries not yet persisted. Falls back to a
// full rewrite only when an in-memory trim dropped old entries — a pure
// append would then grow the file past MAX_HISTORY.
void history_save(void) {
    if (!g_history.path) return;

    if (g_trim_pending) {
        FILE* f = fopen(g_history.path, "w");
        if (!f) return;  /* Beyond: tmpfs full or read-only — silently skip */

        int i = 0;
        for (; i < g_history.count; i++) {
            if (fprintf(f, "%s\n", g_history.commands[i]) < 0) {
                /* Write failed (Beyond: tmpfs capacity exceeded). Stop writing
                 * but don't crash — partial history is better than none. */
                break;
            }
        }
        fclose(f);
        g_trim_pending = 0;
        g_persisted = i;
        return;
    }

    if (g_persisted >= g_history.count) return;  /* nothing new to write */

    FILE* f = fopen(g_history.path, "a");
    if (!f) return;  /* Beyond: tmpfs full or read-only — silently skip */

    int i = g_persisted;
    for (; i < g_history.count; i++) {
        if (fprintf(f, "%s\n", g_history.commands[i]) < 0) {
            /* Write failed (Beyond: tmpfs capacity exceeded). Stop writing
             * but don't crash — partial history is better than none. */
            break;
        }
    }
    fclose(f);
    g_persisted = i;
}

// Add command to history
void history_add(const char* cmd) {
    history_ensure_loaded();
    if (!cmd || !*cmd) return;
    
    // Don't add duplicates
    if (g_history.count > 0 && 
        strcmp(g_history.commands[g_history.count-1], cmd) == 0) {
        return;
    }
    
    // Remove oldest if full
    if (g_history.count >= MAX_HISTORY) {
        free(g_history.commands[0]);
        memmove(g_history.commands, g_history.commands + 1,
                (MAX_HISTORY - 1) * sizeof(char*));
        g_history.count--;
        /* Entries shifted: the on-disk file no longer matches memory.
         * Next save must be a full rewrite, not an append. */
        g_trim_pending = 1;
    }
    
    g_history.commands[g_history.count++] = strdup(cmd);
    g_history.current = g_history.count;
    
    // Auto-save every 100 commands
    if (g_history.count % 100 == 0) {
        history_save();
    }
}

// Navigate history (up/down arrows)
const char* history_prev(void) {
    history_ensure_loaded();
    if (g_history.current > 0) {
        g_history.current--;
        return g_history.commands[g_history.current];
    }
    return NULL;
}

const char* history_next(void) {
    history_ensure_loaded();
    if (g_history.current < g_history.count) {
        g_history.current++;
        if (g_history.current < g_history.count) {
            return g_history.commands[g_history.current];
        }
    }
    return "";
}

// Search history (Ctrl+R style)
const char* history_search(const char* prefix) {
    history_ensure_loaded();
    if (!prefix || !*prefix) return NULL;
    
    size_t len = strlen(prefix);
    
    // Search backwards from current position
    for (int i = g_history.count - 1; i >= 0; i--) {
        if (strncmp(g_history.commands[i], prefix, len) == 0) {
            return g_history.commands[i];
        }
    }
    return NULL;
}

// Fuzzy search (for autosuggestion)
const char* history_fuzzy_search(const char* partial) {
    history_ensure_loaded();
    if (!partial || !*partial) return NULL;
    
    size_t plen = strlen(partial);
    
    // Try exact prefix match first
    for (int i = g_history.count - 1; i >= 0; i--) {
        if (strncmp(g_history.commands[i], partial, plen) == 0) {
            return g_history.commands[i];
        }
    }
    
    // Try substring match
    for (int i = g_history.count - 1; i >= 0; i--) {
        if (strstr(g_history.commands[i], partial)) {
            return g_history.commands[i];
        }
    }
    
    return NULL;
}

// Get history count
int history_count(void) {
    history_ensure_loaded();
    return g_history.count;
}

// Get command at index
const char* history_get(int index) {
    history_ensure_loaded();
    if (index < 0 || index >= g_history.count) {
        return NULL;
    }
    return g_history.commands[index];
}

// Obsidian integration
void obsidian_log_command(const char* cmd) {
    const char* home = get_home();
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s/%s", home, OBSIDIAN_DIR);
    mkdir(dir, 0755);  /* best-effort; Beyond tmpfs may be full */

    // Get current date
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char date[32];
    strftime(date, sizeof(date), "%Y-%m-%d", tm);

    char path[sizeof dir + sizeof date + sizeof "/.md"];
    snprintf(path, sizeof(path), "%s/%s.md", dir, date);

    FILE* f = fopen(path, "a");
    if (!f) return;  /* Beyond: filesystem unavailable — skip silently */
    {
        char time_str[32];
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm);
        fprintf(f, "- `%s` (at %s)\n", cmd, time_str);
    }
    fclose(f);
}

// Analyze user patterns and create summary
void obsidian_analyze_patterns(void) {
    const char* home = get_home();
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s/patterns.md", home, OBSIDIAN_DIR);
    
    FILE* f = fopen(path, "w");
    if (!f) return;
    
    fprintf(f, "# Jash User Patterns\n\n");
    fprintf(f, "Generated: %s\n\n", __DATE__);
    
    // Count command frequency
    struct {
        const char* cmd;
        int count;
    } cmd_counts[100];
    int ncmds = 0;
    
    for (int i = 0; i < g_history.count; i++) {
        const char* cmd = g_history.commands[i];
        char first_word[256];
        sscanf(cmd, "%255s", first_word);
        
        // Find or add
        int found = 0;
        for (int j = 0; j < ncmds; j++) {
            if (strcmp(cmd_counts[j].cmd, first_word) == 0) {
                cmd_counts[j].count++;
                found = 1;
                break;
            }
        }
        if (!found && ncmds < 100) {
            cmd_counts[ncmds].cmd = strdup(first_word);
            cmd_counts[ncmds].count = 1;
            ncmds++;
        }
    }
    
    // Sort by count
    for (int i = 0; i < ncmds - 1; i++) {
        for (int j = i + 1; j < ncmds; j++) {
            if (cmd_counts[j].count > cmd_counts[i].count) {
                const char* tmp_cmd = cmd_counts[i].cmd;
                int tmp_count = cmd_counts[i].count;
                cmd_counts[i].cmd = cmd_counts[j].cmd;
                cmd_counts[i].count = cmd_counts[j].count;
                cmd_counts[j].cmd = tmp_cmd;
                cmd_counts[j].count = tmp_count;
            }
        }
    }
    
    fprintf(f, "## Most Used Commands\n\n");
    for (int i = 0; i < ncmds && i < 20; i++) {
        fprintf(f, "- `%s` (%d times)\n", cmd_counts[i].cmd, cmd_counts[i].count);
    }
    
    fprintf(f, "\n## Total Commands\n\n");
    fprintf(f, "**%d** commands executed\n", g_history.count);
    
    fclose(f);
    
    // Free
    for (int i = 0; i < ncmds; i++) {
        free((void*)cmd_counts[i].cmd);
    }
}

// Cleanup
void history_cleanup(void) {
    history_save();

    for (int i = 0; i < g_history.count; i++) {
        free(g_history.commands[i]);
    }
    free(g_history.path);
    g_history.path = NULL;
    g_history.count = 0;
    g_history.current = 0;
    g_loaded = 0;
    g_file_loaded = 0;
    g_persisted = 0;
    g_trim_pending = 0;
}

// Wrapper functions for compatibility with existing code
#include "../builtins/builtin.h"

void jash_history_clear(jash_state_t* s) {
    (void)s;
    history_ensure_loaded();
    for (int i = 0; i < g_history.count; i++) {
        free(g_history.commands[i]);
        g_history.commands[i] = NULL;
    }
    g_history.count = 0;
    g_history.current = 0;
    g_trim_pending = 1;  /* force full rewrite: clear must truncate the file */
    history_save();
}

uint32_t jash_history_count(jash_state_t* s) {
    (void)s;
    history_ensure_loaded();
    return (uint32_t)g_history.count;
}

const char* jash_history_at(jash_state_t* s, uint32_t index) {
    (void)s;
    history_ensure_loaded();
    if (index >= (uint32_t)g_history.count) {
        return NULL;
    }
    return g_history.commands[index];
}

void jash_history_load(jash_state_t* s, const char* path) {
    (void)s;
    (void)path;  /* history file is derived from HOME, like history_init() */
    /* Real load, idempotent: on the interactive path this runs from the
     * boot thread and history_init() was never called — without this, the
     * on-disk history never entered the session and the exit-time save
     * overwrote ~/.jash_history with only the current session's commands.
     * When history_init() already ran (-c / serve paths), the g_loaded
     * guard makes this a no-op, so no entries are duplicated. */
    history_init();
    history_ensure_loaded();
}

void jash_history_add(jash_state_t* s, const char* cmd) {
    (void)s;
    history_add(cmd);

    /* Auto-analyze patterns every 100 commands */
    static int cmd_counter = 0;
    if (++cmd_counter >= 100) {
        cmd_counter = 0;
        obsidian_analyze_patterns();
    }
}

void jash_history_push(jash_state_t* s, const char* cmd) {
    (void)s;
    history_add(cmd);
}

void jash_history_save(jash_state_t* s, const char* path) {
    (void)s;
    (void)path;
    history_save();
}

int jash_history_expand(jash_state_t* s, const char* line, char* buf, size_t buflen) {
    (void)s;
    (void)line;
    (void)buf;
    (void)buflen;
    // Stub: history expansion not implemented yet
    return 0;
}
