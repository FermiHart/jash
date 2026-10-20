// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// ADR-0003 §M1: module loader public API

#ifndef JASH_MODULE_LOADER_H
#define JASH_MODULE_LOADER_H

#include <module/modjash.h>
#include <limits.h>
#include <stdint.h>

#define JMOD_MAX_MODULES 32
#define JMOD_PATH_CAP PATH_MAX
#define JMOD_PATH_DEFAULT "~/.jash/modules"

/* Loaded module record (one per loaded .modjash) */
typedef struct {
    char          name[64];
    char          path[JMOD_PATH_CAP];
    char          version[32];
    char          description[256];
    uint32_t      caps_declared;  /* from manifest — what module claimed it needs */
    uint32_t      caps_used;      /* from ops->caps_used — what code actually uses */
    jmod_ops_t*   ops;
    void*         dlhandle;       /* dlopen handle for body */
    int           exec_fd;        /* immutable image; retained until unload */
    int           active;         /* 1 = loaded and init'd */
} jmod_entry_t;

/* Module table (global, one per jash process) */
typedef struct {
    jmod_entry_t entries[JMOD_MAX_MODULES];
    int          count;
} jmod_table_t;

extern jmod_table_t g_modules;

/* Trusted Ed25519 public key for module signature verification (JMOD_FLAG_SIGNED).
 * Set at startup from ~/.jash/keys/module_signing_pubkey (32 bytes).
 * Zero-filled if not available — modules with JMOD_FLAG_SIGNED will be rejected. */
extern uint8_t jash_module_trusted_pubkey[32];

/* Pin HOME, loader policy, and the trusted key before mutable shell state runs. */
void jash_module_security_init(void);

/* Compatibility name for callers that only need to ensure initialization. */
void jash_module_trusted_pubkey_load(void);

/* ── API ───────────────────────────────────────────────────────────────── */

/* Load a .modjash file. path = absolute path to .modjash file.
 * Explicit unsigned/legacy loads remain trusted native code unless startup
 * hash/signature policy rejects them. Capability admission is not a sandbox.
 * Returns 0 on success, -1 on error (error text written to stderr). */
int jmod_load(const char* path, uint32_t granted_caps);

/* Automatic loaders additionally bind the installed path to its manifest
 * identity before native code is mapped or initialized. */
int jmod_load_expected(const char* path, uint32_t granted_caps,
                       const char* expected_name);

/* JINV-025 — a concessao de capacidades tem que poder NEGAR.
 *
 * O mecanismo sempre existiu e sempre funcionou: o manifesto declara
 * `caps_declared`, e jmod_load recusa se `caps_declared & ~granted_caps`. Mas
 * TODOS os chamadores passavam 0xFFFFFFFF, entao `missing` era sempre zero e o
 * portao nunca podia disparar. Codigo de seguranca real, incapaz de negar por
 * construcao — o mecanismo existia na camada de UX (o manifesto parecia
 * significar algo) e nao na de imposicao.
 *
 * Esta funcao devolve o que de fato sera concedido a `name`:
 *
 *   - por padrao, exatamente o que o modulo DECLAROU (nunca mais que isso);
 *   - menos o que o policy.conf lido do HOME inicial negar explicitamente,
 *     numa linha `nome: -net -exec`;
 *   - com JASH_MODULE_CAPS_STRICT=1 no startup, NADA e concedido a menos que a mesma
 *     linha conceda: `nome: +fs_read +crypto`.
 *
 * O padrao nao muda o comportamento de nenhum modulo existente — muda o que o
 * portao E CAPAZ de fazer, que era o defeito.
 * Gate: tests/test_jinv025_module_caps.sh */
uint32_t jmod_policy_grant(const char* name, uint32_t caps_declared);

/* Unload by name. Calls ops->shutdown(), dlclose. Returns 0 or -1. */
int jmod_unload(const char* name);

/* Dispatch a verb to the module that provides it.
 * Returns the module's exit code, or -1 if no module handles the verb. */
int jmod_dispatch(const char* verb, const jmod_call_t* call);

/* Run pre_cmd hooks on all loaded modules (in load order). */
void jmod_hook_pre_cmd(const jmod_call_t* call);

/* Run post_cmd hooks on all loaded modules (in load order). */
void jmod_hook_post_cmd(const jmod_call_t* call, int exit_code);

/* Run exit hooks on all loaded modules (reverse load order). */
void jmod_hook_exit(int exit_code);

/* Run mutation hooks on all loaded modules that register for it.
 * Fired from jash_timeline_record() after recording the inverse.
 * Safe to call even when no module is loaded (branch -> return). */
void jmod_hook_mutation(int kind, const char* desc,
                        const char* a, const char* b);

/* Find a loaded module by name. Returns NULL if not found. */
jmod_entry_t* jmod_find(const char* name);

/* Iterate active modules. fn is called for each (read-only view). */
void jmod_each(void (*fn)(const jmod_entry_t* e, void* udata), void* udata);

/* Parse a manifest text block into name/version/caps/description.
 * Writes into out_* pointers (caller provides buffers).
 * Returns 0 on success. */
int jmod_parse_manifest(const char* manifest, size_t manifest_len,
                        char* out_name, size_t name_cap,
                        char* out_version, size_t ver_cap,
                        char* out_description, size_t desc_cap,
                        uint32_t* out_caps);

/* Resolve a short module name to a .modjash file path.
 * Searches JMOD_PATH_DEFAULT (expands ~).
 * Returns 1 if found (path written to out_path), 0 if not. */
int jmod_resolve(const char* name, char* out_path, size_t out_cap);

/* Resolve only from the HOME pinned at process startup. Automatic loading must
 * use this entry point and never inspect a repository-controlled CWD. */
int jmod_resolve_installed(const char* name, char* out_path, size_t out_cap);

#endif /* JASH_MODULE_LOADER_H */
