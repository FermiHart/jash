// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/unix_format — Unix-standard formatting utilities

#include "unix_format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#ifdef JASH_TINALINUX
#include <osif/tina/surface.h>
#endif
#include <unistd.h>
#include <fnmatch.h>
#include <core/env.h>

/* ── Terminal width detection ─────────────────────────────────────────────── */

int unix_get_terminal_width(void) {
#ifdef JASH_TINALINUX
    if (jash_tina_surface_native_output()) {
        unsigned columns = 0;
        return jash_tina_surface_viewport(&columns, NULL) == 0 ? (int)columns : 80;
    }
#endif
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0 && w.ws_col > 0) {
        return w.ws_col;
    }
    /* Fallback: check COLUMNS env var */
    const char* cols = getenv("COLUMNS");
    if (cols) {
        int c = atoi(cols);
        if (c > 0) return c;
    }
    /* Default */
    return 80;
}

/* ── Sensory mode ─────────────────────────────────────────────────────────── */

int unix_is_sensory_mode(void) {
    /* Check JASH_SENSORY env var or theme setting */
    const char* sensory = getenv("JASH_SENSORY");
    if (sensory && (strcmp(sensory, "1") == 0 || strcasecmp(sensory, "on") == 0)) {
        return 1;
    }
    /* Check NO_COLOR env var (standard) */
    if (getenv("NO_COLOR")) {
        return 1;
    }
    return 0;
}

/* ── LS_COLORS standard colors ────────────────────────────────────────────── */

/* File type patterns for coloring */
static const struct {
    const char* pattern;
    const char* color;
} file_colors[] = {
    /* Archives */
    {"*.tar", "\033[31m"},      /* red */
    {"*.tgz", "\033[31m"},
    {"*.arc", "\033[31m"},
    {"*.arj", "\033[31m"},
    {"*.taz", "\033[31m"},
    {"*.lha", "\033[31m"},
    {"*.lz4", "\033[31m"},
    {"*.lzh", "\033[31m"},
    {"*.lzma", "\033[31m"},
    {"*.tlz", "\033[31m"},
    {"*.txz", "\033[31m"},
    {"*.tzo", "\033[31m"},
    {"*.t7z", "\033[31m"},
    {"*.zip", "\033[31m"},
    {"*.z", "\033[31m"},
    {"*.dz", "\033[31m"},
    {"*.gz", "\033[31m"},
    {"*.lrz", "\033[31m"},
    {"*.lz", "\033[31m"},
    {"*.lzo", "\033[31m"},
    {"*.xz", "\033[31m"},
    {"*.zst", "\033[31m"},
    {"*.tzst", "\033[31m"},
    {"*.bz2", "\033[31m"},
    {"*.bz", "\033[31m"},
    {"*.tbz", "\033[31m"},
    {"*.tbz2", "\033[31m"},
    {"*.tz", "\033[31m"},
    {"*.deb", "\033[31m"},
    {"*.rpm", "\033[31m"},
    {"*.jar", "\033[31m"},
    {"*.war", "\033[31m"},
    {"*.ear", "\033[31m"},
    {"*.sar", "\033[31m"},
    {"*.rar", "\033[31m"},
    {"*.alz", "\033[31m"},
    {"*.ace", "\033[31m"},
    {"*.zoo", "\033[31m"},
    {"*.cpio", "\033[31m"},
    {"*.7z", "\033[31m"},
    {"*.rz", "\033[31m"},
    {"*.cab", "\033[31m"},
    {"*.wim", "\033[31m"},
    {"*.swm", "\033[31m"},
    {"*.dwm", "\033[31m"},
    {"*.esd", "\033[31m"},
    /* Images */
    {"*.jpg", "\033[35m"},      /* magenta */
    {"*.jpeg", "\033[35m"},
    {"*.mjpg", "\033[35m"},
    {"*.mjpeg", "\033[35m"},
    {"*.gif", "\033[35m"},
    {"*.bmp", "\033[35m"},
    {"*.pbm", "\033[35m"},
    {"*.pgm", "\033[35m"},
    {"*.ppm", "\033[35m"},
    {"*.tga", "\033[35m"},
    {"*.xbm", "\033[35m"},
    {"*.xpm", "\033[35m"},
    {"*.tif", "\033[35m"},
    {"*.tiff", "\033[35m"},
    {"*.png", "\033[35m"},
    {"*.svg", "\033[35m"},
    {"*.svgz", "\033[35m"},
    {"*.mng", "\033[35m"},
    {"*.pcx", "\033[35m"},
    {"*.mov", "\033[35m"},
    {"*.mpg", "\033[35m"},
    {"*.mpeg", "\033[35m"},
    {"*.m2v", "\033[35m"},
    {"*.mkv", "\033[35m"},
    {"*.webm", "\033[35m"},
    {"*.webp", "\033[35m"},
    {"*.ogm", "\033[35m"},
    {"*.mp4", "\033[35m"},
    {"*.m4v", "\033[35m"},
    {"*.mp4v", "\033[35m"},
    {"*.vob", "\033[35m"},
    {"*.qt", "\033[35m"},
    {"*.nuv", "\033[35m"},
    {"*.wmv", "\033[35m"},
    {"*.asf", "\033[35m"},
    {"*.rm", "\033[35m"},
    {"*.rmvb", "\033[35m"},
    {"*.flc", "\033[35m"},
    {"*.avi", "\033[35m"},
    {"*.fli", "\033[35m"},
    {"*.flv", "\033[35m"},
    {"*.gl", "\033[35m"},
    {"*.dl", "\033[35m"},
    {"*.xcf", "\033[35m"},
    {"*.xwd", "\033[35m"},
    {"*.yuv", "\033[35m"},
    {"*.cgm", "\033[35m"},
    {"*.emf", "\033[35m"},
    {"*.ogv", "\033[35m"},
    {"*.ogx", "\033[35m"},
    /* Audio */
    {"*.aac", "\033[36m"},      /* cyan */
    {"*.au", "\033[36m"},
    {"*.flac", "\033[36m"},
    {"*.m4a", "\033[36m"},
    {"*.mid", "\033[36m"},
    {"*.midi", "\033[36m"},
    {"*.mka", "\033[36m"},
    {"*.mp3", "\033[36m"},
    {"*.mpc", "\033[36m"},
    {"*.ogg", "\033[36m"},
    {"*.ra", "\033[36m"},
    {"*.wav", "\033[36m"},
    {"*.oga", "\033[36m"},
    {"*.opus", "\033[36m"},
    {"*.spx", "\033[36m"},
    {"*.xspf", "\033[36m"},
    {NULL, NULL}
};

const char* unix_get_file_color(const char* name, mode_t mode) {
    if (unix_is_sensory_mode()) {
        return "\033[0m";  /* reset */
    }
    
    /* Directory: blue */
    if (S_ISDIR(mode)) {
        return "\033[01;34m";
    }
    
    /* Symlink: cyan */
    if (S_ISLNK(mode)) {
        return "\033[01;36m";
    }
    
    /* Executable: green */
    if (S_ISREG(mode) && (mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        return "\033[01;32m";
    }
    
    /* Check file extension patterns */
    for (int i = 0; file_colors[i].pattern; i++) {
        if (fnmatch(file_colors[i].pattern, name, 0) == 0) {
            return file_colors[i].color;
        }
    }
    
    /* Default: no color */
    return "\033[0m";
}

/* ── Unicode icons ────────────────────────────────────────────────────────── */

const char* unix_get_file_icon(const char* name, mode_t mode) {
    if (unix_is_sensory_mode()) {
        return "";  /* no icons in sensory mode */
    }
    
    /* Directory */
    if (S_ISDIR(mode)) {
        return "📁 ";
    }
    
    /* Symlink */
    if (S_ISLNK(mode)) {
        return "🔗 ";
    }
    
    /* Executable */
    if (S_ISREG(mode) && (mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        return "⚙️  ";
    }
    
    /* Check for specific types by extension */
    const char* ext = strrchr(name, '.');
    if (ext) {
        if (strcasecmp(ext, ".pdf") == 0) return "📕 ";
        if (strcasecmp(ext, ".md") == 0) return "📝 ";
        if (strcasecmp(ext, ".txt") == 0) return "📄 ";
        if (strcasecmp(ext, ".c") == 0 || strcasecmp(ext, ".h") == 0) return "💻 ";
        if (strcasecmp(ext, ".py") == 0) return "🐍 ";
        if (strcasecmp(ext, ".js") == 0 || strcasecmp(ext, ".ts") == 0) return "📜 ";
        if (strcasecmp(ext, ".go") == 0) return "🔵 ";
        if (strcasecmp(ext, ".rs") == 0) return "🦀 ";
    }
    
    /* Default: regular file */
    return "📄 ";
}

/* ── Column formatting ────────────────────────────────────────────────────── */

/* Calculate display width of string (accounting for unicode and ANSI escapes) */
static int display_width(const char* s) {
    int width = 0;
    while (*s) {
        /* Skip ANSI escape sequences: ESC [ ... m (or other final letter) */
        if ((unsigned char)*s == 0x1B && s[1] == '[') {
            s += 2;
            while (*s && !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))) s++;
            if (*s) s++;
            continue;
        }
        if ((unsigned char)*s < 0x80) {
            width++;
            s++;
        } else if ((unsigned char)*s < 0xC0) {
            s++;  /* continuation byte */
        } else if ((unsigned char)*s < 0xE0) {
            width++;  /* 2-byte char, assume width 1 */
            s += 2;
        } else if ((unsigned char)*s < 0xF0) {
            width++;  /* 3-byte char, assume width 1 */
            s += 3;
        } else {
            width += 2;  /* 4-byte char (emoji), assume width 2 */
            s += 4;
        }
    }
    return width;
}

char* unix_format_columns(const char** items, int count, int item_widths[]) {
    if (count == 0) {
        char* result = malloc(1);
        if (result) result[0] = '\0';
        return result;
    }
    
    int term_width = unix_get_terminal_width();
    
    /* Find maximum item width */
    int max_width = 0;
    for (int i = 0; i < count; i++) {
        int w = item_widths ? item_widths[i] : display_width(items[i]);
        if (w > max_width) max_width = w;
    }
    
    /* Add padding */
    int col_width = max_width + 2;
    int cols = term_width / col_width;
    if (cols < 1) cols = 1;
    
    int rows = (count + cols - 1) / cols;
    
    /* Allocate output buffer */
    size_t buf_size = count * (col_width + 10) + rows * 2 + 1;
    char* output = malloc(buf_size);
    if (!output) return NULL;
    
    output[0] = '\0';
    size_t pos = 0;
    
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            int idx = row + col * rows;
            if (idx >= count) break;
            
            int w = item_widths ? item_widths[idx] : display_width(items[idx]);
            int written = (int)snprintf(output + pos, buf_size - pos, "%s", items[idx]);
            if (written < 0) {
                pos = buf_size - 1;
            } else if (written >= (int)(buf_size - pos)) {
                pos = buf_size - 1;
            } else {
                pos += written;
            }
            
            /* Pad to column width (except last column) */
            if (col < cols - 1 && idx + rows < count) {
                int padding = col_width - w;
                for (int p = 0; p < padding && pos < buf_size - 1; p++) {
                    output[pos++] = ' ';
                }
            }
        }
        if (pos < buf_size - 1) {
            output[pos++] = '\n';
        }
    }
    
    output[pos] = '\0';
    return output;
}
