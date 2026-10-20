// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Module: readline/balance — detect incomplete input constructs.
#ifndef JASH_READLINE_BALANCE_H
#define JASH_READLINE_BALANCE_H

int jash_line_is_complete(const char *line, int len);

#endif
