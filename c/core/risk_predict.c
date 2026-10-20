/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#include "risk_predict.h"
#include <string.h>
#include <stdlib.h>

static const char* HIGH_RISK_PATTERNS[] = {
    "rm -rf /",
    "rm -rf /*",
    ":(){ :|:& };:",
    "chmod -R 777",
    "chown -R",
    "> /dev/sda",
    "mkfs.",
    "dd if=",
    NULL
};

int jash_risk_assess(const char* cmd) {
    if (!cmd) return 0;
    for (int i = 0; HIGH_RISK_PATTERNS[i]; i++) {
        if (strstr(cmd, HIGH_RISK_PATTERNS[i])) {
            return 100;
        }
    }
    if (strstr(cmd, "rm -rf")) return 30;
    if (strstr(cmd, "sudo")) return 20;
    return 0;
}
