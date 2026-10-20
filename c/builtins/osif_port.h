// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_OSIF_PORT_H
#define JASH_BUILTINS_OSIF_PORT_H
#include "osif_publish.h"

/* The existing scaffolder writes exactly these three files. Its CMake
 * fragment is documentation inside AGENT.md, never an automatic edit. */
typedef struct {
    char target[33];
    char syscall_body[2048];
    char osinfo_body[3072];
    char agent_body[4096];
    size_t lengths[3];
} jash_osif_port_plan_t;
int jash_osif_port_prepare(const char* target, jash_osif_port_plan_t* plan);
int jash_osif_port_apply(const jash_osif_port_plan_t* plan,
                         osif_publish_observer_t observer, void* context,
                         osif_publish_store_t* result);
#endif
