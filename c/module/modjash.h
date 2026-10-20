// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// ADR-0003 §M1: .modjash module format definition
//
// On-disk layout:
//   [HEADER 64 bytes][MANIFEST manifest_len bytes][BODY body_len bytes][SIGNATURE sig_len bytes]
//
// The body is a position-independent shared library (.dylib on macOS, .so on Linux).
// M1 loads it via dlopen. Future: custom mmap+reloc loader (~140µs target).

#ifndef JASH_MODULE_MODJASH_H
#define JASH_MODULE_MODJASH_H

#include <stdint.h>
#include <stddef.h>

/* ── On-disk header (64 bytes, fixed) ─────────────────────────────────── */

#define JMOD_MAGIC    "JMODv01"   /* 7 bytes + NUL */
#define JMOD_MAGIC_LEN 8
#define JMOD_ABI_VERSION 1

#define JMOD_FLAG_SIGNED   (1 << 0)  /* legacy body or envelope Ed25519 signature */
#define JMOD_FLAG_OPTIONAL (1 << 1)  /* jash boots without this module */
#define JMOD_FLAG_AUTOLOAD (1 << 2)  /* load on startup without explicit `module load` */
#define JMOD_FLAG_SIGNED_ENVELOPE (1 << 3) /* signs header+manifest+body digest; strict */

typedef struct __attribute__((packed)) {
    char     magic[JMOD_MAGIC_LEN];  /*  0: "JMODv01\0"                */
    uint32_t abi_version;            /*  8: must equal JMOD_ABI_VERSION */
    uint32_t flags;                  /* 12: JMOD_FLAG_*                 */
    uint8_t  body_blake3[32];        /* 16: BLAKE3 of body (zeros = unverified in M1) */
    uint32_t manifest_len;           /* 48: bytes of manifest section   */
    uint32_t body_len;               /* 52: bytes of body section       */
    uint32_t sig_len;                /* 56: bytes of signature (0 = none) */
    uint32_t _reserved;              /* 60: padding to 64 bytes         */
} jmod_header_t;                     /* total: 64 bytes                 */

/* ── Capability bitmap (caps_needed in manifest) ──────────────────────── */

typedef enum {
    JMOD_CAP_FS_READ        = 1 << 0,   /* read from filesystem              */
    JMOD_CAP_FS_WRITE       = 1 << 1,   /* write to filesystem               */
    JMOD_CAP_NET            = 1 << 2,   /* network access                    */
    JMOD_CAP_EXEC           = 1 << 3,   /* spawn subprocesses                */
    JMOD_CAP_STATE_READ     = 1 << 4,   /* read jash_state_t                 */
    JMOD_CAP_STATE_WRITE    = 1 << 5,   /* mutate jash_state_t               */
    JMOD_CAP_TTY            = 1 << 6,   /* direct tty/ansi access            */
    JMOD_CAP_CRYPTO         = 1 << 7,   /* use jash crypto primitives        */
    JMOD_CAP_MODULE_LOAD    = 1 << 8,   /* load other modules                */
    JMOD_CAP_HOOK_PRE_CMD   = 1 << 9,   /* register pre-command hook         */
    JMOD_CAP_HOOK_POST_CMD  = 1 << 10,  /* register post-command hook        */
    JMOD_CAP_HOOK_EXIT      = 1 << 11,  /* register shell-exit hook          */
    JMOD_CAP_HOOK_MUTATION  = 1 << 12,  /* receive mutation notifications    */
} jmod_cap_t;

/* ── Manifest (text section between header and body) ──────────────────── */
/*
 * Simple key=value format, one per line (no CBOR yet — that's S1).
 * Required keys:
 *   name=<string>
 *   version=<semver>
 *   caps=<hex uint32 of jmod_cap_t flags>
 * Optional:
 *   description=<string>
 *   provides_verbs=<comma-separated list of builtin verb names>
 *   provides_hooks=pre_cmd,post_cmd,exit
 *   min_abi=<uint>
 */

/* ── Runtime module ops (exported from body) ──────────────────────────── */
/*
 * The shared library body MUST export one symbol: `jmod_ops`
 * of type jmod_ops_t. All function pointers must be non-NULL except
 * where marked optional.
 */

struct jash_state;  /* forward — modules get the full header via <core/state.h> */
typedef struct jash_state jash_state_t;

/* Writer function type — same ABI as jash_writer_t in theme/io.h */
typedef long (*jmod_writer_t)(void* ctx, const char* buf, size_t n);

/* Context passed to every module dispatch call */
typedef struct {
    jash_state_t*  state;
    jmod_writer_t  writer;         /* output writer; pass to jash_writes/writef    */
    void*          writer_ctx;     /* opaque context for writer                    */
    const char*    verb;           /* the verb that triggered this dispatch        */
    const char*    args;           /* everything after the verb (trimmed)          */
} jmod_call_t;

typedef struct {
    /* Lifecycle */
    int  (*init)(void);                 /* called once after load — return 0 ok   */
    void (*shutdown)(void);             /* called on unload or shell exit          */

    /* Dispatch — called for every verb in provides_verbs */
    int  (*dispatch)(const jmod_call_t* call);  /* return exit code               */

    /* Hooks (NULL if not provided) */
    void (*hook_pre_cmd)(const jmod_call_t* call);
    void (*hook_post_cmd)(const jmod_call_t* call, int exit_code);
    void (*hook_exit)(int exit_code);

    /* Mutation notification: called when a builtin records an undoable
     * operation via jash_timeline_record().  kind = jash_undo_kind_t.
     * Only fired when at least one loaded module implements this hook.
     * No writer/output — modules record internally if needed. */
    void (*hook_mutation)(int kind, const char* desc,
                          const char* a, const char* b);

    /* Metadata (set by module, read by `module info`) */
    const char* name;
    const char* version;
    const char* description;
    uint32_t    caps_used;   /* actual caps at runtime — may be subset of declared */
} jmod_ops_t;

/* The one symbol a module body must export */
#define JMOD_OPS_SYMBOL "jmod_ops"

#endif /* JASH_MODULE_MODJASH_H */
