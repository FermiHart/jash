/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#include "plugin.h"
#include <dlfcn.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <limits.h>
#include <errno.h>

#define MAX_PLUGIN_SIZE (50 * 1024 * 1024) // 50MB Anti-DoS

typedef struct {
    void* dl_handle;
    jash_plugin_handle_t* plugin;
    jash_plugin_shutdown_fn shutdown;
    char path[PATH_MAX];
    dev_t device;
    ino_t inode;
} plugin_entry_t;

static plugin_entry_t plugins[JASH_PLUGIN_MAX];
static int plugin_count = 0;

static int j_streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int jash_plugin_load(const char* path) {
    if (!path || !*path) return -1;
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    if (plugin_count >= JASH_PLUGIN_MAX) return -1;

    /* SECURITY (BEARJASH-001/TOCTOU): Hardened plugin loader.
     * 1. Open FD first to freeze the file identity (prevents TOCTOU race).
     * 2. fstat on the FD (not the path).
     * 3. Validate ownership, permissions, size, type.
     * 4. dlopen via /proc/self/fd/%d (Linux) or /dev/fd/%d (macOS).
     * F E R M I ∞ H A R T */
#ifndef O_NOFOLLOW
    struct stat pre_st;
    if (lstat(path, &pre_st) != 0 || S_ISLNK(pre_st.st_mode)) return -1;
#endif

    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
    if (fd < 0) return -1;
#ifndef O_CLOEXEC
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].device == st.st_dev && plugins[i].inode == st.st_ino) {
            close(fd); errno = EEXIST; return -1;
        }
    }
#ifndef O_NOFOLLOW
    if (st.st_dev != pre_st.st_dev || st.st_ino != pre_st.st_ino) { close(fd); return -1; }
#endif

    /* Type and Size checks */
    if (!S_ISREG(st.st_mode)) { close(fd); return -1; }
    if (st.st_size <= 0 || st.st_size > MAX_PLUGIN_SIZE) { close(fd); return -1; }

    /* Permission checks: no world-writable, no group-writable, no suid/sgid */
    if (st.st_mode & (S_IWOTH | S_IWGRP | S_ISUID | S_ISGID)) { close(fd); return -1; }

    /* Ownership check: must be owned by root (0) or the current process user */
    uid_t current_uid = getuid();
    if (st.st_uid != 0 && st.st_uid != current_uid) { close(fd); return -1; }

    /* Secure dlopen via FD path to avoid path traversal/symlink attacks */
    char fd_path[64];
#ifdef __linux__
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
#else
    snprintf(fd_path, sizeof(fd_path), "/dev/fd/%d", fd);
#endif

    void* handle = dlopen(fd_path, RTLD_NOW | RTLD_LOCAL);
    close(fd); /* FD can be closed after dlopen maps the object */
    if (!handle) return -1;

    jash_plugin_init_fn init;
    *(void **)(&init) = dlsym(handle, "jash_plugin_init");
    if (!init) {
        dlclose(handle);
        return -1;
    }

    jash_plugin_handle_t* plugin = init();
    if (!plugin) {
        dlclose(handle);
        return -1;
    }

    jash_plugin_shutdown_fn shutdown;
    *(void **)(&shutdown) = dlsym(handle, "jash_plugin_shutdown");

    /* Names are used as stable registry identities and by unload_all. Never
     * admit an unnamed plugin or an ambiguous duplicate into that registry. */
    int invalid = !plugin->info.name || !plugin->info.name[0] ||
                  strnlen(plugin->info.name, 64) == 64 ||
                  (plugin->info.version && strnlen(plugin->info.version, 256) == 256) ||
                  (plugin->info.description && strnlen(plugin->info.description, 4096) == 4096);
    for (int i = 0; !invalid && i < plugin_count; i++)
        if (!strcmp(plugins[i].plugin->info.name, plugin->info.name)) invalid = 1;
    if (invalid) {
        if (shutdown) shutdown(plugin);
        dlclose(handle); errno = EINVAL; return -1;
    }

    plugins[plugin_count].dl_handle = handle;
    plugins[plugin_count].plugin = plugin;
    plugins[plugin_count].shutdown = shutdown;
    memcpy(plugins[plugin_count].path, path, strlen(path) + 1u);
    plugins[plugin_count].device = st.st_dev;
    plugins[plugin_count].inode = st.st_ino;
    plugin_count++;

    return 0;
}

int jash_plugin_unload(const char* name) {
    for (int i = 0; i < plugin_count; i++) {
        if (plugins[i].plugin && plugins[i].plugin->info.name &&
            j_streq(plugins[i].plugin->info.name, name)) {
            if (plugins[i].shutdown) {
                plugins[i].shutdown(plugins[i].plugin);
            }
            int closed = dlclose(plugins[i].dl_handle);
            for (int j = i; j < plugin_count - 1; j++) {
                plugins[j] = plugins[j + 1];
            }
            plugin_count--;
            return closed == 0 ? 0 : -1;
        }
    }
    return -1;
}

void jash_plugin_unload_all(void) {
    while (plugin_count > 0) {
        jash_plugin_unload(plugins[0].plugin->info.name);
    }
}

int jash_plugin_count(void) { return plugin_count; }

const jash_plugin_info_t* jash_plugin_info_at(int index) {
    return index >= 0 && index < plugin_count ? &plugins[index].plugin->info : NULL;
}

const char* jash_plugin_path_at(int index) {
    return index >= 0 && index < plugin_count ? plugins[index].path : NULL;
}

int jash_plugin_list(jash_writer_t w, void* ctx) {
    if (plugin_count == 0) {
        jash_writes(w, ctx, "No plugins loaded.\n");
        return 0;
    }
    for (int i = 0; i < plugin_count; i++) {
        jash_plugin_info_t* info = &plugins[i].plugin->info;
        jash_writef(w, ctx, "%-16s %-8s %s\n",
            info->name ? info->name : "?",
            info->version ? info->version : "?",
            info->description ? info->description : "");
    }
    return 0;
}

const jash_builtin_t* jash_plugin_lookup_builtin(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < plugin_count; i++) {
        const jash_plugin_builtin_t* pb = plugins[i].plugin->builtins;
        if (!pb) continue;
        for (int j = 0; pb[j].name; j++) {
            if (j_streq(pb[j].name, name)) {
                static jash_builtin_t result;
                result.name = pb[j].name;
                result.fn = pb[j].fn;
                result.help = pb[j].help;
                return &result;
            }
        }
    }
    return 0;
}
