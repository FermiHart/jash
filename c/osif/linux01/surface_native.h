/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0
 * Linux01 adapter of c/osif/tina/surface.h. */
#ifndef JASH_LINUX01_SURFACE_NATIVE_H
#define JASH_LINUX01_SURFACE_NATIVE_H
#include <stddef.h>
int jash_linux01_surface_attach(void);
int jash_linux01_surface_probe(void);
int jash_linux01_surface_native_input(void);
int jash_linux01_surface_native_output(void);
int jash_linux01_surface_editor_begin(void);
int jash_linux01_surface_editor_end(void);
int jash_linux01_surface_foreground(int pgid);
int jash_linux01_surface_foreground_owner(void);
int jash_linux01_surface_viewport(unsigned *columns,unsigned *rows);
void jash_linux01_surface_error(const char *data,size_t len);
void jash_linux01_surface_progress(unsigned current,unsigned total,const char *label);
void jash_linux01_surface_receipt(unsigned index,int status);
#endif
