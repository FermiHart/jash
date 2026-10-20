/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_PLUGIN_H
#define JASH_PLUGIN_H

#include <core/state.h>
#include <theme/io.h>
#include <builtins/builtin.h>

#define JASH_PLUGIN_API_VERSION 1

typedef struct {
    const char* name;
    const char* version;
    const char* description;
} jash_plugin_info_t;

typedef struct {
    const char* name;
    jash_builtin_fn fn;
    const char* help;
} jash_plugin_builtin_t;

typedef struct {
    jash_plugin_info_t info;
    const jash_plugin_builtin_t* builtins;
    void* internal;
} jash_plugin_handle_t;

typedef jash_plugin_handle_t* (*jash_plugin_init_fn)(void);
typedef void (*jash_plugin_shutdown_fn)(jash_plugin_handle_t* handle);

#define JASH_PLUGIN_MAX 8

int   jash_plugin_load(const char* path);
int   jash_plugin_unload(const char* name);
void  jash_plugin_unload_all(void);
int   jash_plugin_list(jash_writer_t w, void* ctx);
/* Read-only loaded metadata. Pointers remain valid until load/unload. */
int   jash_plugin_count(void);
const jash_plugin_info_t* jash_plugin_info_at(int index);
const char* jash_plugin_path_at(int index);
const jash_builtin_t* jash_plugin_lookup_builtin(const char* name);

#endif
