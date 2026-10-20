/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   osif/singuos/syscall.c — SinguOS L0 backend via blibc bear_*
 *
 *   The SinguOS jash process links against the SinguOS blibc port — exactly
 *   the same bear_* syscall translation contract as the beyondbsd backend.
 *   To keep ONE translation table (never two copies of the Linux-NR map),
 *   this port includes the beyondbsd implementation verbatim. If the SinguOS
 *   kernel ABI ever diverges from blibc, it diverges there first.
 *
 *   See AGENT.md for the port governance (waves J1–J4).
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/

#include "../beyondbsd/syscall.c"
