// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/change — sovereign spec-driven change engine (ADR-0003)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Wave 1-2: change folder + schema engine + artifact lifecycle.
//   change schema init <name>
//   change schema fork <src> <name>
//   change propose <name> [--schema <schema>]
//   change continue [<name>]           create next ready artifact
//   change ff [<name>]                 create all remaining artifacts
//   change status                        list schemas and active changes
//
// This is the shell-native replacement for OpenSpec's /opsx:* slash commands.
// No telemetry, no npm, no LLM in the scaffolding path.

#include "builtin.h"
#include <theme/colors.h>
#include <theme/io.h>
#include <core/timeline.h>

#include <stdio.h>
#include <stdlib.h>
#ifdef JASH_BLIBC
#include <bear_spawn.h>
#endif
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>
#include <stdint.h>

#define CHG_DIR          ".changes"
#define CHG_SCHEMA_DIR   ".changes/schemas"
#define CHG_ARCHIVE_DIR  ".changes/archive"
#define CHG_MAX_NAME     128
#define CHG_MAX_PATH     1024
#define CHG_MAX_ARTIFACTS 16
#define CHG_MAX_REQUIRES  8

/* -------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* -------------------------------------------------------------------------- */

static int chg_mkdir_p(const char* path) {
    char tmp[CHG_MAX_PATH];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int chg_dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int chg_file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int chg_name_valid(const char* name) {
    if (!name || !*name) return 0;
    for (const char* p = name; *p; p++) {
        if ((*p >= 'a' && *p <= 'z') ||
            (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_') continue;
        return 0;
    }
    return 1;
}

static void chg_uuid(char* out, size_t cap) {
    static uint32_t counter = 0;
    struct timespec ts = {0};
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    counter++;
    snprintf(out, cap, "chg-%08lx-%08lx-%04x",
             (unsigned long)ts.tv_sec,
             (unsigned long)ts.tv_nsec,
             counter & 0xFFFF);
}

static int chg_write_file(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    if (fputs(content, f) == EOF) { fclose(f); return -1; }
    if (fclose(f) != 0) return -1;
    return 0;
}

static int chg_write_template(const char* path, const char* body, const char* name) {
    char buf[8192];
    /* `body` is always one of the internal CHG_TPL_* constants, each with
     * exactly one %s consumed by `name` (verified: all 8 call sites) — a
     * fixed template, not user input. Suppress locally, not globally (W6). */
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wformat-nonliteral"
    snprintf(buf, sizeof(buf), body, name);
    #pragma GCC diagnostic pop
    return chg_write_file(path, buf);
}

static void chg_rtrim(char* s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ')) s[--n] = '\0';
}

static void chg_extract_value(const char* line, char* out, size_t cap) {
    const char* p = line;
    while (*p && *p != ':') p++;
    if (*p == ':') p++;
    while (*p == ' ' || *p == '\t') p++;
    size_t len = strlen(p);
    while (len > 0 && (p[len-1] == '\n' || p[len-1] == '\r' || p[len-1] == ' ')) len--;
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Schema engine                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    char id[CHG_MAX_NAME];
    char generates[CHG_MAX_PATH];
    char requires[CHG_MAX_REQUIRES][CHG_MAX_NAME];
    int n_requires;
} chg_artifact_t;

typedef struct {
    char name[CHG_MAX_NAME];
    chg_artifact_t artifacts[CHG_MAX_ARTIFACTS];
    int n_artifacts;
} chg_schema_t;

static int chg_parse_schema(const char* path, chg_schema_t* out) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    memset(out, 0, sizeof(*out));
    char line[512];
    int in_artifacts = 0;
    int artifact_idx = -1;
    while (fgets(line, sizeof(line), f)) {
        chg_rtrim(line);
        if (strncmp(line, "name:", 5) == 0) {
            chg_extract_value(line, out->name, sizeof(out->name));
        } else if (strcmp(line, "artifacts:") == 0) {
            in_artifacts = 1;
        } else if (in_artifacts) {
            if (line[0] != ' ' && line[0] != '-') {
                in_artifacts = 0;
                continue;
            }
            if (strncmp(line, "  - id:", 7) == 0) {
                artifact_idx++;
                if (artifact_idx >= CHG_MAX_ARTIFACTS) break;
                out->n_artifacts = artifact_idx + 1;
                chg_extract_value(line, out->artifacts[artifact_idx].id, sizeof(out->artifacts[artifact_idx].id));
            } else if (artifact_idx >= 0 && strncmp(line, "    generates:", 14) == 0) {
                chg_extract_value(line, out->artifacts[artifact_idx].generates, sizeof(out->artifacts[artifact_idx].generates));
            } else if (artifact_idx >= 0 && strncmp(line, "    requires:", 13) == 0) {
                char reqbuf[256];
                chg_extract_value(line, reqbuf, sizeof(reqbuf));
                char* p = reqbuf;
                while (*p && *p != '[') p++;
                if (*p == '[') p++;
                while (*p && *p != ']') {
                    while (*p == ' ' || *p == '\t') p++;
                    char req[CHG_MAX_NAME];
                    int i = 0;
                    while (*p && *p != ',' && *p != ']' && i < CHG_MAX_NAME - 1) {
                        if (*p != ' ' && *p != '\t') req[i++] = *p;
                        p++;
                    }
                    req[i] = '\0';
                    if (i > 0 && out->artifacts[artifact_idx].n_requires < CHG_MAX_REQUIRES) {
                        snprintf(out->artifacts[artifact_idx].requires[out->artifacts[artifact_idx].n_requires++],
                                 CHG_MAX_NAME, "%s", req);
                    }
                    if (*p == ',') p++;
                }
            }
        }
    }
    fclose(f);
    return out->n_artifacts > 0 ? 0 : -1;
}

static int chg_dir_has_markdown(const char* path) {
    DIR* d = opendir(path);
    if (!d) return 0;
    int found = 0;
    struct dirent* ent;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        char child[CHG_MAX_PATH];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) found = chg_dir_has_markdown(child);
        else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len > 3 && strcmp(ent->d_name + len - 3, ".md") == 0) found = 1;
        }
    }
    closedir(d);
    return found;
}

static int chg_artifact_exists(const char* change_dir, const chg_artifact_t* a) {
    char path[CHG_MAX_PATH];
    if (strcmp(a->id, "specs") == 0) {
        /* specs artifact: any .md under specs/ */
        snprintf(path, sizeof(path), "%s/specs", change_dir);
        return chg_dir_has_markdown(path);
    }
    /* default: file named after generates */
    snprintf(path, sizeof(path), "%s/%s", change_dir, a->generates);
    return chg_file_exists(path) ? 1 : 0;
}

static int chg_artifact_ready(const chg_schema_t* schema, const chg_artifact_t* a,
                              const int* done) {
    for (int i = 0; i < a->n_requires; i++) {
        int found = 0;
        for (int j = 0; j < schema->n_artifacts; j++) {
            if (strcmp(schema->artifacts[j].id, a->requires[i]) == 0) {
                if (done[j]) found = 1;
                break;
            }
        }
        if (!found) return 0;
    }
    return 1;
}

/* -------------------------------------------------------------------------- */
/* Schema templates                                                           */
/* -------------------------------------------------------------------------- */

static const char* CHG_SCHEMA_SPEC_DRIVEN =
"name: spec-driven\n"
"version: 1\n"
"description: Standard proposal → specs → design → tasks flow.\n"
"artifacts:\n"
"  - id: proposal\n"
"    generates: proposal.md\n"
"    requires: []\n"
"  - id: specs\n"
"    generates: specs/**/spec.md\n"
"    requires: [proposal]\n"
"  - id: design\n"
"    generates: design.md\n"
"    requires: [proposal]\n"
"  - id: tasks\n"
"    generates: tasks.md\n"
"    requires: [specs, design]\n";

static const char* CHG_SCHEMA_BUGFIX =
"name: bugfix\n"
"version: 1\n"
"description: Minimal reproduce → fix → verify flow.\n"
"artifacts:\n"
"  - id: reproduce\n"
"    generates: reproduce.md\n"
"    requires: []\n"
"  - id: fix\n"
"    generates: fix.md\n"
"    requires: [reproduce]\n"
"  - id: verify\n"
"    generates: verify.md\n"
"    requires: [fix]\n";

static const char* CHG_SCHEMA_SPIKE =
"name: spike\n"
"version: 1\n"
"description: Research-first exploration with findings only.\n"
"artifacts:\n"
"  - id: findings\n"
"    generates: findings.md\n"
"    requires: []\n";

static const char* chg_builtin_schema(const char* name) {
    if (strcmp(name, "spec-driven") == 0) return CHG_SCHEMA_SPEC_DRIVEN;
    if (strcmp(name, "bugfix") == 0) return CHG_SCHEMA_BUGFIX;
    if (strcmp(name, "spike") == 0) return CHG_SCHEMA_SPIKE;
    return NULL;
}

static const char* chg_schema_path(const char* schema_name, char* out, size_t cap) {
    snprintf(out, cap, "%s/%s/schema.yaml", CHG_SCHEMA_DIR, schema_name);
    return out;
}

/* -------------------------------------------------------------------------- */
/* Artifact templates                                                         */
/* -------------------------------------------------------------------------- */

static const char* CHG_TPL_PROPOSAL =
"# Proposal: %s\n\n"
"## Intent\n\n"
"_What problem are we solving?_\n\n"
"## Scope\n\n"
"In scope:\n- \n\n"
"Out of scope:\n- \n\n"
"## Approach\n\n"
"_High-level strategy._\n";

static const char* CHG_TPL_SPEC =
"# Delta Spec: %s\n\n"
"## ADDED Requirements\n\n"
"### Requirement: \n\n"
"_Requirement text using SHALL/MUST/SHOULD._\n\n"
"#### Scenario: \n"
"- GIVEN \n"
"- WHEN \n"
"- THEN \n\n"
"## MODIFIED Requirements\n\n"
"## REMOVED Requirements\n\n";

static const char* CHG_TPL_DESIGN =
"# Design: %s\n\n"
"## Technical Approach\n\n"
"_How the change will be implemented._\n\n"
"## Architecture Decisions\n\n"
"### Decision: \n\n"
"- Option chosen: \n"
"- Rationale: \n\n"
"## File Changes\n\n"
"- `path/to/file` (new / modified)\n";

static const char* CHG_TPL_TASKS =
"# Tasks: %s\n\n"
"## 1. Setup\n"
"- [ ] 1.1 \n"
"- [ ] 1.2 \n\n"
"## 2. Implementation\n"
"- [ ] 2.1 \n"
"- [ ] 2.2 \n\n"
"## 3. Verification\n"
"- [ ] 3.1 \n";

static const char* CHG_TPL_REPRODUCE =
"# Reproduce: %s\n\n"
"## Steps\n\n"
"1. \n\n"
"## Expected bug\n\n"
"_What should happen vs what happens._\n";

static const char* CHG_TPL_FIX =
"# Fix: %s\n\n"
"## Root cause\n\n"
"_Why the bug happens._\n\n"
"## Fix description\n\n"
"_What changed._\n";

static const char* CHG_TPL_VERIFY_BUGFIX =
"# Verify: %s\n\n"
"## Verification steps\n\n"
"- [ ] Reproduce steps now pass\n"
"- [ ] Regression test added\n";

static const char* CHG_TPL_FINDINGS =
"# Findings: %s\n\n"
"## Summary\n\n"
"_What we learned._\n\n"
"## Recommendations\n\n"
"- \n";

static int chg_create_artifact(const char* change_dir, const char* name,
                               const chg_artifact_t* a, jash_writer_t w, void* ctx) {
    char path[CHG_MAX_PATH];
    if (strcmp(a->id, "specs") == 0) {
        snprintf(path, sizeof(path), "%s/specs", change_dir);
        chg_mkdir_p(path);
        snprintf(path, sizeof(path), "%s/specs/spec.md", change_dir);
        return chg_write_template(path, CHG_TPL_SPEC, name);
    }
    if (strcmp(a->id, "proposal") == 0) {
        snprintf(path, sizeof(path), "%s/proposal.md", change_dir);
        return chg_write_template(path, CHG_TPL_PROPOSAL, name);
    }
    if (strcmp(a->id, "design") == 0) {
        snprintf(path, sizeof(path), "%s/design.md", change_dir);
        return chg_write_template(path, CHG_TPL_DESIGN, name);
    }
    if (strcmp(a->id, "tasks") == 0) {
        snprintf(path, sizeof(path), "%s/tasks.md", change_dir);
        return chg_write_template(path, CHG_TPL_TASKS, name);
    }
    if (strcmp(a->id, "reproduce") == 0) {
        snprintf(path, sizeof(path), "%s/reproduce.md", change_dir);
        return chg_write_template(path, CHG_TPL_REPRODUCE, name);
    }
    if (strcmp(a->id, "fix") == 0) {
        snprintf(path, sizeof(path), "%s/fix.md", change_dir);
        return chg_write_template(path, CHG_TPL_FIX, name);
    }
    if (strcmp(a->id, "verify") == 0) {
        snprintf(path, sizeof(path), "%s/verify.md", change_dir);
        return chg_write_template(path, CHG_TPL_VERIFY_BUGFIX, name);
    }
    if (strcmp(a->id, "findings") == 0) {
        snprintf(path, sizeof(path), "%s/findings.md", change_dir);
        return chg_write_template(path, CHG_TPL_FINDINGS, name);
    }
    jash_writef(w, ctx, "%swarning:%s no template for artifact '%s', skipping\n",
                J_ROLE_WARN, J_RESET, a->id);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Spec / delta engine                                                        */
/* -------------------------------------------------------------------------- */

#define CHG_MAX_REQ_TITLE 256
#define CHG_MAX_REQ_BODY  8192
#define CHG_MAX_REQS      64

typedef struct {
    char title[CHG_MAX_REQ_TITLE];
    char body[CHG_MAX_REQ_BODY];
    int is_added;
    int is_modified;
    int is_removed;
} chg_requirement_t;

typedef struct {
    chg_requirement_t reqs[CHG_MAX_REQS];
    int n_reqs;
    char purpose[1024];
} chg_spec_t;

static int chg_spec_parse(const char* path, chg_spec_t* spec) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    memset(spec, 0, sizeof(*spec));
    char line[1024];
    int in_purpose = 0;
    while (fgets(line, sizeof(line), f)) {
        chg_rtrim(line);
        if (strncmp(line, "## Purpose", 10) == 0) {
            in_purpose = 1;
            continue;
        }
        if (in_purpose) {
            if (line[0] == '#' && strncmp(line, "### Requirement:", 16) != 0) {
                in_purpose = 0;
            } else if (strlen(line) > 0) {
                size_t cur = strlen(spec->purpose);
                size_t add = strlen(line);
                if (cur + add + 2 < sizeof(spec->purpose)) {
                    if (cur > 0) spec->purpose[cur++] = '\n';
                    memcpy(spec->purpose + cur, line, add + 1);
                }
            }
        }
        if (strncmp(line, "### Requirement:", 16) == 0) {
            /* found a requirement without section header: treat as base spec */
            if (spec->n_reqs >= CHG_MAX_REQS) { fclose(f); return -1; }
            int idx = spec->n_reqs++;
            memset(&spec->reqs[idx], 0, sizeof(spec->reqs[idx]));
            chg_extract_value(line, spec->reqs[idx].title, sizeof(spec->reqs[idx].title));
            /* read body until next ### Requirement or EOF */
            while (fgets(line, sizeof(line), f)) {
                chg_rtrim(line);
                if (strncmp(line, "### Requirement:", 16) == 0) {
                    /* push back: not portable; re-parse by recursion? */
                    /* instead, parse the rest from here manually */
                    size_t cur = strlen(spec->reqs[idx].body);
                    size_t add = strlen(line);
                    if (cur + add + 2 < sizeof(spec->reqs[idx].body)) {
                        if (cur > 0) spec->reqs[idx].body[cur++] = '\n';
                        memcpy(spec->reqs[idx].body + cur, line, add + 1);
                    }
                    break;
                }
                size_t cur = strlen(spec->reqs[idx].body);
                size_t add = strlen(line);
                if (cur + add + 2 < sizeof(spec->reqs[idx].body)) {
                    if (cur > 0) spec->reqs[idx].body[cur++] = '\n';
                    memcpy(spec->reqs[idx].body + cur, line, add + 1);
                }
            }
        }
    }
    fclose(f);
    return 0;
}

static int chg_delta_parse(const char* path, chg_spec_t* spec) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    memset(spec, 0, sizeof(*spec));
    char line[1024];
    int section = 0; /* 0=none, 1=added, 2=modified, 3=removed */
    long last_pos = 0;
    while (fgets(line, sizeof(line), f)) {
        chg_rtrim(line);
        if (strcmp(line, "## ADDED Requirements") == 0) {
            section = 1;
        } else if (strcmp(line, "## MODIFIED Requirements") == 0) {
            section = 2;
        } else if (strcmp(line, "## REMOVED Requirements") == 0) {
            section = 3;
        } else if (strncmp(line, "## ", 3) == 0 && section != 0) {
            /* new unknown section */
            section = 0;
        } else if (strncmp(line, "### Requirement:", 16) == 0 && section != 0) {
            /* parse this requirement and subsequent body */
            if (spec->n_reqs >= CHG_MAX_REQS) { fclose(f); return -1; }
            int idx = spec->n_reqs++;
            memset(&spec->reqs[idx], 0, sizeof(spec->reqs[idx]));
            chg_extract_value(line, spec->reqs[idx].title, sizeof(spec->reqs[idx].title));
            spec->reqs[idx].is_added = (section == 1);
            spec->reqs[idx].is_modified = (section == 2);
            spec->reqs[idx].is_removed = (section == 3);
            while (fgets(line, sizeof(line), f)) {
                chg_rtrim(line);
                if (strncmp(line, "### Requirement:", 16) == 0) {
                    /* push back by seeking */
                    last_pos = ftell(f) - strlen(line) - 1;
                    if (last_pos >= 0) fseek(f, last_pos, SEEK_SET);
                    break;
                }
                if ((strncmp(line, "## ADDED", 8) == 0 ||
                     strncmp(line, "## MODIFIED", 11) == 0 ||
                     strncmp(line, "## REMOVED", 10) == 0) && line[0] == '#') {
                    last_pos = ftell(f) - strlen(line) - 1;
                    if (last_pos >= 0) fseek(f, last_pos, SEEK_SET);
                    break;
                }
                size_t cur = strlen(spec->reqs[idx].body);
                size_t add = strlen(line);
                if (cur + add + 2 < sizeof(spec->reqs[idx].body)) {
                    if (cur > 0) spec->reqs[idx].body[cur++] = '\n';
                    memcpy(spec->reqs[idx].body + cur, line, add + 1);
                }
            }
        }
    }
    fclose(f);
    return 0;
}

static int chg_spec_find(chg_spec_t* spec, const char* title) {
    for (int i = 0; i < spec->n_reqs; i++) {
        if (strcmp(spec->reqs[i].title, title) == 0) return i;
    }
    return -1;
}

static int chg_spec_write(const char* path, const chg_spec_t* spec) {
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# Specification\n\n");
    if (spec->purpose[0]) {
        fprintf(f, "## Purpose\n\n%s\n\n", spec->purpose);
    } else {
        fprintf(f, "## Purpose\n\n_Describe this domain._\n\n");
    }
    fprintf(f, "## Requirements\n\n");
    for (int i = 0; i < spec->n_reqs; i++) {
        fprintf(f, "### Requirement: %s\n", spec->reqs[i].title);
        if (spec->reqs[i].body[0]) {
            fprintf(f, "%s\n", spec->reqs[i].body);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    return 0;
}

static int chg_apply_delta(const chg_spec_t* base, const chg_spec_t* delta,
                           chg_spec_t* out) {
    memcpy(out, base, sizeof(*out));
    for (int i = 0; i < delta->n_reqs; i++) {
        const chg_requirement_t* r = &delta->reqs[i];
        int idx = chg_spec_find(out, r->title);
        if (r->is_added) {
            if (idx >= 0) {
                /* conflict: existing requirement with same title */
                return -1;
            }
            if (out->n_reqs >= CHG_MAX_REQS) return -1;
            memcpy(&out->reqs[out->n_reqs++], r, sizeof(*r));
        } else if (r->is_modified) {
            if (idx < 0) return -1; /* cannot modify non-existent */
            memcpy(&out->reqs[idx], r, sizeof(*r));
        } else if (r->is_removed) {
            if (idx < 0) return -1; /* cannot remove non-existent */
            /* shift remaining */
            for (int j = idx; j < out->n_reqs - 1; j++) {
                out->reqs[j] = out->reqs[j + 1];
            }
            out->n_reqs--;
        }
    }
    return 0;
}

/* forward declarations for commands that need name resolution */
static int chg_resolve_change(jash_writer_t w, void* ctx,
                              const char* requested, char* out, size_t cap);
static int chg_build_status(const char* change_name, chg_schema_t* schema,
                            int* done, int* ready, int* blocked);

/* -------------------------------------------------------------------------- */
/* Verify engine                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    int total_tasks;
    int done_tasks;
    int total_reqs;
    int total_scenarios;
    int build_rc;
    int test_rc;
} chg_verify_result_t;

static void chg_count_tasks(const char* tasks_path, chg_verify_result_t* r) {
    FILE* f = fopen(tasks_path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "- [ ]", 5) == 0 || strncmp(p, "- [x]", 5) == 0 ||
            strncmp(p, "- [X]", 5) == 0) {
            r->total_tasks++;
            if (p[3] == 'x' || p[3] == 'X') r->done_tasks++;
        }
    }
    fclose(f);
}

static void chg_count_spec_coverage(const char* spec_path, chg_verify_result_t* r) {
    FILE* f = fopen(spec_path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        chg_rtrim(line);
        if (strncmp(line, "### Requirement:", 16) == 0) r->total_reqs++;
        if (strncmp(line, "#### Scenario:", 14) == 0) r->total_scenarios++;
    }
    fclose(f);
}

static int chg_run_build_test(jash_writer_t w, void* ctx) {
    /* prefer tests/test_suite.sh, fallback make */
    if (chg_file_exists("tests/test_suite.sh")) {
        jash_writef(w, ctx, "  running tests/test_suite.sh...\n");
#ifdef JASH_BLIBC
        char *av[] = { "bash", "tests/test_suite.sh", NULL };
        return bear_spawn("bash", av);
#else
        return system("bash tests/test_suite.sh >/dev/null 2>&1");
#endif
    }
    if (chg_file_exists("Makefile")) {
        jash_writef(w, ctx, "  running make...\n");
#ifdef JASH_BLIBC
        char *av[] = { "make", NULL };
        return bear_spawn("make", av);
#else
        return system("make >/dev/null 2>&1");
#endif
    }
    return -1; /* no build/test found */
}

static int chg_cmd_verify(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;

    char change_dir[CHG_MAX_PATH];
    snprintf(change_dir, sizeof(change_dir), "%s/%s", CHG_DIR, change_name);
    if (!chg_dir_exists(change_dir)) {
        jash_writef(w, ctx, "%serror:%s change '%s' not found\n", J_ROLE_ERR, J_RESET, change_name);
        return 1;
    }

    chg_verify_result_t v;
    memset(&v, 0, sizeof(v));

    char tasks_path[CHG_MAX_PATH];
    snprintf(tasks_path, sizeof(tasks_path), "%s/tasks.md", change_dir);
    chg_count_tasks(tasks_path, &v);

    char spec_path[CHG_MAX_PATH];
    snprintf(spec_path, sizeof(spec_path), "%s/specs/spec.md", change_dir);
    chg_count_spec_coverage(spec_path, &v);

    jash_writef(w, ctx, "%sverifying%s '%s'...\n", J_ROLE_ACCENT, J_RESET, change_name);

    /* completeness */
    jash_writef(w, ctx, "%sCompleteness:%s\n", J_ROLE_ACCENT, J_RESET);
    jash_writef(w, ctx, "  tasks: %d/%d complete\n", v.done_tasks, v.total_tasks);
    jash_writef(w, ctx, "  requirements: %d | scenarios: %d\n", v.total_reqs, v.total_scenarios);

    /* correctness: build/test */
    jash_writef(w, ctx, "%sCorrectness:%s\n", J_ROLE_ACCENT, J_RESET);
    int build_rc = chg_run_build_test(w, ctx);
    if (build_rc == -1) {
        jash_writef(w, ctx, "  %s⚠%s no build/test target found\n", J_ROLE_WARN, J_RESET);
    } else if (build_rc == 0) {
        jash_writef(w, ctx, "  %s✓%s build/test passed\n", J_ROLE_OK, J_RESET);
    } else {
        jash_writef(w, ctx, "  %s✗%s build/test failed (rc=%d)\n", J_ROLE_ERR, J_RESET, build_rc);
    }

    /* summary */
    int ready = (v.total_tasks > 0 && v.done_tasks == v.total_tasks) &&
                (build_rc == 0 || build_rc == -1);
    jash_writef(w, ctx, "%sSummary:%s\n", J_ROLE_ACCENT, J_RESET);
    if (ready) {
        jash_writef(w, ctx, "  %s✓%s ready to archive\n", J_ROLE_OK, J_RESET);
    } else {
        jash_writef(w, ctx, "  %s✗%s not ready to archive\n", J_ROLE_ERR, J_RESET);
    }
    return ready ? 0 : 1;
}

/* -------------------------------------------------------------------------- */
/* Change metadata                                                            */
/* -------------------------------------------------------------------------- */

static int chg_load_meta(const char* change_name, char* schema_name, size_t schema_cap,
                         char* status, size_t status_cap) {
    char path[CHG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s/.change.yaml", CHG_DIR, change_name);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "schema:", 7) == 0) {
            chg_extract_value(line, schema_name, schema_cap);
        } else if (strncmp(line, "status:", 7) == 0) {
            chg_extract_value(line, status, status_cap);
        }
    }
    fclose(f);
    return 0;
}

static int chg_meta_value(const char* change_name, const char* key, char* out, size_t cap) {
    char path[CHG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s/.change.yaml", CHG_DIR, change_name);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s:", key);
    size_t plen = strlen(prefix);
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, prefix, plen) == 0) {
            chg_extract_value(line, out, cap);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    if (cap) out[0] = '\0';
    return -1;
}

static int chg_update_meta_status(const char* change_name, const char* new_status) {
    char path[CHG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s/.change.yaml", CHG_DIR, change_name);
    char tmp[CHG_MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* in = fopen(path, "r");
    if (!in) return -1;
    FILE* out = fopen(tmp, "w");
    if (!out) { fclose(in); return -1; }
    char line[256];
    int wrote = 0;
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "status:", 7) == 0) {
            fprintf(out, "status: %s\n", new_status);
            wrote = 1;
        } else {
            fputs(line, out);
        }
    }
    fclose(in); fclose(out);
    if (!wrote) {
        FILE* append = fopen(tmp, "a");
        if (append) { fprintf(append, "status: %s\n", new_status); fclose(append); }
    }
    if (rename(tmp, path) != 0) return -1;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Commands                                                                   */
/* -------------------------------------------------------------------------- */

static int chg_cmd_schema_init(jash_state_t* s, jash_writer_t w, void* ctx,
                               const char* args) {
    (void)s; (void)ctx;
    char name[CHG_MAX_NAME];
    if (sscanf(args, "%127s", name) != 1) {
        jash_writef(w, ctx, "%susage:%s change schema init <name>\n", J_ROLE_ERR, J_RESET);
        return 1;
    }
    if (!chg_name_valid(name)) {
        jash_writef(w, ctx, "%serror:%s invalid schema name '%s'\n", J_ROLE_ERR, J_RESET, name);
        return 1;
    }
    const char* body = chg_builtin_schema(name);
    if (!body) {
        jash_writef(w, ctx, "%serror:%s unknown builtin schema '%s' (try spec-driven, bugfix, spike)\n",
                    J_ROLE_ERR, J_RESET, name);
        return 1;
    }
    char dir[CHG_MAX_PATH];
    snprintf(dir, sizeof(dir), "%s/%s", CHG_SCHEMA_DIR, name);
    if (chg_dir_exists(dir)) {
        jash_writef(w, ctx, "%serror:%s schema '%s' already exists\n", J_ROLE_ERR, J_RESET, name);
        return 1;
    }
    if (chg_mkdir_p(dir) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot create schema dir: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }
    char file[CHG_MAX_PATH];
    snprintf(file, sizeof(file), "%s/schema.yaml", dir);
    if (chg_write_file(file, body) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot write schema file: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }
    jash_writef(w, ctx, "%s✓%s created schema '%s' at %s\n", J_ROLE_OK, J_RESET, name, file);
    return 0;
}

static int chg_cmd_schema_fork(jash_state_t* s, jash_writer_t w, void* ctx,
                               const char* args) {
    (void)s; (void)ctx;
    char src[CHG_MAX_NAME], name[CHG_MAX_NAME];
    if (sscanf(args, "%127s %127s", src, name) != 2) {
        jash_writef(w, ctx, "%susage:%s change schema fork <src> <name>\n", J_ROLE_ERR, J_RESET);
        return 1;
    }
    if (!chg_name_valid(src) || !chg_name_valid(name)) {
        jash_writef(w, ctx, "%serror:%s invalid schema name\n", J_ROLE_ERR, J_RESET);
        return 1;
    }
    char src_file[CHG_MAX_PATH], dst_file[CHG_MAX_PATH];
    chg_schema_path(src, src_file, sizeof(src_file));
    if (!chg_file_exists(src_file)) {
        jash_writef(w, ctx, "%serror:%s source schema '%s' not found\n", J_ROLE_ERR, J_RESET, src);
        return 1;
    }
    char dst_dir[CHG_MAX_PATH];
    snprintf(dst_dir, sizeof(dst_dir), "%s/%s", CHG_SCHEMA_DIR, name);
    if (chg_dir_exists(dst_dir)) {
        jash_writef(w, ctx, "%serror:%s schema '%s' already exists\n", J_ROLE_ERR, J_RESET, name);
        return 1;
    }
    if (chg_mkdir_p(dst_dir) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot create schema dir: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }
    snprintf(dst_file, sizeof(dst_file), "%s/schema.yaml", dst_dir);
    FILE* in = fopen(src_file, "r");
    if (!in) {
        jash_writef(w, ctx, "%serror:%s cannot read source schema: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }
    FILE* out = fopen(dst_file, "w");
    if (!out) {
        fclose(in);
        jash_writef(w, ctx, "%serror:%s cannot write dest schema: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        if (strncmp(line, "name:", 5) == 0) {
            fprintf(out, "name: %s\n", name);
        } else {
            fputs(line, out);
        }
    }
    fclose(in); fclose(out);
    jash_writef(w, ctx, "%s✓%s forked schema '%s' → '%s'\n", J_ROLE_OK, J_RESET, src, name);
    return 0;
}

static int chg_create_change(jash_state_t* s, jash_writer_t w, void* ctx,
                             const char* name, const char* schema_name,
                             const char* goal_uuid) {
    (void)s;
    char schema_path[CHG_MAX_PATH];
    chg_schema_path(schema_name, schema_path, sizeof(schema_path));
    if (!chg_file_exists(schema_path)) {
        jash_writef(w, ctx, "%serror:%s schema '%s' not found. Run 'change schema init %s' first.\n",
                    J_ROLE_ERR, J_RESET, schema_name, schema_name);
        return 1;
    }

    char change_dir[CHG_MAX_PATH];
    snprintf(change_dir, sizeof(change_dir), "%s/%s", CHG_DIR, name);
    if (chg_dir_exists(change_dir)) {
        jash_writef(w, ctx, "%serror:%s change '%s' already exists\n", J_ROLE_ERR, J_RESET, name);
        return 1;
    }

    if (chg_mkdir_p(change_dir) != 0 ||
        chg_mkdir_p(CHG_ARCHIVE_DIR) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot create change dir: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }

    char uuid[64];
    chg_uuid(uuid, sizeof(uuid));

    char meta_path[CHG_MAX_PATH];
    snprintf(meta_path, sizeof(meta_path), "%s/.change.yaml", change_dir);
    FILE* meta = fopen(meta_path, "w");
    if (!meta) {
        jash_writef(w, ctx, "%serror:%s cannot write metadata: %s\n", J_ROLE_ERR, J_RESET, strerror(errno));
        return 1;
    }
    fprintf(meta, "uuid: %s\n", uuid);
    fprintf(meta, "name: %s\n", name);
    fprintf(meta, "schema: %s\n", schema_name);
    if (goal_uuid) fprintf(meta, "goal: %s\n", goal_uuid);
    fprintf(meta, "status: proposed\n");
    fprintf(meta, "created: %ld\n", (long)time(NULL));
    fclose(meta);

    jash_writef(w, ctx, "%s✓%s created change '%s' (%s) using schema '%s'\n",
                J_ROLE_OK, J_RESET, name, uuid, schema_name);
    jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_MUTED, change_dir, J_RESET);
    return 0;
}

static int chg_cmd_propose(jash_state_t* s, jash_writer_t w, void* ctx,
                           const char* args) {
    char name[CHG_MAX_NAME];
    char schema_name[CHG_MAX_NAME] = "spec-driven";
    char goal_uuid[128] = {0};

    if (sscanf(args, "%127s", name) != 1) {
        jash_writef(w, ctx, "%susage:%s change propose <name> [--schema <schema>] [--goal <uuid>]\n",
                    J_ROLE_ERR, J_RESET);
        return 1;
    }
    if (!chg_name_valid(name)) {
        jash_writef(w, ctx, "%serror:%s invalid change name '%s'\n", J_ROLE_ERR, J_RESET, name);
        return 1;
    }

    const char* p = args;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    while (*p) {
        if (strncmp(p, "--schema", 8) == 0) {
            p += 8;
            while (*p == ' ') p++;
            if (sscanf(p, "%127s", schema_name) != 1) {
                jash_writef(w, ctx, "%serror:%s --schema requires a value\n", J_ROLE_ERR, J_RESET);
                return 1;
            }
        } else if (strncmp(p, "--goal", 6) == 0) {
            p += 6;
            while (*p == ' ') p++;
            if (sscanf(p, "%127s", goal_uuid) != 1) {
                jash_writef(w, ctx, "%serror:%s --goal requires a value\n", J_ROLE_ERR, J_RESET);
                return 1;
            }
        } else {
            jash_writef(w, ctx, "%serror:%s unexpected argument '%s' (did you forget --schema?)\n",
                        J_ROLE_ERR, J_RESET, p);
            return 1;
        }
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
    }

    if (!chg_name_valid(schema_name)) {
        jash_writef(w, ctx, "%serror:%s invalid schema name '%s'\n", J_ROLE_ERR, J_RESET, schema_name);
        return 1;
    }

    return chg_create_change(s, w, ctx, name, schema_name,
                             goal_uuid[0] ? goal_uuid : NULL);
}

static int chg_resolve_change(jash_writer_t w, void* ctx,
                              const char* requested, char* out, size_t cap) {
    if (requested && *requested) {
        char name[CHG_MAX_NAME];
        if (sscanf(requested, "%127s", name) != 1) {
            jash_writef(w, ctx, "%serror:%s invalid change name\n", J_ROLE_ERR, J_RESET);
            return -1;
        }
        if (!chg_name_valid(name)) {
            jash_writef(w, ctx, "%serror:%s invalid change name '%s'\n", J_ROLE_ERR, J_RESET, name);
            return -1;
        }
        char dir[CHG_MAX_PATH];
        snprintf(dir, sizeof(dir), "%s/%s", CHG_DIR, name);
        if (!chg_dir_exists(dir)) {
            jash_writef(w, ctx, "%serror:%s change '%s' not found\n", J_ROLE_ERR, J_RESET, name);
            return -1;
        }
        snprintf(out, cap, "%s", name);
        return 0;
    }
    /* auto-detect: use the only active change */
    if (!chg_dir_exists(CHG_DIR)) {
        jash_writef(w, ctx, "%serror:%s no active changes\n", J_ROLE_ERR, J_RESET);
        return -1;
    }
    int count = 0;
    char last[CHG_MAX_NAME] = "";
    DIR* active = opendir(CHG_DIR);
    if (active) {
        struct dirent* ent;
        while ((ent = readdir(active)) != NULL) {
            if (strcmp(ent->d_name, "archive") == 0 || strcmp(ent->d_name, "schemas") == 0) continue;
            if (!chg_name_valid(ent->d_name)) continue;
            char meta[CHG_MAX_PATH];
            snprintf(meta, sizeof(meta), "%s/%s/.change.yaml", CHG_DIR, ent->d_name);
            if (chg_file_exists(meta)) {
                count++;
                snprintf(last, sizeof(last), "%s", ent->d_name);
            }
        }
        closedir(active);
    }
    if (count == 0) {
        jash_writef(w, ctx, "%serror:%s no active changes\n", J_ROLE_ERR, J_RESET);
        return -1;
    }
    if (count > 1) {
        jash_writef(w, ctx, "%serror:%s multiple active changes; specify one:\n", J_ROLE_ERR, J_RESET);
        active = opendir(CHG_DIR);
        if (active) {
            struct dirent* ent;
            while ((ent = readdir(active)) != NULL) {
                if (strcmp(ent->d_name, "archive") == 0 || strcmp(ent->d_name, "schemas") == 0) continue;
                if (!chg_name_valid(ent->d_name)) continue;
                char meta[CHG_MAX_PATH];
                snprintf(meta, sizeof(meta), "%s/%s/.change.yaml", CHG_DIR, ent->d_name);
                if (chg_file_exists(meta)) jash_writef(w, ctx, "  - %s\n", ent->d_name);
            }
            closedir(active);
        }
        return -1;
    }
    snprintf(out, cap, "%s", last);
    return 0;
}

static int chg_build_status(const char* change_name, chg_schema_t* schema,
                            int* done, int* ready, int* blocked) {
    char change_dir[CHG_MAX_PATH];
    snprintf(change_dir, sizeof(change_dir), "%s/%s", CHG_DIR, change_name);
    char schema_path[CHG_MAX_PATH];
    char schema_name[CHG_MAX_NAME] = "";
    char status_tmp[CHG_MAX_NAME] = "";
    chg_load_meta(change_name, schema_name, sizeof(schema_name), status_tmp, sizeof(status_tmp));
    chg_schema_path(schema_name, schema_path, sizeof(schema_path));
    if (chg_parse_schema(schema_path, schema) != 0) return -1;

    for (int i = 0; i < schema->n_artifacts; i++) {
        done[i] = chg_artifact_exists(change_dir, &schema->artifacts[i]) ? 1 : 0;
    }
    for (int i = 0; i < schema->n_artifacts; i++) {
        if (done[i]) { ready[i] = 0; blocked[i] = 0; continue; }
        ready[i] = chg_artifact_ready(schema, &schema->artifacts[i], done) ? 1 : 0;
        blocked[i] = ready[i] ? 0 : 1;
    }
    return 0;
}

static int chg_cmd_continue(jash_state_t* s, jash_writer_t w, void* ctx,
                            const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;

    chg_schema_t schema;
    int done[CHG_MAX_ARTIFACTS] = {0};
    int ready[CHG_MAX_ARTIFACTS] = {0};
    int blocked[CHG_MAX_ARTIFACTS] = {0};
    if (chg_build_status(change_name, &schema, done, ready, blocked) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot parse schema for '%s'\n", J_ROLE_ERR, J_RESET, change_name);
        return 1;
    }

    int all_done = 1;
    for (int i = 0; i < schema.n_artifacts; i++) {
        if (!done[i]) { all_done = 0; break; }
    }
    if (all_done) {
        jash_writef(w, ctx, "%s✓%s change '%s' has all artifacts; run 'change apply'\n",
                    J_ROLE_OK, J_RESET, change_name);
        return 0;
    }

    int created = -1;
    for (int i = 0; i < schema.n_artifacts; i++) {
        if (ready[i]) { created = i; break; }
    }
    if (created < 0) {
        jash_writef(w, ctx, "%serror:%s no artifacts ready (dependencies missing)\n", J_ROLE_ERR, J_RESET);
        jash_writef(w, ctx, "  artifact status for '%s':\n", change_name);
        for (int i = 0; i < schema.n_artifacts; i++) {
            const char* sym = done[i] ? "✓" : (blocked[i] ? "○" : "◆");
            const char* role = done[i] ? J_ROLE_OK : (blocked[i] ? J_ROLE_MUTED : J_ROLE_ACCENT);
            jash_writef(w, ctx, "  %s%s%s %s (%s)\n", role, sym, J_RESET,
                        schema.artifacts[i].id,
                        done[i] ? "done" : (blocked[i] ? "blocked" : "ready"));
        }
        return 1;
    }

    char change_dir[CHG_MAX_PATH];
    snprintf(change_dir, sizeof(change_dir), "%s/%s", CHG_DIR, change_name);
    if (chg_create_artifact(change_dir, change_name, &schema.artifacts[created], w, ctx) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot create artifact '%s'\n",
                    J_ROLE_ERR, J_RESET, schema.artifacts[created].id);
        return 1;
    }
    if (strcmp(schema.artifacts[created].id, "tasks") == 0) {
        chg_update_meta_status(change_name, "ready");
    }

    jash_writef(w, ctx, "%s✓%s created artifact '%s' for change '%s'\n",
                J_ROLE_OK, J_RESET, schema.artifacts[created].id, change_name);
    return 0;
}

static int chg_cmd_ff(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;

    chg_schema_t schema;
    int done[CHG_MAX_ARTIFACTS] = {0};
    int ready[CHG_MAX_ARTIFACTS] = {0};
    int blocked[CHG_MAX_ARTIFACTS] = {0};
    if (chg_build_status(change_name, &schema, done, ready, blocked) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot parse schema for '%s'\n", J_ROLE_ERR, J_RESET, change_name);
        return 1;
    }

    char change_dir[CHG_MAX_PATH];
    snprintf(change_dir, sizeof(change_dir), "%s/%s", CHG_DIR, change_name);
    int created = 0;
    for (int pass = 0; pass < schema.n_artifacts; pass++) {
        for (int i = 0; i < schema.n_artifacts; i++) {
            if (done[i]) continue;
            if (chg_artifact_ready(&schema, &schema.artifacts[i], done)) {
                if (chg_create_artifact(change_dir, change_name, &schema.artifacts[i], w, ctx) != 0) {
                    jash_writef(w, ctx, "%serror:%s cannot create artifact '%s'\n",
                                J_ROLE_ERR, J_RESET, schema.artifacts[i].id);
                    return 1;
                }
                jash_writef(w, ctx, "%s✓%s %s\n", J_ROLE_OK, J_RESET, schema.artifacts[i].id);
                done[i] = 1;
                created++;
            }
        }
    }

    if (created == 0) {
        jash_writef(w, ctx, "%s✓%s change '%s' already has all artifacts\n",
                    J_ROLE_OK, J_RESET, change_name);
    } else {
        chg_update_meta_status(change_name, "ready");
        jash_writef(w, ctx, "%s✓%s fast-forwarded '%s' (%d artifacts). Run 'change apply'.\n",
                    J_ROLE_OK, J_RESET, change_name, created);
    }
    return 0;
}

static int chg_sync_one_spec(const char* delta_root, const char* line,
                             jash_writer_t w, void* ctx) {
    const char* rel = line + strlen(delta_root);
    while (*rel == '/') rel++;
    char target[CHG_MAX_PATH];
    snprintf(target, sizeof(target), "specs/%s", rel);

    chg_spec_t base, delta, out;
    int has_base = chg_spec_parse(target, &base) == 0;
    if (!has_base) memset(&base, 0, sizeof(base));
    if (chg_delta_parse(line, &delta) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot parse delta spec %s\n", J_ROLE_ERR, J_RESET, line);
        return 0;
    }
    if (chg_apply_delta(&base, &delta, &out) != 0) {
        jash_writef(w, ctx, "%serror:%s conflict applying delta to %s\n", J_ROLE_ERR, J_RESET, target);
        return 0;
    }
    char target_dir[CHG_MAX_PATH];
    snprintf(target_dir, sizeof(target_dir), "specs/%s", rel);
    char* last_slash = strrchr(target_dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        chg_mkdir_p(target_dir);
    }
    if (chg_spec_write(target, &out) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot write %s\n", J_ROLE_ERR, J_RESET, target);
        return 0;
    }
    jash_writef(w, ctx, "%s✓%s synced %s\n", J_ROLE_OK, J_RESET, target);
    return 1;
}

static int chg_sync_specs_tree(const char* delta_root, const char* path,
                               int* synced, jash_writer_t w, void* ctx) {
    DIR* d = opendir(path);
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        char child[CHG_MAX_PATH];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) continue;
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            chg_sync_specs_tree(delta_root, child, synced, w, ctx);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = strlen(ent->d_name);
            if (len > 3 && strcmp(ent->d_name + len - 3, ".md") == 0)
                *synced += chg_sync_one_spec(delta_root, child, w, ctx);
        }
    }
    closedir(d);
    return 0;
}

static int chg_sync_specs(const char* change_name, jash_writer_t w, void* ctx) {
    char delta_dir[CHG_MAX_PATH];
    snprintf(delta_dir, sizeof(delta_dir), "%s/%s/specs", CHG_DIR, change_name);
    if (!chg_dir_exists(delta_dir)) {
        jash_writef(w, ctx, "%snote:%s no delta specs to sync\n", J_ROLE_WARN, J_RESET);
        return 0;
    }
    int synced = 0;
    if (chg_sync_specs_tree(delta_dir, delta_dir, &synced, w, ctx) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot list delta specs\n", J_ROLE_ERR, J_RESET);
        return 1;
    }
    if (synced == 0) {
        jash_writef(w, ctx, "%snote:%s no delta specs were synced\n", J_ROLE_WARN, J_RESET);
    }
    return 0;
}

static int chg_cmd_sync(jash_state_t* s, jash_writer_t w, void* ctx,
                        const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;
    jash_writef(w, ctx, "%ssyncing%s delta specs for '%s'...\n", J_ROLE_ACCENT, J_RESET, change_name);
    return chg_sync_specs(change_name, w, ctx);
}

static int chg_cmd_apply(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;

    chg_schema_t schema;
    int done[CHG_MAX_ARTIFACTS] = {0};
    int ready[CHG_MAX_ARTIFACTS] = {0};
    int blocked[CHG_MAX_ARTIFACTS] = {0};
    if (chg_build_status(change_name, &schema, done, ready, blocked) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot parse schema for '%s'\n", J_ROLE_ERR, J_RESET, change_name);
        return 1;
    }
    for (int i = 0; i < schema.n_artifacts; i++) {
        if (!done[i]) {
            jash_writef(w, ctx, "%serror:%s artifact '%s' is not ready; run 'change ff %s' first\n",
                        J_ROLE_ERR, J_RESET, schema.artifacts[i].id, change_name);
            return 1;
        }
    }

    jash_writef(w, ctx, "%scheckpoint%s before applying '%s'...\n", J_ROLE_ACCENT, J_RESET, change_name);
    jash_b_checkpoint(s, w, ctx, "");

    chg_update_meta_status(change_name, "applying");
    jash_writef(w, ctx, "%s✓%s change '%s' is now applying. Execute tasks in .changes/%s/tasks.md\n",
                J_ROLE_OK, J_RESET, change_name, change_name);
    return 0;
}

static int chg_cmd_archive(jash_state_t* s, jash_writer_t w, void* ctx,
                           const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;

    jash_writef(w, ctx, "%sarchiving%s '%s'...\n", J_ROLE_ACCENT, J_RESET, change_name);

    jash_writef(w, ctx, "%scheckpoint%s before archiving...\n", J_ROLE_ACCENT, J_RESET);
    jash_b_checkpoint(s, w, ctx, "");

    if (chg_sync_specs(change_name, w, ctx) != 0) {
        jash_writef(w, ctx, "%serror:%s sync failed; archive aborted\n", J_ROLE_ERR, J_RESET);
        return 1;
    }

    char src[CHG_MAX_PATH], dst[CHG_MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", CHG_DIR, change_name);
    char ts[32];
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot timestamp archive\n", J_ROLE_ERR, J_RESET);
        return 1;
    }
    struct tm* tm_info = gmtime(&now.tv_sec);
    strftime(ts, sizeof(ts), "%Y-%m-%d", tm_info);
    snprintf(dst, sizeof(dst), "%s/%s-%s", CHG_ARCHIVE_DIR, ts, change_name);
    if (chg_dir_exists(dst)) {
        jash_writef(w, ctx, "%serror:%s archive target already exists: %s\n", J_ROLE_ERR, J_RESET, dst);
        return 1;
    }
    if (rename(src, dst) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot archive '%s': %s\n", J_ROLE_ERR, J_RESET, change_name, strerror(errno));
        return 1;
    }
    jash_writef(w, ctx, "%s✓%s archived '%s' to %s\n", J_ROLE_OK, J_RESET, change_name, dst);
    return 0;
}

static int chg_cmd_rewind(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* args) {
    (void)s;
    char archive_name[CHG_MAX_NAME];
    if (!args || !*args || sscanf(args, "%127s", archive_name) != 1 ||
        !chg_name_valid(archive_name)) {
        jash_writef(w, ctx, "%susage:%s change rewind <archive-name>\n", J_ROLE_ERR, J_RESET);
        return 1;
    }
    char src[CHG_MAX_PATH];
    snprintf(src, sizeof(src), "%s/%s", CHG_ARCHIVE_DIR, archive_name);
    if (!chg_dir_exists(src)) {
        jash_writef(w, ctx, "%serror:%s archive '%s' not found\n", J_ROLE_ERR, J_RESET, archive_name);
        return 1;
    }
    /* restore using jash rewind to last checkpoint */
    jash_writef(w, ctx, "%srewinding%s to last checkpoint...\n", J_ROLE_ACCENT, J_RESET);
    jash_b_rewind(s, w, ctx, "");

    /* move change back to active; archive name is YYYY-MM-DD-<change-name> */
    char dst[CHG_MAX_PATH];
    const char* name = archive_name;
    if (strlen(archive_name) > 11 && archive_name[4] == '-' && archive_name[7] == '-' && archive_name[10] == '-') {
        name = archive_name + 11;
    }
    snprintf(dst, sizeof(dst), "%s/%s", CHG_DIR, name);
    if (chg_dir_exists(dst)) {
        jash_writef(w, ctx, "%serror:%s active change '%s' already exists\n", J_ROLE_ERR, J_RESET, name);
        return 1;
    }
    if (rename(src, dst) != 0) {
        jash_writef(w, ctx, "%serror:%s cannot restore '%s': %s\n", J_ROLE_ERR, J_RESET, archive_name, strerror(errno));
        return 1;
    }
    chg_update_meta_status(name, "proposed");
    jash_writef(w, ctx, "%s✓%s restored archive '%s' to active change '%s'\n",
                J_ROLE_OK, J_RESET, archive_name, name);
    return 0;
}

static int chg_cmd_recall(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* args) {
    (void)s;
    char change_name[CHG_MAX_NAME];
    if (chg_resolve_change(w, ctx, args, change_name, sizeof(change_name)) != 0) return 1;

    char uuid[128] = "";
    char schema[CHG_MAX_NAME] = "";
    char status[CHG_MAX_NAME] = "";
    char goal[128] = "";
    char created[64] = "";
    chg_meta_value(change_name, "uuid", uuid, sizeof(uuid));
    chg_meta_value(change_name, "schema", schema, sizeof(schema));
    chg_meta_value(change_name, "status", status, sizeof(status));
    chg_meta_value(change_name, "goal", goal, sizeof(goal));
    chg_meta_value(change_name, "created", created, sizeof(created));

    chg_schema_t sch;
    int done[CHG_MAX_ARTIFACTS] = {0};
    int ready[CHG_MAX_ARTIFACTS] = {0};
    int blocked[CHG_MAX_ARTIFACTS] = {0};
    int has_status = chg_build_status(change_name, &sch, done, ready, blocked) == 0;

    jash_writef(w, ctx, "%s# change recall:%s %s\n", J_ROLE_BANNER, J_RESET, change_name);
    jash_writef(w, ctx, "uuid:    %s\n", uuid[0] ? uuid : "?");
    jash_writef(w, ctx, "schema:  %s\n", schema[0] ? schema : "?");
    jash_writef(w, ctx, "status:  %s\n", status[0] ? status : "?");
    if (goal[0]) jash_writef(w, ctx, "goal:    %s\n", goal);
    if (created[0]) jash_writef(w, ctx, "created: %s\n", created);
    jash_writef(w, ctx, "path:    %s/%s\n", CHG_DIR, change_name);

    if (has_status) {
        jash_writef(w, ctx, "\n%sArtifacts:%s\n", J_ROLE_ACCENT, J_RESET);
        for (int i = 0; i < sch.n_artifacts; i++) {
            const char* state = done[i] ? "done" : (ready[i] ? "ready" : "blocked");
            const char* sym = done[i] ? "✓" : (ready[i] ? "◆" : "○");
            const char* role = done[i] ? J_ROLE_OK : (ready[i] ? J_ROLE_ACCENT : J_ROLE_MUTED);
            jash_writef(w, ctx, "  %s%s%s %-12s %s\n", role, sym, J_RESET, sch.artifacts[i].id, state);
        }
    }
    return 0;
}

static int chg_cmd_status(jash_state_t* s, jash_writer_t w, void* ctx,
                          const char* args) {
    (void)s;
    int json_mode = 0;
    if (args && strstr(args, "--json")) json_mode = 1;

    /* Collect schemas */
    char schemas[CHG_MAX_ARTIFACTS][CHG_MAX_NAME];
    int n_schemas = 0;
    if (chg_dir_exists(CHG_SCHEMA_DIR)) {
        DIR* d = opendir(CHG_SCHEMA_DIR);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL && n_schemas < CHG_MAX_ARTIFACTS) {
                if (!chg_name_valid(ent->d_name)) continue;
                char path[CHG_MAX_PATH];
                snprintf(path, sizeof(path), "%s/%s/schema.yaml", CHG_SCHEMA_DIR, ent->d_name);
                if (chg_file_exists(path)) {
                    snprintf(schemas[n_schemas], CHG_MAX_NAME, "%s", ent->d_name);
                    n_schemas++;
                }
            }
            closedir(d);
        }
    }

    /* Collect changes */
    struct { char name[CHG_MAX_NAME]; char schema[CHG_MAX_NAME]; char status[CHG_MAX_NAME]; char goal[128];
             int n_artifacts; char artifact_ids[CHG_MAX_ARTIFACTS][64]; int artifact_done[CHG_MAX_ARTIFACTS]; } changes[64];
    int n_changes = 0;
    if (chg_dir_exists(CHG_DIR)) {
        DIR* d = opendir(CHG_DIR);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL && n_changes < 64) {
                if (strcmp(ent->d_name, "archive") == 0 || strcmp(ent->d_name, "schemas") == 0) continue;
                if (!chg_name_valid(ent->d_name)) continue;
                const char* line = ent->d_name;
                char meta[CHG_MAX_PATH];
                snprintf(meta, sizeof(meta), "%s/%s/.change.yaml", CHG_DIR, line);
                if (chg_file_exists(meta)) {
                    snprintf(changes[n_changes].name, CHG_MAX_NAME, "%s", line);
                    chg_load_meta(line, changes[n_changes].schema, CHG_MAX_NAME,
                                  changes[n_changes].status, CHG_MAX_NAME);
                    chg_meta_value(line, "goal", changes[n_changes].goal, sizeof(changes[n_changes].goal));

                    chg_schema_t sch;
                    int done[CHG_MAX_ARTIFACTS] = {0};
                    int ready[CHG_MAX_ARTIFACTS] = {0};
                    int blocked[CHG_MAX_ARTIFACTS] = {0};
                    char schema_path[CHG_MAX_PATH];
                    chg_schema_path(changes[n_changes].schema, schema_path, sizeof(schema_path));
                    changes[n_changes].n_artifacts = 0;
                    if (chg_parse_schema(schema_path, &sch) == 0) {
                        chg_build_status(line, &sch, done, ready, blocked);
                        changes[n_changes].n_artifacts = sch.n_artifacts;
                        for (int i = 0; i < sch.n_artifacts; i++) {
                            snprintf(changes[n_changes].artifact_ids[i], 64, "%s", sch.artifacts[i].id);
                            changes[n_changes].artifact_done[i] = done[i];
                        }
                    }
                    n_changes++;
                }
            }
            closedir(d);
        }
    }

    /* Output */
    if (json_mode) {
        jash_writef(w, ctx, "{\n  \"schemas\": [");
        for (int i = 0; i < n_schemas; i++) {
            if (i) jash_writef(w, ctx, ",");
            jash_writef(w, ctx, "\n    \"%s\"", schemas[i]);
        }
        if (n_schemas) jash_writef(w, ctx, "\n  ");
        jash_writef(w, ctx, "],\n  \"changes\": [");
        for (int i = 0; i < n_changes; i++) {
            if (i) jash_writef(w, ctx, ",");
            jash_writef(w, ctx, "\n    {\n      \"name\": \"%s\",\n      \"schema\": \"%s\",\n      \"status\": \"%s\"",
                        changes[i].name, changes[i].schema, changes[i].status);
            if (changes[i].goal[0])
                jash_writef(w, ctx, ",\n      \"goal\": \"%s\"", changes[i].goal);
            jash_writef(w, ctx, ",\n      \"artifacts\": [");
            for (int j = 0; j < changes[i].n_artifacts; j++) {
                if (j) jash_writef(w, ctx, ",");
                jash_writef(w, ctx, "\n        {\"id\": \"%s\", \"status\": \"%s\"}",
                            changes[i].artifact_ids[j],
                            changes[i].artifact_done[j] ? "done" : "pending");
            }
            jash_writef(w, ctx, "\n      ]\n    }");
        }
        if (n_changes) jash_writef(w, ctx, "\n  ");
        jash_writef(w, ctx, "]\n}\n");
    } else {
        jash_writef(w, ctx, "%s# change status%s\n\n", J_ROLE_BANNER, J_RESET);
        jash_writef(w, ctx, "%sSchemas:%s\n", J_ROLE_ACCENT, J_RESET);
        if (!n_schemas) {
            jash_writef(w, ctx, "  (none)\n");
        } else {
            for (int i = 0; i < n_schemas; i++)
                jash_writef(w, ctx, "  %s%s%s\n", J_ROLE_OK, schemas[i], J_RESET);
        }
        jash_writef(w, ctx, "\n%sActive changes:%s\n", J_ROLE_ACCENT, J_RESET);
        if (!n_changes) {
            jash_writef(w, ctx, "  (none)\n");
        } else {
            for (int i = 0; i < n_changes; i++) {
                int ndone = 0;
                for (int j = 0; j < changes[i].n_artifacts; j++) if (changes[i].artifact_done[j]) ndone++;
                jash_writef(w, ctx, "  %s%-20s%s schema=%s status=%s",
                            J_ROLE_OK, changes[i].name, J_RESET, changes[i].schema, changes[i].status);
                if (changes[i].goal[0]) jash_writef(w, ctx, " goal=%s", changes[i].goal);
                if (changes[i].n_artifacts) jash_writef(w, ctx, " artifacts=%d/%d", ndone, changes[i].n_artifacts);
                jash_writef(w, ctx, "\n");
                for (int j = 0; j < changes[i].n_artifacts; j++) {
                    const char* sym = changes[i].artifact_done[j] ? "✓" : "○";
                    const char* role = changes[i].artifact_done[j] ? J_ROLE_OK : J_ROLE_MUTED;
                    jash_writef(w, ctx, "      %s%s%s %s\n", role, sym, J_RESET, changes[i].artifact_ids[j]);
                }
            }
        }
    }
    return 0;
}

static int chg_cmd_help(jash_writer_t w, void* ctx) {
    jash_writef(w, ctx,
        "%schange%s — sovereign spec-driven change engine\n\n"
        "Wave 1-5 commands:\n"
        "  change schema init <name>              create a builtin schema\n"
        "  change schema fork <src> <name>        copy and rename a schema\n"
        "  change propose <name> [--schema ...]   start a new change\n"
        "  change continue [<name>]               create next ready artifact\n"
        "  change ff [<name>]                     create all remaining artifacts\n"
        "  change apply [<name>]                  checkpoint and mark applying\n"
        "  change verify [<name>]                 deterministic completeness/build check\n"
        "  change sync [<name>]                   merge delta specs into specs/\n"
        "  change archive [<name>]                checkpoint, sync, and archive\n"
        "  change rewind <archive-name>           restore archived change\n"
        "  change recall [<name>]                 recall metadata, goal, artifacts\n"
        "  change status [--json]                 list schemas and active changes\n",
        J_ROLE_BANNER, J_RESET);
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Entry point                                                                */
/* -------------------------------------------------------------------------- */

static int change_legacy(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) return chg_cmd_help(w, ctx);

    char sub[64], rest[1024];
    rest[0] = '\0';
    int n = sscanf(args, "%63s %1023[^\n]", sub, rest);
    if (n < 1) return chg_cmd_help(w, ctx);

    if (strcmp(sub, "schema") == 0) {
        char op[64], opargs[1024];
        opargs[0] = '\0';
        if (sscanf(rest, "%63s %1023[^\n]", op, opargs) < 1) {
            jash_writef(w, ctx, "%susage:%s change schema init|fork ...\n", J_ROLE_ERR, J_RESET);
            return 1;
        }
        if (strcmp(op, "init") == 0) return chg_cmd_schema_init(s, w, ctx, opargs);
        if (strcmp(op, "fork") == 0) return chg_cmd_schema_fork(s, w, ctx, opargs);
        jash_writef(w, ctx, "%serror:%s unknown schema subcommand '%s'\n", J_ROLE_ERR, J_RESET, op);
        return 1;
    }

    if (strcmp(sub, "propose") == 0) return chg_cmd_propose(s, w, ctx, rest);
    if (strcmp(sub, "continue") == 0) return chg_cmd_continue(s, w, ctx, rest);
    if (strcmp(sub, "ff") == 0) return chg_cmd_ff(s, w, ctx, rest);
    if (strcmp(sub, "apply") == 0) return chg_cmd_apply(s, w, ctx, rest);
    if (strcmp(sub, "verify") == 0) return chg_cmd_verify(s, w, ctx, rest);
    if (strcmp(sub, "sync") == 0) return chg_cmd_sync(s, w, ctx, rest);
    if (strcmp(sub, "archive") == 0) return chg_cmd_archive(s, w, ctx, rest);
    if (strcmp(sub, "rewind") == 0) return chg_cmd_rewind(s, w, ctx, rest);
    if (strcmp(sub, "recall") == 0) return chg_cmd_recall(s, w, ctx, rest);
    if (strcmp(sub, "status") == 0) return chg_cmd_status(s, w, ctx, rest);

    jash_writef(w, ctx, "%serror:%s unknown change subcommand '%s'\n", J_ROLE_ERR, J_RESET, sub);
    return 1;
}

#include "change_native.inc"
