/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_OSIF_BETHOS_SURFACE_H
#define JASH_OSIF_BETHOS_SURFACE_H

#include <stddef.h>
#include <stdint.h>

#define JASH_BETHOS_SURFACE_CELL_BYTE UINT32_C(1)
#define JASH_BETHOS_SURFACE_CELL_PROMPT UINT32_C(6)
#define JASH_BETHOS_SURFACE_CELL_CONTROL UINT32_C(9)
#define JASH_BETHOS_SURFACE_CONTROL_CLEAR UINT32_C(1)

long jash_bethos_surface_emit(uint32_t cell_type, const void *data,
                              size_t length);
int jash_bethos_surface_clear(void);
int jash_bethos_surface_prompt(const char *text, size_t length);

static inline int jash_bethos_surface_active(void)
{
    return 1;
}

#endif
