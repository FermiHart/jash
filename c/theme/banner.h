// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: theme/banner — startup banner.
//
// The banner is the first thing the user sees. It must:
//   - identify jash (and Josh, and the codename)
//   - credit F E R M I ∞ H A R T
//   - invite the user to hear the music (license-aligned)
//   - look good in TTY ≥ 80 columns
//   - be theme-aware (use J_ROLE_* roles, not raw colors)
//
// Sacred-geometry codename for jash 0.1 = "Subsumption" (port phase).

#ifndef JASH_THEME_BANNER_H
#define JASH_THEME_BANNER_H

#include "io.h"

/* Render the full banner. Pass a writer that targets stdout (Josh:
 * sys_write; standalone: write()) or a test-time string collector. */
void jash_banner_render(jash_writer_t w, void* ctx, long pid);

/* Render a short one-line build-id banner — useful for scripts and
 * non-interactive launches. */
void jash_banner_short(jash_writer_t w, void* ctx);

/* Build identifier — single source of truth for codename + version.
 * jash is "0.1 forever" per manifesto §0.5; codename moves over time. */
extern const char* const JASH_VERSION;     /* "0.1" */
extern const char* const JASH_CODENAME;    /* "Subsumption" */
extern const char* const JASH_AUTHOR;      /* "F E R M I ∞ H A R T" */
extern const char* const JASH_AUTHOR_MAIL; /* "contact@fermihart.com" */
extern const char* const JASH_MUSIC_URL;   /* "https://fermihart.com/music" */

#endif /* JASH_THEME_BANNER_H */
