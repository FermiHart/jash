// SPDX-License-Identifier: LicenseRef-SPL-1.0
// The first metadata line remains the legacy directory. An optional explicit
// basename resolves numeric-prefix ambiguity without changing that boundary.
#define TRASH_NAME_MARKER "JASH-NAME-HEX:"
typedef struct {
    char directory[512], name[256];
    const char* source;
    const char* error;
} trash_metadata_t;

static int trash_hex_nibble(unsigned char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

static int trash_metadata_read(int parent, const char* entry, trash_metadata_t* result) {
    memset(result, 0, sizeof *result);
    result->source = "unavailable";
    char name[512], bytes[1537];
    int length = snprintf(name, sizeof name, "%s.meta", entry);
    if (length < 0 || (size_t)length >= sizeof name) { result->error = "metadata_path_limit"; return -1; }
    int fd = openat(parent, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { result->error = errno == ENOENT ? "metadata_missing" : "metadata_unavailable"; return -1; }
    struct stat state;
    size_t used = 0;
    if (fstat(fd, &state) || !S_ISREG(state.st_mode) || state.st_uid != geteuid() ||
        (state.st_mode & 0022) || state.st_nlink != 1 || state.st_size < 1 || state.st_size > 1536)
        result->error = "invalid_metadata";
    while (!result->error && used < sizeof bytes - 1u) {
        ssize_t count = read(fd, bytes + used, sizeof bytes - 1u - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result->error = "metadata_read_failed"; break; }
        if (!count) break;
        used += (size_t)count;
    }
    if (!result->error && (used != (size_t)state.st_size || memchr(bytes, 0, used))) result->error = "invalid_metadata";
    if (close(fd) && !result->error) result->error = "metadata_close_failed";
    if (result->error) return -1;
    bytes[used] = 0;
    char* newline = memchr(bytes, '\n', used);
    if (!newline || (size_t)(newline - bytes) >= sizeof result->directory || memchr(bytes, '\r', (size_t)(newline - bytes))) {
        result->error = "invalid_metadata"; return -1;
    }
    size_t directory_size = (size_t)(newline - bytes);
    memcpy(result->directory, bytes, directory_size); result->directory[directory_size] = 0;
    const char* extra = newline + 1;
    if (*extra) {
        const size_t marker_size = sizeof TRASH_NAME_MARKER - 1u;
        size_t remaining = used - (size_t)(extra - bytes);
        if (remaining <= marker_size || strncmp(extra, TRASH_NAME_MARKER, marker_size) || extra[remaining - 1u] != '\n') {
            result->error = "invalid_metadata"; return -1;
        }
        size_t hex_size = remaining - marker_size - 1u;
        if (!hex_size || (hex_size & 1u) || hex_size / 2u >= sizeof result->name) {
            result->error = "invalid_metadata"; return -1;
        }
        for (size_t i = 0; i < hex_size / 2u; i++) {
            int high = trash_hex_nibble((unsigned char)extra[marker_size + 2u * i]);
            int low = trash_hex_nibble((unsigned char)extra[marker_size + 2u * i + 1u]);
            if (high < 0 || low < 0 || !(high | low) || (high * 16 + low) == '/') {
                result->error = "invalid_metadata"; return -1;
            }
            result->name[i] = (char)(high * 16 + low);
        }
        result->name[hex_size / 2u] = 0;
        result->source = "metadata";
    } else {
        const char* original = entry;
        const char* first = strchr(original, '_');
        int numeric = first && first != original;
        for (const char* p = original; numeric && p < first; p++) if (*p < '0' || *p > '9') numeric = 0;
        if (numeric) original = first + 1;
        const char* next = strchr(original, '_');
        numeric = next && next != original;
        for (const char* p = original; numeric && p < next; p++) if (*p < '0' || *p > '9') numeric = 0;
        if (numeric) { result->error = "ambiguous_legacy_name"; return -1; }
        if (!*original || strlen(original) >= sizeof result->name) { result->error = "invalid_metadata"; return -1; }
        memcpy(result->name, original, strlen(original) + 1u);
        result->source = "legacy_single_prefix";
    }
    if (!strcmp(result->name, ".") || !strcmp(result->name, "..")) { result->error = "invalid_metadata"; return -1; }
    return 0;
}
