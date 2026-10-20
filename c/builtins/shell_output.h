// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_SHELL_OUTPUT_H
#define JASH_BUILTINS_SHELL_OUTPUT_H
#include "discovery_output.h"

/* The semantic provider is shared by JSON and the explicit JTV2 adapter. The
 * negative sentinel means that no structured selector was supplied; existing
 * human-mode parsers continue to receive their original argument string. */
#define JASH_SHELL_TEXT (-10000)
int jash_shell_state_try(jash_state_t*, jash_writer_t, void*,
                         const char* command, const char* arguments);
int jash_shell_env_data(jash_state_t*, jash_writer_t, void*);
int jash_shell_pwd_data(jash_writer_t, void*, const char* path, int physical);

/* Native state accessors; borrowed names stay valid until a state mutation. */
size_t jash_readonly_count(void);
const char* jash_readonly_at(size_t);
int jash_readonly_mark_checked(const char*);
size_t jash_shopt_count(void);
const char* jash_shopt_at(size_t, int*);
int jash_shopt_set(jash_state_t*, const char*, int);
size_t jash_trap_count(void);
int jash_trap_at(size_t, int*, const char**);
int jash_trap_resolve(const char*);
const char* jash_trap_name(int);
const char* jash_trap_action(int);
int jash_trap_update(int, const char*);
int jash_readonly_can_mark(const char*);
#endif
