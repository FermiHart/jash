// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/unix_format — Unix-standard formatting utilities
//
// Features:
//   - Column-based output (like GNU ls)
//   - LS_COLORS standard colors
//   - Unicode icons for file types
//   - Terminal width detection
//   - Sensory mode (accessibility: no colors, no unicode)

#ifndef JASH_UNIX_FORMAT_H
#define JASH_UNIX_FORMAT_H

#include <sys/stat.h>

// Terminal width detection
int unix_get_terminal_width(void);

// LS_COLORS standard colors
// Directories: blue (34)
// Executables: green (32)
// Symlinks: cyan (36)
// Archives: red (31)
// Images: magenta (35)
// Audio: cyan (36)
// Videos: magenta (35)
const char* unix_get_file_color(const char* name, mode_t mode);

// Unicode icons
// 📁 directory, 📄 file, ⚙️ executable, 🔗 symlink, 📦 archive, 🖼️ image, 🎵 audio, 🎬 video
const char* unix_get_file_icon(const char* name, mode_t mode);

// Column formatting
// Formats array of strings into columns that fit terminal width
// Returns allocated string with column-formatted output
char* unix_format_columns(const char** items, int count, int item_widths[]);

// Sensory mode check
int unix_is_sensory_mode(void);

#endif // JASH_UNIX_FORMAT_H
