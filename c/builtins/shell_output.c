// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Native shell-state observations. JSON and JTV2 project the same records.
#include "shell_output.h"
#include <core/argv.h>
#include <core/env.h>
#include <core/timeline.h>
#include <function_defs.h>
#include <parser/arithmetic.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <unistd.h>

extern int jash_readonly_is_set(const char*);
typedef jash_discovery_rows_t rows_t;
#define STR(r, text) jash_discovery_string((r), (text))
#define FIELD(name, value) {JDV_LITERAL(name), (value)}

typedef struct { jash_writer_t writer; void* context; const char* command; int mode; int admission_failed; } effect_context_t;
static int admit(rows_t* rows, effect_context_t* effect) {
    if (rows->effects) return 0;
    int status = jash_discovery_effects_begin(rows, effect->writer, effect->context,
        effect->command, effect->mode, "jash.shell-state.v1");
    if (status) effect->admission_failed = 1;
    return status;
}


static int emit(jash_writer_t writer, void* context, const char* command,
                int mode, rows_t* rows, int status) {
    int result = jash_discovery_emit_schema(writer, context, command, mode,
                    rows, status, "jash.shell-state.v1");
    jash_discovery_rows_free(rows);
    return result;
}

static int variable_name(const char* name, int array) {
    if (!name || !*name || strlen(name) >= JASH_ENV_KEY_LEN) return 0;
    const unsigned char* p = (const unsigned char*)name;
    if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || *p == '_')) return 0;
    for (p++; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_') continue;
        if (array && *p == '[' && p[1]) {
            const char* end = strchr((const char*)p + 1, ']');
            return end && !end[1] && end > (const char*)p + 1;
        }
        return 0;
    }
    return 1;
}

static void variable_row(rows_t* rows, jash_state_t* state, const char* name,
                          const char* operation, jdv_value_t before,
                          int changed, const char* error) {
    const char* value = jash_env_get(state, name);
    char attr[JASH_ENV_KEY_LEN + 32];
    snprintf(attr, sizeof attr, "__jash_int__%s", name);
    int integer = jash_env_get(state, attr) != NULL;
    snprintf(attr, sizeof attr, "__nameref__%s", name);
    jdv_record_input_t fields[] = {
        FIELD("kind", STR(rows, "variable")), FIELD("name", STR(rows, name)),
        FIELD("operation", STR(rows, operation)), FIELD("before", before),
        FIELD("value", STR(rows, value)), FIELD("present", jdv_value_bool(value != NULL)),
        FIELD("readonly", jdv_value_bool(jash_readonly_is_set(name))),
        FIELD("integer", jdv_value_bool(integer)),
        FIELD("nameref", STR(rows, jash_env_get(state, attr))),
        FIELD("changed", jdv_value_bool(changed)), FIELD("error", STR(rows, error))
    };
    jash_discovery_add(rows, fields, sizeof fields / sizeof *fields);
}

/* One initializer is one atomic environment batch. The existing flat array
 * model updates the named elements; observations do not invent an array type. */
static int array_initializer(rows_t* rows, jash_state_t* state, const char* name,
        const char* text, int integer, int lower, int upper, int readonly) {
    size_t size = strlen(text);
    if (size < 2 || text[size - 1] != ')') return 2;
    char* source = strndup(text + 1, size - 2);
    if (!source) return 125;
    jash_argv_t elements;
    int built = jash_argv_build(source, &elements); free(source);
    if (built < 0) return 2;
    size_t count = (size_t)elements.argc;
    char** keys = calloc(count + 1, sizeof(*keys));
    char** values = calloc(count + 1, sizeof(*values));
    jdv_value_t* prior = calloc(count + 1, sizeof(*prior));
    unsigned char* changed = calloc(count + 1, sizeof(*changed));
    int status = (!keys || !values || !prior || !changed) ? 125 : 0;
    unsigned long long next = 0;
    for (size_t i = 0; i < count && !status; i++) {
        const char* value = elements.argv[i];
        char key[JASH_ENV_KEY_LEN];
        if (value[0] == '[') {
            const char* close = strchr(value + 1, ']');
            if (!close || close[1] != '=' || close == value + 1) { status = 2; break; }
            size_t length = (size_t)(close - value - 1);
            int n = snprintf(key, sizeof key, "%s[%.*s]", name, (int)length, value + 1);
            if (n < 0 || (size_t)n >= sizeof key) { status = 2; break; }
            char* end; errno = 0;
            unsigned long long index = strtoull(value + 1, &end, 10);
            if (!errno && end == close && value[1] >= '0' && value[1] <= '9') next = index == ULLONG_MAX ? index : index + 1;
            value = close + 2;
        } else {
            int n = snprintf(key, sizeof key, "%s[%llu]", name, next);
            if (n < 0 || (size_t)n >= sizeof key || next == ULLONG_MAX) { status = 2; break; }
            next++;
        }
        if (jash_readonly_is_set(key)) { status = 1; break; }
        keys[i] = strdup(key); values[i] = strdup(value);
        if (!keys[i] || !values[i]) { status = 125; break; }
        for (char* c = values[i]; *c; c++) {
            if (lower && *c >= 'A' && *c <= 'Z') *c += 'a' - 'A';
            if (upper && *c >= 'a' && *c <= 'z') *c -= 'a' - 'A';
        }
        if (integer) {
            jash_arith_clear_error(); long n = jash_eval_arithmetic(values[i], state);
            if (jash_arith_error()) { status = 1; break; }
            char number[64]; snprintf(number, sizeof number, "%ld", n);
            free(values[i]); values[i] = strdup(number);
            if (!values[i]) { status = 125; break; }
        }
        const char* old = jash_env_get(state, key);
        changed[i] = !old || strcmp(old, values[i]);
        prior[i] = STR(rows, old);
        if (rows->error) status = 75;
    }
    if (!status && jash_env_apply_batch(state, (const char* const*)keys, (const char* const*)values, (int)count)) status = 1;
    if (!status && readonly && jash_readonly_mark_checked(name)) status = 1;
    if (!status) {
        for (size_t i = 0; i < count && !rows->error; i++) variable_row(rows, state, keys[i], "set", prior[i], changed[i], NULL);
        if (!count) variable_row(rows, state, name, "set", jdv_value_nothing(), 0, NULL);
    } else variable_row(rows, state, name, "set", jdv_value_nothing(), 0,
        status == 2 ? "invalid_array_initializer" : status == 125 ? "allocation_failed" : "array_update_failed");
    for (size_t i = 0; i < count; i++) { if (keys) free(keys[i]); if (values) free(values[i]); }
    free(keys); free(values); free(prior); free(changed); jash_argv_free(&elements);
    return status;
}

static int variables(rows_t* rows, jash_state_t* state, const char* command,
                      int argc, char** argv, effect_context_t* effect) {
    int deleting = !strcmp(command, "unset"), readonly = !strcmp(command, "readonly");
    int declaring = !strcmp(command, "declare") || !strcmp(command, "typeset");
    int array = !strcmp(command, "array"), printing = 0, functions = 0;
    int integer = 0, lower = 0, upper = 0, nameref = 0, index = 0;
    while (index < argc && argv[index][0] == '-' && argv[index][1]) {
        const char* option = argv[index++];
        if (!strcmp(option, "--")) break;
        for (const char* p = option + 1; *p; p++) {
            if (*p == 'p') printing = 1;
            else if (!strcmp(command, "export") && *p == 'n') deleting = 1;
            else if (deleting && *p == 'v') { /* variable namespace */ }
            else if (declaring && (*p == 'f' || *p == 'F')) functions = 1;
            else if (declaring && *p == 'r') readonly = 1;
            else if (declaring && *p == 'i') integer = 1;
            else if (declaring && *p == 'l') lower = 1;
            else if (declaring && *p == 'u') upper = 1;
            else if (declaring && *p == 'n') nameref = 1;
            else if (declaring && (*p == 'a' || *p == 'A' || *p == 'g' || *p == 'x')) { }
            else return 2;
        }
    }
    if (lower && upper) return 2;
    if (functions) {
        int rc = 0;
        for (int i = index; i < argc || (index == argc && state->functions && i - index < state->functions->count); i++) {
            const char* name = index == argc ? state->functions->funcs[i - index].name : argv[i];
            const char* body = state->functions ? jash_function_lookup(state->functions, name) : NULL;
            jdv_record_input_t fields[] = {
                FIELD("kind", STR(rows, "function")), FIELD("name", STR(rows, name)),
                FIELD("body", STR(rows, body)), FIELD("present", jdv_value_bool(body != NULL))
            };
            jash_discovery_add(rows, fields, 4);
            if (!body) rc = 1;
        }
        return rc;
    }
    if (index == argc) {
        if (deleting || array) return array ? 2 : 0;
        if (readonly && !declaring) {
            for (size_t i = 0; i < jash_readonly_count(); i++)
                variable_row(rows, state, jash_readonly_at(i), "query", jdv_value_nothing(), 0, NULL);
        } else {
            uint32_t count = jash_env_count(state);
            for (uint32_t i = 0; i < count; i++) {
                const char *name = NULL, *value = NULL;
                if (jash_env_at(state, i, &name, &value))
                    variable_row(rows, state, name, "query", jdv_value_nothing(), 0, NULL);
            }
        }
        return 0;
    }
    /* Validate the option surface before any state change. Per-operand failures
     * remain explicit so a later valid operand does not erase earlier errors. */
    int status = 0;
    for (int i = index; i < argc && !rows->error; i++) {
        const char* equal = strchr(argv[i], '=');
        size_t length = equal ? (size_t)(equal - argv[i]) : strlen(argv[i]);
        char name[JASH_ENV_KEY_LEN];
        if (!length || length >= sizeof name) {
            variable_row(rows, state, argv[i], "invalid", jdv_value_nothing(), 0, "invalid_identifier");
            status = 1; if (jash_discovery_effects_flush(rows)) return 125; continue;
        }
        memcpy(name, argv[i], length); name[length] = 0;
        if (!variable_name(name, declaring || array || deleting)) {
            variable_row(rows, state, name, "invalid", jdv_value_nothing(), 0, "invalid_identifier");
            status = 1; if (jash_discovery_effects_flush(rows)) return 125; continue;
        }
        const char* current = jash_env_get(state, name);
        jdv_value_t before = STR(rows, current);
        if (rows->error) break; /* No effect if the evidence cannot be retained. */
        const char* error = NULL;
        const char* operation = printing ? "query" : deleting ? "unset" : equal ? "set" : readonly ? "mark_readonly" : "query";
        int changed = 0;
        if (printing || (array && !equal)) {
            int found = current != NULL || jash_readonly_is_set(name);
            if (found) variable_row(rows, state, name, "query", jdv_value_nothing(), 0, NULL);
            uint32_t count = jash_env_count(state);
            for (uint32_t j = 0; j < count; j++) {
                const char *key = NULL, *value = NULL;
                if (jash_env_at(state, j, &key, &value) && !strncmp(key, name, length) && key[length] == '[') {
                    variable_row(rows, state, key, "query", jdv_value_nothing(), 0, NULL); found = 1;
                }
            }
            if (!found) {
                variable_row(rows, state, name, "query", jdv_value_nothing(), 0, "not_found");
                status = 1;
            }
            continue;
        }
        if (deleting || equal || readonly || (declaring && !current && !nameref)) {
            int admission = admit(rows, effect);
            if (admission) return admission;
        }
        if ((deleting || equal) && jash_readonly_is_set(name)) error = "readonly_variable";
        else if (readonly && !jash_readonly_can_mark(name)) error = "readonly_limit";
        else if (declaring && equal && equal[1] == '(' && !nameref) {
            int result = array_initializer(rows, state, name, equal + 1, integer, lower, upper, readonly);
            if (result) status = result;
            if (jash_discovery_effects_flush(rows)) return 125;
            continue;
        }
        else if (deleting) {
            if (equal) error = "invalid_identifier";
            else {
                changed = current != NULL;
                (void)jash_env_unset(state, name);
                for (uint32_t j = 0; j < jash_env_count(state);) {
                    const char *key = NULL, *value = NULL;
                    if (jash_env_at(state, j, &key, &value) && !strncmp(key, name, length) && key[length] == '[') {
                        char saved[JASH_ENV_KEY_LEN]; memcpy(saved, key, strlen(key) + 1);
                        (void)jash_env_unset(state, saved); changed = 1;
                    } else j++;
                }
            }
        } else if (equal || (declaring && !current && !nameref)) {
            const char* value = equal ? equal + 1 : "";
            char* converted = strdup(value);
            if (!converted) error = "allocation_failed";
            else {
                for (char* p = converted; *p; p++) {
                    if (lower && *p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
                    if (upper && *p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
                }
                char number[64], attr[JASH_ENV_KEY_LEN + 32];
                const char* keys[2] = {name, NULL};
                const char* values[2] = {converted, NULL};
                int count = 1;
                if (integer) {
                    jash_arith_clear_error();
                    long result = jash_eval_arithmetic(converted, state);
                    if (jash_arith_error()) error = "arithmetic_error";
                    snprintf(number, sizeof number, "%ld", result);
                    values[0] = number;
                    snprintf(attr, sizeof attr, "__jash_int__%s", name);
                    keys[1] = attr; values[1] = "1"; count = 2;
                }
                if (nameref) {
                    snprintf(attr, sizeof attr, "__nameref__%s", name);
                    keys[0] = attr;
                    if (!variable_name(converted, 1)) error = "invalid_nameref";
                }
                if (strlen(keys[0]) >= JASH_ENV_KEY_LEN || (count == 2 && strlen(keys[1]) >= JASH_ENV_KEY_LEN)) error = "attribute_name_limit";
                if (!error) {
                    changed = !current || strcmp(current, values[0]) != 0;
                    if (jash_env_apply_batch(state, keys, values, count)) { error = "environment_update_failed"; changed = 0; }
                }
                free(converted);
            }
        }
        if (!error && readonly) {
            int prior = jash_readonly_is_set(name);
            if (jash_readonly_mark_checked(name)) error = "readonly_limit";
            else changed |= !prior;
        }
        if (error) status = 1;
        variable_row(rows, state, name, operation, before, changed, error);
        if (jash_discovery_effects_flush(rows)) return 125;
    }
    return status;
}

static void option_row(rows_t* rows, const char* name, int enabled, int implemented) {
    jdv_record_input_t fields[] = {
        FIELD("kind", STR(rows, "shell_option")), FIELD("name", STR(rows, name)),
        FIELD("enabled", jdv_value_bool(enabled)), FIELD("behavior_implemented", jdv_value_bool(implemented))
    };
    jash_discovery_add(rows, fields, 4);
}

static int shell_options(rows_t* rows, jash_state_t* state, int argc, char** argv, effect_context_t* effect) {
    int positionals = -1;
    /* Validate every option before applying any of them. */
    for (int pass = 0; pass < 2; pass++) {
        if (pass && argc && !(argc == 1 && (!strcmp(argv[0], "-o") || !strcmp(argv[0], "+o")))) {
            int admission = admit(rows, effect);
            if (admission) return admission;
        }
        for (int i = 0; i < argc; i++) {
            const char* arg = argv[i];
            if (!strcmp(arg, "--")) { positionals = i + 1; break; }
            if (!strcmp(arg, "-o") || !strcmp(arg, "+o")) {
                if (i + 1 == argc) continue;
                int rc = jash_shell_opt_long(pass ? state : NULL, argv[++i], arg[0] == '-');
                if (rc) return 2;
            } else if ((arg[0] == '-' || arg[0] == '+') && arg[1]) {
                for (const char* p = arg + 1; *p; p++)
                    if (jash_shell_opt_short(pass ? state : NULL, *p, arg[0] == '-')) return 2;
            } else { positionals = i; break; }
        }
    }
    if (positionals >= 0) {
        if (jash_env_replace_positionals(state, (const char* const*)(argv + positionals), argc - positionals)) return 1;
        for (int i = positionals; i < argc; i++) {
            jdv_record_input_t fields[] = {
                FIELD("kind", STR(rows, "positional")), FIELD("index", jdv_value_u64((uint64_t)(i - positionals + 1))),
                FIELD("value", STR(rows, argv[i]))
            };
            jash_discovery_add(rows, fields, 3);
        }
    }
#define OPTION(name, supported) option_row(rows, #name, state->shell_opts.name, (supported))
    OPTION(errexit, 1); OPTION(nounset, 1); OPTION(xtrace, 1); OPTION(verbose, 1);
    OPTION(noglob, 1); OPTION(noclobber, 1); OPTION(monitor, 0); OPTION(allexport, 0);
    OPTION(notify, 0); OPTION(hashall, 0); OPTION(keyword, 0); OPTION(privileged, 0);
    OPTION(onecmd, 0); OPTION(pipefail, 1); OPTION(posix, 0); OPTION(emacs, 1);
    OPTION(vi, 1); OPTION(errtrace, 1);
#undef OPTION
    return 0;
}

static int shopts(rows_t* rows, jash_state_t* state, int argc, char** argv, effect_context_t* effect) {
    int index = 0, action = -1, quiet = 0;
    for (; index < argc && argv[index][0] == '-'; index++) {
        const char* arg = argv[index];
        if (!strcmp(arg, "--")) { index++; break; }
        for (const char* p = arg + 1; *p; p++) {
            if (*p == 's') action = 1;
            else if (*p == 'u') action = 0;
            else if (*p == 'q') quiet = 1;
            else if (*p != 'p') return 2;
        }
    }
    int status = 0;
    for (size_t i = 0; i < jash_shopt_count() && !rows->error; i++) {
        int value;
        const char* name = jash_shopt_at(i, &value);
        int selected = index == argc;
        for (int j = index; j < argc; j++) if (!strcmp(name, argv[j])) selected = 1;
        if (!selected) continue;
        if (action >= 0 && index < argc) {
            int admission = admit(rows, effect);
            if (admission) return admission;
            jash_shopt_set(state, name, action); value = action;
        }
        if (quiet && !value) status = 1;
        int implemented = !strcmp(name, "lastpipe") || !strcmp(name, "nullglob") ||
                          !strcmp(name, "failglob") || !strcmp(name, "nocaseglob");
        option_row(rows, name, value, implemented);
        if (jash_discovery_effects_flush(rows)) return 125;
    }
    for (int i = index; i < argc; i++) {
        int found = 0;
        for (size_t j = 0; j < jash_shopt_count(); j++)
            if (!strcmp(jash_shopt_at(j, NULL), argv[i])) found = 1;
        if (!found) {
            jdv_record_input_t fields[] = {FIELD("name", STR(rows, argv[i])), FIELD("error", STR(rows, "unknown_option"))};
            jash_discovery_add(rows, fields, 2); status = 1;
            if (jash_discovery_effects_flush(rows)) return 125;
        }
    }
    return status;
}

static int mask(rows_t* rows, int argc, char** argv, effect_context_t* effect) {
    int index = 0;
    if (index < argc && !strcmp(argv[index], "-S")) index++;
    if (index < argc && !strcmp(argv[index], "--")) index++;
    if (argc - index > 1) return 2;
    unsigned long value = 0;
    if (index < argc) {
        char* end; errno = 0;
        value = strtoul(argv[index], &end, 8);
        if (errno || !argv[index][0] || *end || value > 0777u || argv[index][0] == '-') return 2;
    }
    mode_t previous = umask(0); umask(previous);
    mode_t current = index < argc ? (mode_t)value : previous;
    if (index < argc) {
        int admission = admit(rows, effect);
        if (admission) return admission;
        umask(current);
    }
    char octal[5]; snprintf(octal, sizeof octal, "%04o", (unsigned)current);
    jdv_record_input_t fields[] = {
        FIELD("kind", STR(rows, "umask")), FIELD("before", jdv_value_u64(previous)),
        FIELD("mask", jdv_value_u64(current)), FIELD("octal", STR(rows, octal)),
        FIELD("changed", jdv_value_bool(current != previous))
    };
    jash_discovery_add(rows, fields, 5);
    return 0;
}

typedef struct { char flag; int resource; const char* name; uint64_t scale; } resource_t;
static const resource_t resources[] = {
    {'c',RLIMIT_CORE,"core_file_bytes",512}, {'d',RLIMIT_DATA,"data_bytes",1024},
    {'f',RLIMIT_FSIZE,"file_bytes",512}, {'n',RLIMIT_NOFILE,"open_files",1},
    {'s',RLIMIT_STACK,"stack_bytes",1024}, {'t',RLIMIT_CPU,"cpu_seconds",1},
#ifdef RLIMIT_NPROC
    {'u',RLIMIT_NPROC,"processes",1},
#endif
#ifdef RLIMIT_AS
    {'v',RLIMIT_AS,"address_space_bytes",1024},
#endif
#ifdef RLIMIT_MEMLOCK
    {'l',RLIMIT_MEMLOCK,"locked_bytes",1024},
#endif
#ifdef RLIMIT_RSS
    {'m',RLIMIT_RSS,"resident_bytes",1024},
#endif
};

static int limits(rows_t* rows, int argc, char** argv, effect_context_t* effect) {
    char flag = 'f'; int hard = 0, soft = 0, all = 0, index = 0;
    for (; index < argc && argv[index][0] == '-'; index++) {
        if (!strcmp(argv[index], "--")) { index++; break; }
        for (const char* p = argv[index] + 1; *p; p++) {
            if (*p == 'H') hard = 1;
            else if (*p == 'S') soft = 1;
            else if (*p == 'a') all = 1;
            else {
                int found = 0;
                for (size_t i = 0; i < sizeof resources / sizeof *resources; i++)
                    if (resources[i].flag == *p) found = 1;
                if (!found) return 2;
                flag = *p;
            }
        }
    }
    if (argc - index > 1 || (all && index < argc)) return 2;
    int status = 0;
    for (size_t i = 0; i < sizeof resources / sizeof *resources; i++) {
        const resource_t* r = &resources[i];
        if (!all && r->flag != flag) continue;
        struct rlimit before = {0}, after = {0};
        const char* error = NULL;
        if (getrlimit(r->resource, &before)) error = "getrlimit_failed";
        after = before;
        if (!error && index < argc) {
            rlim_t value = RLIM_INFINITY;
            if (strcmp(argv[index], "unlimited")) {
                char* end; errno = 0;
                unsigned long long parsed = strtoull(argv[index], &end, 10);
                if (errno || !argv[index][0] || argv[index][0] == '-' || *end ||
                    parsed > UINT64_MAX / r->scale ||
                    (uint64_t)(rlim_t)(parsed * r->scale) != parsed * r->scale) return 2;
                value = (rlim_t)(parsed * r->scale);
                if (value == RLIM_INFINITY) return 2;
            }
            if (hard) after.rlim_max = value;
            if (soft || !hard) after.rlim_cur = value;
            int admission = admit(rows, effect);
            if (admission) return admission;
            if (setrlimit(r->resource, &after)) { error = "setrlimit_failed"; after = before; }
        }
        if (error) status = 1;
        jdv_record_input_t fields[] = {
            FIELD("kind", STR(rows, "resource_limit")), FIELD("name", STR(rows, r->name)),
            FIELD("display_scale", jdv_value_u64(r->scale)),
            FIELD("soft", after.rlim_cur == RLIM_INFINITY ? jdv_value_nothing() : jdv_value_u64(after.rlim_cur)),
            FIELD("hard", after.rlim_max == RLIM_INFINITY ? jdv_value_nothing() : jdv_value_u64(after.rlim_max)),
            FIELD("soft_unlimited", jdv_value_bool(after.rlim_cur == RLIM_INFINITY)),
            FIELD("hard_unlimited", jdv_value_bool(after.rlim_max == RLIM_INFINITY)),
            FIELD("changed", jdv_value_bool(before.rlim_cur != after.rlim_cur || before.rlim_max != after.rlim_max)),
            FIELD("error", STR(rows, error))
        };
        jash_discovery_add(rows, fields, 9);
    }
    return status;
}

static int cpu_times(rows_t* rows, int argc) {
    if (argc) return 2;
    struct tms value;
    long rate = sysconf(_SC_CLK_TCK);
    if (rate <= 0 || times(&value) == (clock_t)-1) return 1;
    jdv_record_input_t fields[] = {
        FIELD("kind", STR(rows, "cpu_times")), FIELD("ticks_per_second", jdv_value_u64((uint64_t)rate)),
        FIELD("shell_user_ticks", jdv_value_i64(value.tms_utime)),
        FIELD("shell_system_ticks", jdv_value_i64(value.tms_stime)),
        FIELD("children_user_ticks", jdv_value_i64(value.tms_cutime)),
        FIELD("children_system_ticks", jdv_value_i64(value.tms_cstime))
    };
    jash_discovery_add(rows, fields, 6);
    return 0;
}

static void trap_row(rows_t* rows, const char* requested, int signal,
                     const char* operation, jdv_value_t before, int changed,
                     int error) {
    const char* action = jash_trap_action(signal);
    const char* code = !error ? NULL : error == EINVAL ? "invalid_signal" :
        error == ENOTSUP ? "signal_not_catchable" : error == E2BIG ? "action_limit" :
        error == ENOSPC ? "handler_limit" : "signal_install_failed";
    jdv_record_input_t fields[] = {
        FIELD("kind", STR(rows, "trap")), FIELD("requested", STR(rows, requested)),
        FIELD("signal", signal ? jdv_value_i64(signal) : jdv_value_nothing()),
        FIELD("name", STR(rows, jash_trap_name(signal))), FIELD("operation", STR(rows, operation)),
        FIELD("before", before), FIELD("action", STR(rows, action)),
        FIELD("present", jdv_value_bool(action != NULL)),
        FIELD("ignored", jdv_value_bool(action && !*action)),
        FIELD("changed", jdv_value_bool(changed)), FIELD("error", STR(rows, code)),
        FIELD("errno", jdv_value_u64((uint64_t)error))
    };
    jash_discovery_add(rows, fields, sizeof fields / sizeof *fields);
}

static int trap_rows(rows_t* rows, jash_state_t* state, int argc, char** argv, effect_context_t* effect) {
    (void)state;
    int index = 0, list = 0, printing = 0;
    for (; index < argc && argv[index][0] == '-' && argv[index][1]; index++) {
        if (!strcmp(argv[index], "--")) { index++; break; }
        for (const char* p = argv[index] + 1; *p; p++) {
            if (*p == 'l') list = 1;
            else if (*p == 'p') printing = 1;
            else return 2;
        }
    }
    if (list) {
        if (index < argc) return 2;
        for (int sig = -4; sig <= 63; sig++) {
            const char* name = jash_trap_name(sig);
            if (name) trap_row(rows, name, sig, "list", jdv_value_nothing(), 0, 0);
        }
        return 0;
    }
    if (index == argc) {
        for (size_t i = 0; i < jash_trap_count(); i++) {
            int sig; const char* action;
            if (jash_trap_at(i, &sig, &action))
                trap_row(rows, jash_trap_name(sig), sig, "query", jdv_value_nothing(), 0, 0);
        }
        return 0;
    }
    const char* action = NULL;
    if (!printing) {
        const char* first = argv[index];
        int sig = jash_trap_resolve(first);
        if (!(sig && (index + 1 == argc || (*first >= '0' && *first <= '9')))) {
            action = argv[index++];
            if (index == argc) return 2;
            if (!strcmp(action, "-")) action = NULL;
        }
    }
    int status = 0;
    for (; index < argc && !rows->error; index++) {
        int sig = jash_trap_resolve(argv[index]);
        const char* old = jash_trap_action(sig);
        jdv_value_t before = STR(rows, old);
        int changed = (old == NULL) != (action == NULL) || (old && action && strcmp(old, action));
        if (rows->error) break;
        if (sig && !printing) {
            int admission = admit(rows, effect);
            if (admission) return admission;
        }
        int error = !sig ? EINVAL : printing ? 0 : jash_trap_update(sig, action);
        if (error) status = 1;
        trap_row(rows, argv[index], sig, printing ? "query" : action ? "set" : "reset",
                 before, !printing && !error && changed, error);
        if (jash_discovery_effects_flush(rows)) return 125;
    }
    return status;
}

int jash_shell_env_data(jash_state_t* state, jash_writer_t writer, void* context) {
    rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "env", JASH_DISCOVERY_DATA, "allocation_failed", 125);
    uint32_t count = jash_env_count(state);
    for (uint32_t i = 0; i < count; i++) {
        const char *key = NULL, *value = NULL;
        if (!jash_env_at(state, i, &key, &value)) continue;
        jdv_record_input_t fields[] = {FIELD("name", STR(&rows, key)), FIELD("value", STR(&rows, value))};
        jash_discovery_add(&rows, fields, 2);
    }
    return emit(writer, context, "env", JASH_DISCOVERY_DATA, &rows, 0);
}

int jash_shell_pwd_data(jash_writer_t writer, void* context, const char* path, int physical) {
    rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "pwd", JASH_DISCOVERY_DATA, "allocation_failed", 125);
    jdv_record_input_t fields[] = {
        FIELD("cwd", jash_discovery_path(&rows, path)), FIELD("physical", jdv_value_bool(physical))
    };
    jash_discovery_add(&rows, fields, 2);
    return emit(writer, context, "pwd", JASH_DISCOVERY_DATA, &rows, 0);
}

int jash_shell_state_try(jash_state_t* state, jash_writer_t writer, void* context,
                         const char* command, const char* arguments) {
    jash_argv_t words;
    if (jash_argv_build(arguments, &words) < 0) return JASH_SHELL_TEXT;
    int mode;
    int selected = jash_discovery_format(words.argc, words.argv, &mode);
    if (!selected) { jash_argv_free(&words); return JASH_SHELL_TEXT; }
    if (selected < 0) {
        jash_argv_free(&words);
        return jash_discovery_error(writer, context, command, mode, "conflicting_formats", 2);
    }
    int count = 0, ended = 0;
    for (int i = 0; i < words.argc; i++) {
        if (!strcmp(words.argv[i], "--")) ended = 1;
        if (!ended && jash_discovery_format_flag(words.argv[i])) continue;
        words.argv[count++] = words.argv[i];
    }
    rows_t rows;
    if (jash_discovery_rows_init(&rows)) {
        jash_argv_free(&words);
        return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125);
    }
    int status;
    effect_context_t effect = {writer, context, command, mode, 0};
    if (!strcmp(command, "set")) status = shell_options(&rows, state, count, words.argv, &effect);
    else if (!strcmp(command, "shopt")) status = shopts(&rows, state, count, words.argv, &effect);
    else if (!strcmp(command, "umask")) status = mask(&rows, count, words.argv, &effect);
    else if (!strcmp(command, "ulimit")) status = limits(&rows, count, words.argv, &effect);
    else if (!strcmp(command, "times")) status = cpu_times(&rows, count);
    else if (!strcmp(command, "trap")) status = trap_rows(&rows, state, count, words.argv, &effect);
    else status = variables(&rows, state, command, count, words.argv, &effect);
    jash_argv_free(&words);
    if (effect.admission_failed) { jash_discovery_rows_free(&rows); return status; }
    if (rows.effects) return emit(writer, context, command, mode, &rows, status);
    if (status == 2 || (status && !rows.count)) {
        jash_discovery_rows_free(&rows);
        return jash_discovery_error(writer, context, command, mode,
                    status == 2 ? "invalid_options" : "operation_failed", status);
    }
    return emit(writer, context, command, mode, &rows, status);
}

/* These legacy parsers receive already unquoted text in the human path. Keep
 * that compatibility while preserving exact argv for structured invocations. */
static int shell_argv(jash_builtin_fn legacy, const char* command,
                       jash_state_t* state, jash_writer_t writer, void* context,
                       int argc, char** argv) {
    char* arguments = jash_argv_join(argc, argv);
    if (!arguments) return 125;
    int status = jash_shell_state_try(state, writer, context, command, arguments);
    free(arguments);
    if (status != JASH_SHELL_TEXT) return status;
    return jash_discovery_text(legacy, state, writer, context, argc, argv);
}
#define SHELL_ARGV(name) \
int jash_b_##name##_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) { \
    return shell_argv(jash_b_##name, #name, s, w, ctx, argc, argv); \
}
SHELL_ARGV(array)
SHELL_ARGV(unset)
SHELL_ARGV(shopt)
SHELL_ARGV(umask)
SHELL_ARGV(ulimit)
SHELL_ARGV(times)
#undef SHELL_ARGV
