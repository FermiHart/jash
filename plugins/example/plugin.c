/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#include "../../c/plugin.h"
#include <string.h>

static int hello_fn(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s; (void)args;
    jash_writes(w, ctx, "hello from plugin!\n");
    return 0;
}

static jash_plugin_handle_t handle;

jash_plugin_handle_t* jash_plugin_init(void) {
    handle.info.name = "example";
    handle.info.version = "1.0";
    handle.info.description = "Example plugin";
    static jash_plugin_builtin_t builtins[] = {
        { "hello", hello_fn, "print hello from plugin" },
        { NULL, NULL, NULL }
    };
    handle.builtins = builtins;
    handle.internal = 0;
    return &handle;
}
