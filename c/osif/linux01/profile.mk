# Author: F E R M I ∞ H A R T <contact@fermihart.com>
# SPDX-License-Identifier: LicenseRef-SPL-1.0
# All Jash changes for this profile live under c/osif/linux01/.
JASH_CANONICAL_UNITS := readline/edit readline/keymap readline/render \
    theme/io theme/prompt theme/theme core/history core/autocomplete \
    core/predict core/argv theme/banner core/version builtins/version
BLIBC_UNITS := core/mem/memcpy core/mem/memmove core/mem/memset core/mem/memcmp \
    core/mem/memchr core/str/strlen core/str/strnlen core/str/strcmp core/str/strncmp \
    core/str/strchr core/str/strrchr core/str/strncpy core/str/strlcpy \
    core/str/strlcat core/str/str_more io/fmt/fmt
