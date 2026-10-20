// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BearJash — startup banner

#include "banner.h"
#include "colors.h"
#include <string.h>
#include <sys/utsname.h>

/* JASH logo — block unicode characters */
static const char* const JASH_LOGO[] = {
    "",
    "    ██╗ █████╗ ███████╗██╗  ██╗",
    "    ██║██╔══██╗██╔════╝██║  ██║",
    "    ██║███████║███████╗███████║",
    "██  ██║██╔══██║╚════██║██╔══██║",
    "█████╔╝██║  ██║███████║██║  ██║",
    "╚════╝ ╚═╝  ╚═╝╚══════╝╚═╝  ╚═╝",
    "",
    0
};

static const char* get_os_name(void) {
    static struct utsname u;
    if (uname(&u) == 0) return u.sysname;
    return "Unknown";
}

static const char* get_arch(void) {
    static struct utsname u;
    if (uname(&u) == 0) return u.machine;
    return "";
}

void jash_banner_short(jash_writer_t w, void* ctx) {
    jash_writef(w, ctx,
        "%sjash %s%s%s %s%s%s\n",
        J_ROLE_PROMPT, J_BOLD, JASH_VERSION, J_RESET,
        J_ROLE_ACCENT, JASH_CODENAME, J_RESET);
}

void jash_banner_render(jash_writer_t w, void* ctx, long pid) {
    if (!w) return;

    jash_writes(w, ctx, "\n");

    /* Logo */
    jash_writes(w, ctx, J_ROLE_BANNER J_BOLD);
    for (int i = 0; JASH_LOGO[i]; i++) {
        jash_writes(w, ctx, JASH_LOGO[i]);
        jash_writes(w, ctx, "\n");
    }
    jash_writes(w, ctx, J_RESET);

    /* 0.1α Subsumption · Darwin x86_64 */
    jash_writef(w, ctx,
        "  %s%s%s%s %sα%s %s%s%s  %s·%s  %s%s%s %s%s%s\n",
        J_ROLE_BANNER, J_BOLD, JASH_VERSION, J_RESET,
        J_ROLE_ACCENT, J_RESET,
        J_ROLE_OK, JASH_CODENAME, J_RESET,
        J_ROLE_MUTED, J_RESET,
        J_ROLE_PROMPT, get_os_name(), J_RESET,
        J_ROLE_MUTED, get_arch(), J_RESET);

    /* Separator */
    jash_writef(w, ctx, "  %s", J_ROLE_MUTED);
    for (int i = 0; i < 38; i++) jash_writes(w, ctx, "─");
    jash_writef(w, ctx, "%s\n", J_RESET);

    /* Type help for commands  Pid 12345 */
    jash_writef(w, ctx,
        "  %sType%s %shelp%s %sfor commands%s  %sPid %s%ld%s\n",
        J_ROLE_MUTED, J_RESET,
        J_BOLD, J_RESET,
        J_ROLE_MUTED, J_RESET,
        J_ROLE_MUTED, J_RESET, pid, J_RESET);

    jash_writes(w, ctx, "\n");
}
