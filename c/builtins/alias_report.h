// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_ALIAS_REPORT_H
#define JASH_BUILTINS_ALIAS_REPORT_H
#include "discovery_output.h"
int jash_alias_result_row(jash_discovery_rows_t*, const char* name, const char* operation,
                          const char* before, const char* value, int changed, const char* error);
int jash_alias_report_emit(jash_writer_t, void*, const char* command, int mode,
                           jash_discovery_rows_t*, int status);
#endif
