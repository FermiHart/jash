// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_RECOVERY_H
#define JASH_CORE_RECOVERY_H
#include <stdint.h>
#include <theme/io.h>

typedef struct {
    char token[33];
    uint64_t entry_id;
    const char* coverage;
} jash_recovery_ref_t;

/* Persist a guarded inverse before exporting a handle. Failure leaves token
 * empty. This is an owner-controlled local store, not a bearer authorization. */
int jash_recovery_capture(uint64_t id, int kind, const char* path, const char* data,
                           jash_recovery_ref_t* out);
int jash_recovery_canonical(const char* path, char* out, size_t capacity);
int jash_recovery_command(jash_writer_t writer, void* context, const char* args);
typedef struct jash_recovery_pins jash_recovery_pins_t;
/* A bounded, conservative pin snapshot holds the admission lock until free.
 * NULL or incomplete catalogs protect every candidate (fail closed). */
jash_recovery_pins_t* jash_recovery_pins_load(void);
/* Same catalog, bound to an explicit storage home rather than mutable shell HOME. */
jash_recovery_pins_t* jash_recovery_pins_load_home(const char* home);
int jash_recovery_pins_contains(const jash_recovery_pins_t*, const char* path);
int jash_recovery_pins_complete(const jash_recovery_pins_t*);
void jash_recovery_pins_free(jash_recovery_pins_t*);
#endif
