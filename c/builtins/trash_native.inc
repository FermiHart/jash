// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Semantic adapters for the existing private trash and transactional engines.
#define TRASH_NATIVE_MAX_ENTRIES 4094u

static int trash_native_private(int parent, const char* name, int create) {
    if (create) {
        mode_t mask = umask(0);
        int made = mkdirat(parent, name, 0700), saved = errno;
        umask(mask); errno = saved;
        if (made && errno != EEXIST) return -1;
        if (!made && fsync(parent)) return -1;
    }
    int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat state;
    if (fd >= 0 && (fstat(fd, &state) || state.st_uid != geteuid() || (state.st_mode & 0022))) {
        close(fd); errno = EPERM; return -1;
    }
    return fd;
}

static int trash_native_open(int create) {
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') { errno = EINVAL; return -1; }
    int root = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (root < 0) return -1;
    int jash = trash_native_private(root, ".jash", create), saved = errno;
    if (close(root) && jash >= 0) { close(jash); return -1; }
    errno = saved;
    if (jash < 0) return -1;
    int trash = trash_native_private(jash, "trash", create); saved = errno;
    if (close(jash) && trash >= 0) { close(trash); return -1; }
    errno = saved; return trash;
}

static int trash_native_entry(int parent, const char* name) {
    if (name[0] == '.') return 0;
    size_t size = strlen(name);
    if (size < 5 || strcmp(name + size - 5u, ".meta")) return 1;
    char metadata[512];
    int n = snprintf(metadata, sizeof metadata, "%s.meta", name);
    struct stat state;
    return n > 0 && (size_t)n < sizeof metadata && !fstatat(parent, metadata, &state, AT_SYMLINK_NOFOLLOW);
}

static const char* trash_native_kind(mode_t mode) {
    return S_ISREG(mode) ? "regular" : S_ISDIR(mode) ? "directory" : S_ISLNK(mode) ? "symlink" : "other";
}

static int trash_native_action(jash_discovery_rows_t* rows, const char* operation,
                                const char* source, const char* destination, const char* entry,
                                int dry, int applied, int durable, const char* error, int error_number) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "action")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("source"), jash_discovery_path(rows, source)},
        {JDV_LITERAL("destination"), jash_discovery_path(rows, destination)},
        {JDV_LITERAL("entry"), jash_discovery_path(rows, entry)},
        {JDV_LITERAL("dry_run"), jdv_value_bool(dry)},
        {JDV_LITERAL("applied"), applied < 0 ? jdv_value_nothing() : jdv_value_bool(applied)},
        {JDV_LITERAL("durable"), jdv_value_bool(durable)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
        {JDV_LITERAL("errno"), jdv_value_u64((uint64_t)error_number)}
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

static int trash_native_summary(jash_discovery_rows_t* rows, const char* operation,
                                 uint64_t entries, uint64_t bytes, uint64_t completed,
                                 int scan_complete, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "summary")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("entries"), jdv_value_u64(entries)},
        {JDV_LITERAL("bytes"), jdv_value_filesize(bytes)},
        {JDV_LITERAL("completed"), jdv_value_u64(completed)},
        {JDV_LITERAL("scan_complete"), jdv_value_bool(scan_complete)},
        {JDV_LITERAL("consistency"), jash_discovery_string(rows, "live")},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)}
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

static int trash_native_finish(jash_discovery_rows_t* rows, jash_writer_t writer, void* context, int mode, int status) {
    int rc = mode == JASH_DISCOVERY_DATA
        ? jash_discovery_emit_data_outcome(writer, context, "trash", rows, status,
              status == 75 ? JDV_STREAM_INCOMPLETE : status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE,
              status == 75 ? "entry_limit" : status ? "operation_failed" : NULL)
        : jash_discovery_emit_schema(writer, context, "trash", mode, rows, status, "jash.trash.v1");
    jash_discovery_rows_free(rows); return rc;
}

static int trash_native_policy(jash_writer_t writer, void* context, int mode, const char* operation) {
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "trash", mode, "allocation_failed", 125);
    int changed = strcmp(operation, "status") != 0;
    int before = jash_trash_protected(), source_before = jash_trash_source();
    if (changed) {
        int rc = jash_discovery_effects_begin(&rows, writer, context, "trash", mode, "jash.trash.v1");
        if (rc) { jash_discovery_rows_free(&rows); return rc; }
        jash_trash_set(!strcmp(operation, "on") || !strcmp(operation, "protect") ? 1 : !strcmp(operation, "off") ? 0 : -1);
    }
    int source = jash_trash_source(), protected = jash_trash_protected();
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(&rows, "policy")},
        {JDV_LITERAL("operation"), jash_discovery_string(&rows, operation)},
        {JDV_LITERAL("protected"), jdv_value_bool(protected)},
        {JDV_LITERAL("source"), jash_discovery_string(&rows, source == 0 ? "explicit" : source == 1 ? "environment" : "recording_default")},
        {JDV_LITERAL("changed"), jdv_value_bool(changed && (before != protected || source_before != source))}
    };
    (void)jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0]);
    return trash_native_finish(&rows, writer, context, mode, 0);
}

static int trash_native_scan(jash_writer_t writer, void* context, int mode, const char* operation,
                              int dry, int all, int argc, char** argv) {
    char trash_path[512];
    if (trash_get_dir(trash_path, sizeof trash_path)) return jash_discovery_error(writer, context, "trash", mode, "trash_path_unavailable", 1);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "trash", mode, "allocation_failed", 125);
    int restore = !strcmp(operation, "restore"), info = !strcmp(operation, "info");
    int rc = 0, complete = 1;
    uint64_t count = 0, bytes = 0, matched = 0, completed = 0;
    const char* source_error = NULL;
    if (restore && !dry) {
        rc = jash_discovery_effects_begin(&rows, writer, context, "trash", mode, "jash.trash.v1");
        if (rc) { jash_discovery_rows_free(&rows); return rc; }
    }
    int fd = trash_native_open(0);
    DIR* directory = fd >= 0 ? fdopendir(fd) : NULL;
    if (!directory) {
        if (fd >= 0) close(fd);
        if (errno != ENOENT) { source_error = "trash_unavailable"; rc = 1; complete = 0; }
        goto finish;
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) { source_error = "directory_read_failed"; rc = 1; complete = 0; } break; }
        if (!trash_native_entry(fd, entry->d_name)) continue;
        if (count == TRASH_NATIVE_MAX_ENTRIES) { source_error = "entry_limit"; rc = 75; complete = 0; break; }
        count++;
        char path[1024], original[1024];
        (void)snprintf(path, sizeof path, "%s/%s", trash_path, entry->d_name);
        trash_metadata_t metadata;
        int meta = trash_metadata_read(fd, entry->d_name, &metadata);
        original[0] = 0;
        if (!meta) {
            int length = snprintf(original, sizeof original, "%s%s%s", metadata.directory,
                metadata.directory[0] ? "/" : "", metadata.name);
            if (length < 0 || (size_t)length >= sizeof original) { metadata.error = "original_path_limit"; meta = -1; original[0] = 0; }
        }
        struct stat state;
        int observed = !fstatat(fd, entry->d_name, &state, AT_SYMLINK_NOFOLLOW);
        const char* error = !observed ? "entry_stat_failed" : meta ? metadata.error : NULL;
        if (observed && state.st_size >= 0) {
            if ((uint64_t)state.st_size > UINT64_MAX - bytes) { source_error = "size_overflow"; rc = 75; complete = 0; break; }
            bytes += (uint64_t)state.st_size;
        }
        if (restore) {
            int selected = all;
            if (!meta) for (int i = 0; !selected && i < argc; i++)
                if (!strcmp(argv[i], strchr(argv[i], '/') ? original : metadata.name)) selected = 1;
            if (meta && !all) continue;
            if (!selected) continue;
            matched++;
            int applied = 0, durable = 0, error_number = 0;
            if (!error && !dry) {
                int destination = trash_open_restore_dir(metadata.directory);
                if (destination < 0) { error = "restore_directory_unavailable"; error_number = errno; }
                else {
                    int moved = trash_move_file_at(fd, entry->d_name, destination, metadata.name);
                    error_number = moved ? errno : 0;
                    if (close(destination) && !moved) { error_number = errno; moved = -1; }
                    applied = moved ? -1 : 1;
                    if (moved) error = "restore_failed";
                    else {
                        char meta_name[512]; (void)snprintf(meta_name, sizeof meta_name, "%s.meta", entry->d_name);
                        if (unlinkat(fd, meta_name, 0) || fsync(fd)) { error = "metadata_cleanup_failed"; error_number = errno; }
                        else { durable = 1; completed++; }
                    }
                }
            }
            int delivered = trash_native_action(&rows, "restore", path, original[0] ? original : NULL,
                                                 entry->d_name, dry, applied, durable, error, error_number);
            if (delivered) { rc = delivered; complete = 0; break; }
            if (error) { rc = 1; break; }
        } else {
            if (error) rc = 1;
            if (info) continue;
            jdv_record_input_t fields[] = {
                {JDV_LITERAL("record"), jash_discovery_string(&rows, "entry")},
                {JDV_LITERAL("entry"), jash_discovery_path(&rows, entry->d_name)},
                {JDV_LITERAL("path"), jash_discovery_path(&rows, path)},
                {JDV_LITERAL("original_path"), jash_discovery_path(&rows, original[0] ? original : NULL)},
                {JDV_LITERAL("name_source"), jash_discovery_string(&rows, metadata.source)},
                {JDV_LITERAL("kind"), jash_discovery_string(&rows, observed ? trash_native_kind(state.st_mode) : NULL)},
                {JDV_LITERAL("size"), observed && state.st_size >= 0 ? jdv_value_filesize((uint64_t)state.st_size) : jdv_value_nothing()},
                {JDV_LITERAL("mtime_seconds"), observed ? jdv_value_i64((int64_t)state.st_mtime) : jdv_value_nothing()},
                {JDV_LITERAL("ttl_seconds"), observed && state.st_size >= 0 ? jdv_value_u64((uint64_t)jash_trash_ttl_for((long)state.st_size)) : jdv_value_nothing()},
                {JDV_LITERAL("error"), jash_discovery_string(&rows, error)}
            };
            if (jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0])) { rc = 75; complete = 0; break; }
        }
    }
    if (closedir(directory) && !rc) { source_error = "directory_close_failed"; rc = 1; complete = 0; }
finish:
    if (restore && !matched && !source_error) { source_error = "not_found"; rc = 1; }
    (void)trash_native_summary(&rows, operation, count, bytes, completed, complete, source_error);
    return trash_native_finish(&rows, writer, context, mode, rc);
}

static int trash_native_move(jash_writer_t writer, void* context, int mode, int dry, int argc, char** argv) {
    char trash_path[512];
    if (trash_get_dir(trash_path, sizeof trash_path)) return jash_discovery_error(writer, context, "trash", mode, "trash_path_unavailable", 1);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "trash", mode, "allocation_failed", 125);
    int rc = 0;
    uint64_t completed = 0, observed = 0;
    if (!dry) {
        rc = jash_discovery_effects_begin(&rows, writer, context, "trash", mode, "jash.trash.v1");
        if (rc) { jash_discovery_rows_free(&rows); return rc; }
    }
    int parent = trash_native_open(!dry);
    if (parent < 0 && !(dry && errno == ENOENT)) {
        (void)trash_native_summary(&rows, "move", 0, 0, 0, 0, "trash_unavailable");
        return trash_native_finish(&rows, writer, context, mode, 1);
    }
    static unsigned serial;
    for (int i = 0; i < argc; i++) {
        char source[1024], directory[512], entry[128], destination[1024];
        source[0] = directory[0] = entry[0] = destination[0] = 0;
        const char* error = NULL; int error_number = 0, applied = 0, durable = 0;
        struct stat state;
        if (jash_recovery_canonical(argv[i], source, sizeof source) || lstat(source, &state)) error = "source_unavailable";
        else if (!S_ISREG(state.st_mode) && !S_ISDIR(state.st_mode)) { error = "unsupported_file_type"; errno = ENOTSUP; }
        else {
            const char* slash = strrchr(source, '/');
            size_t length = slash ? (size_t)(slash - source) : 0;
            if (!length) length = 1;
            if (length >= sizeof directory || memchr(source, '\n', length) || memchr(source, '\r', length)) {
                error = "directory_representation_limit"; errno = ENAMETOOLONG;
            } else {
                memcpy(directory, source, length); directory[length] = 0;
                (void)snprintf(entry, sizeof entry, "%lld_%ld_%u_item", (long long)time(NULL), (long)getpid(), ++serial);
                (void)snprintf(destination, sizeof destination, "%s/%s", trash_path, entry);
                if (!dry) {
                    int moved = jash_trash_move_file(source, trash_path, entry, directory);
                    applied = moved ? -1 : 1;
                    if (moved) error = "move_failed";
                    else {
                        durable = 1; completed++;
                        char description[200]; snprintf(description, sizeof description, "trash %.180s", argv[i]);
                        jash_timeline_record(JASH_UNDO_TRASH, description, source, destination);
                    }
                }
            }
        }
        if (error) { error_number = errno; rc = 1; }
        observed++;
        int delivered = trash_native_action(&rows, "move", source[0] ? source : argv[i], destination[0] ? destination : NULL,
                                             entry[0] ? entry : NULL, dry, applied, durable, error, error_number);
        if (delivered) { rc = delivered; break; }
    }
    if (parent >= 0 && close(parent) && !rc) rc = 1;
    (void)trash_native_summary(&rows, "move", observed, 0, completed, observed == (uint64_t)argc, NULL);
    return trash_native_finish(&rows, writer, context, mode, rc);
}

int jash_b_trash_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, "trash", mode, "conflicting_formats", 2);
    if (!mode) return jash_discovery_text(jash_b_trash, state, writer, context, argc, argv);
    const char* operation = NULL;
    char* operands[1024]; int count = 0, ended = 0, dry = 0, all = 0;
    for (int i = 0; i < argc; i++) {
        const char* argument = argv[i];
        if (!ended && !strcmp(argument, "--")) { ended = 1; if (!operation) operation = "move"; continue; }
        if (!ended && jash_discovery_format_flag(argument)) continue;
        if (!ended && !strcmp(argument, "--dry-run")) { if (dry) goto invalid; dry = 1; continue; }
        if (!ended && (!strcmp(argument, "--all") || !strcmp(argument, "-a"))) { if (all) goto invalid; all = 1; continue; }
        if (!operation && !ended) {
            if (!strcmp(argument, "ls")) operation = "list";
            else if (!strcmp(argument, "undel")) operation = "restore";
            else if (!strcmp(argument, "clear") || !strcmp(argument, "empty")) operation = "gc";
            else if (!strcmp(argument, "list") || !strcmp(argument, "info") || !strcmp(argument, "restore") ||
                     !strcmp(argument, "move") || !strcmp(argument, "gc") || !strcmp(argument, "status") ||
                     !strcmp(argument, "on") || !strcmp(argument, "off") || !strcmp(argument, "protect") ||
                     !strcmp(argument, "auto") || !strcmp(argument, "default")) operation = argument;
            if (operation) continue;
            operation = "move";
        }
        if ((!ended && argument[0] == '-') || !*argument || count == 1024) goto invalid;
        operands[count++] = argv[i];
    }
    if (!operation) operation = "list";
    if (!strcmp(operation, "move")) {
        if (!count || all) goto invalid;
        return trash_native_move(writer, context, mode, dry, count, operands);
    }
    if (!strcmp(operation, "restore")) {
        if ((!count && !all) || (count && all)) goto invalid;
        return trash_native_scan(writer, context, mode, operation, dry, all, count, operands);
    }
    if (count || all) goto invalid;
    if (!strcmp(operation, "gc")) return trash_empty(state, writer, context, mode, dry);
    if (dry) goto invalid;
    if (!strcmp(operation, "list") || !strcmp(operation, "info")) return trash_native_scan(writer, context, mode, operation, 0, 0, 0, NULL);
    return trash_native_policy(writer, context, mode, operation);
invalid:
    return jash_discovery_error(writer, context, "trash", mode, "invalid_options", 2);
}
