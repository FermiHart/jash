/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_TINA_SURFACE_H
#define JASH_TINA_SURFACE_H
#include <stddef.h>

/* TinaLinux-specific surface transport. Ordinary Linux keeps its TTY path. */
int jash_tina_surface_probe(void);
int jash_tina_surface_native_input(void);
int jash_tina_surface_native_output(void);
int jash_tina_surface_editor_begin(void);
int jash_tina_surface_editor_end(void);
int jash_tina_surface_foreground(int pgid);
int jash_tina_surface_foreground_owner(void);
int jash_tina_surface_viewport(unsigned* columns, unsigned* rows);
void jash_tina_surface_error(const char* data, size_t len);
void jash_tina_surface_progress(unsigned current, unsigned total, const char* label);
#endif
