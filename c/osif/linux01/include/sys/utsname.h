/* Author: F E R M I ∞ H A R T <contact@fermihart.com>
 * SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef LINUX01_UTSNAME_H
#define LINUX01_UTSNAME_H
#include "port.h"
struct utsname {
    char sysname[9], nodename[9], release[9], version[9], machine[9];
};
#endif
