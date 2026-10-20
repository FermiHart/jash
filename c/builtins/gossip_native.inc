// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native records share the persisted ring format with the existing channel.
 * Receive only opens an immutable published inode; send owns the writer lock. */
static int gossip_channel_load(int directory, const char* name, channel_t* data) {
    memcpy(data->magic, "JASHG001", 8);
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    int error = fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || (st.st_mode & 077) || (st.st_size && st.st_size != sizeof(*data));
    size_t used = 0;
    while (!error && st.st_size && used < sizeof(*data)) {
        ssize_t n = read(fd, (char*)data + used, sizeof(*data) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = 1; break; }
        used += (size_t)n;
    }
    if (close(fd)) error = 1;
    if (error || memcmp(data->magic, "JASHG001", 8)) return -1;
    uint64_t count = data->sequence < GOSSIP_MAX ? data->sequence : GOSSIP_MAX;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t seq = data->sequence - i;
        channel_entry_t* entry = &data->entries[(seq - 1) % GOSSIP_MAX];
        if (entry->sequence != seq || !memchr(entry->message, 0, sizeof entry->message) ||
            !memchr(entry->sender, 0, sizeof entry->sender)) return -1;
    }
    return 0;
}

static int gossip_channel_publish(int directory, const char* name, const channel_t* data, int* applied) {
    static unsigned long serial;
    char temp[120]; int n = snprintf(temp, sizeof temp, "%s.tmp.%ld.%lu", name, (long)getpid(), ++serial);
    if (n < 0 || (size_t)n >= sizeof temp) return -1;
    int fd = openat(directory, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    size_t used = 0; int error = 0;
    while (used < sizeof(*data)) {
        ssize_t written = write(fd, (const char*)data + used, sizeof(*data) - used);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { error = 1; break; }
        used += (size_t)written;
    }
    if (!error && fsync(fd)) error = 1;
    if (close(fd)) error = 1;
    if (!error && renameat(directory, temp, directory, name)) error = 1;
    else if (!error) { *applied = 1; if (fsync(directory)) error = 1; }
    if (error && !*applied) (void)unlinkat(directory, temp, 0);
    return error ? -1 : 0;
}

static jdv_value_t gossip_entry_value(semantic_value_t* sem, const char* sender,
        const char* message, int64_t timestamp, uint64_t sequence, int persistent) {
    jdv_record_input_t fields[] = {
        semantic_field("sender", semantic_text(sem, sender)),
        semantic_field("message", semantic_text(sem, message)),
        semantic_field("timestamp", jdv_value_i64(timestamp)),
        semantic_field("timestamp_unit", semantic_symbol(sem, "unix-seconds")),
        semantic_field("sequence", persistent ? jdv_value_u64(sequence) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 5);
}

static int gossip_native_provider(semantic_builtin_t* call, int argc, char** argv, const char* channel) {
    if (!argc) return semantic_error(call, "missing_operation", 2);
    const char* operation = argv[0];
    int send = !strcmp(operation, "send"), clear = !strcmp(operation, "clear");
    int receive = !strcmp(operation, "receive");
    if (!send && !receive && !clear) return semantic_error(call, "invalid_operation", 2);
    if (channel && clear) return semantic_error(call, "channel_clear_unavailable", 2);
    const char* message = NULL; uint64_t after = 0, limit = GOSSIP_MAX;
    if (send) {
        if (argc != 2 || strlen(argv[1]) >= sizeof(messages[0].message)) return semantic_error(call, "message_limit_or_arguments", 2);
        message = argv[1];
    } else if (clear) {
        if (argc != 1) return semantic_error(call, "invalid_arguments", 2);
    } else for (int i = 1; i < argc; i += 2) {
        if (i + 1 == argc || !channel) return semantic_error(call, "invalid_arguments", 2);
        if (!strcmp(argv[i], "--after")) { if (channel_number(argv[i + 1], &after)) return semantic_error(call, "invalid_cursor", 2); }
        else if (!strcmp(argv[i], "--limit")) { if (channel_number(argv[i + 1], &limit) || !limit || limit > GOSSIP_MAX) return semantic_error(call, "invalid_limit", 2); }
        else return semantic_error(call, "invalid_arguments", 2);
    }
    if (channel) {
        size_t length = strlen(channel);
        if (!length || length > 48) return semantic_error(call, "invalid_channel", 2);
        for (const char* p = channel; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return semantic_error(call, "invalid_channel", 2);
    }
    if ((send || clear) && semantic_admit(call)) return 125;
    semantic_value_t* sem = &call->values;
    jdv_value_t items[GOSSIP_MAX]; size_t count = 0;
    uint64_t sequence = 0, next = after, oldest = 1;
    int status = 0, applied = 0, durable = 0, expired = 0;
    const char* error = NULL;
    if (!channel) {
        if (send) {
            int slot = (last_idx + 1) % GOSSIP_MAX;
            snprintf(messages[slot].sender, sizeof messages[slot].sender, "%s", get_sender());
            memcpy(messages[slot].message, message, strlen(message) + 1);
            messages[slot].timestamp = time(NULL); last_idx = slot;
            if (msg_count < GOSSIP_MAX) msg_count++;
            applied = 1;
        } else if (clear) { msg_count = 0; last_idx = -1; applied = 1; }
        if (receive) for (int i = 0; i < msg_count; i++) {
            int slot = (last_idx - msg_count + 1 + i + GOSSIP_MAX) % GOSSIP_MAX;
            items[count++] = gossip_entry_value(sem, messages[slot].sender, messages[slot].message, messages[slot].timestamp, 0, 0);
        }
    } else {
        int home_fd = -1, directory = -1, lock = -1;
        const char* home = getenv("HOME");
        channel_t data = {0};
        if (!home || *home != '/') { error = "home_unavailable"; goto channel_done; }
        home_fd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (home_fd < 0) { error = "home_unavailable"; goto channel_done; }
        if (send && mkdirat(home_fd, ".jash", 0700) && errno != EEXIST) { error = "journal_unavailable"; goto channel_done; }
        directory = openat(home_fd, ".jash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (directory < 0) { if (errno != ENOENT || send) error = "journal_unavailable"; goto channel_done; }
        struct stat st;
        if (fstat(directory, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) { error = "journal_unavailable"; goto channel_done; }
        char name[80], lockname[96]; snprintf(name, sizeof name, "gossip-%s.bin", channel);
        if (send) {
            snprintf(lockname, sizeof lockname, "%s.lock", name);
            lock = openat(directory, lockname, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
            if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
                (st.st_mode & 077) || st.st_nlink != 1 || flock(lock, LOCK_EX)) { error = "journal_lock_failed"; goto channel_done; }
        }
        if (gossip_channel_load(directory, name, &data)) { error = "journal_invalid"; goto channel_done; }
        sequence = data.sequence;
        oldest = sequence > GOSSIP_MAX ? sequence - GOSSIP_MAX + 1 : 1;
        expired = receive && after && (after < oldest - 1 || after > sequence);
        if (expired) { error = "cursor_expired"; status = 2; goto channel_done; }
        if (send) {
            if (sequence == UINT64_MAX) { error = "sequence_exhausted"; goto channel_done; }
            uint64_t selected = sequence + 1;
            channel_entry_t* entry = &data.entries[(selected - 1) % GOSSIP_MAX];
            memset(entry, 0, sizeof *entry); entry->sequence = selected; entry->timestamp = time(NULL);
            snprintf(entry->sender, sizeof entry->sender, "%ld", (long)getpid());
            memcpy(entry->message, message, strlen(message) + 1); data.sequence = selected;
            if (gossip_channel_publish(directory, name, &data, &applied)) error = "journal_publish_failed";
            else durable = 1;
            if (applied) sequence = selected;
            next = sequence;
        } else if (after < sequence) {
            uint64_t first = after >= oldest ? after + 1 : oldest;
            for (uint64_t current = first; current <= sequence && count < limit; current++) {
                channel_entry_t* entry = &data.entries[(current - 1) % GOSSIP_MAX];
                items[count++] = gossip_entry_value(sem, entry->sender, entry->message, entry->timestamp, current, 1);
                next = current;
                if (current == UINT64_MAX) break;
            }
        }
channel_done:
        if (!error && receive && after > sequence) { error = "cursor_expired"; status = 2; }
        if (lock >= 0 && close(lock) && !error) error = "journal_close_failed";
        if (directory >= 0 && close(directory) && !error) error = "journal_close_failed";
        if (home_fd >= 0 && close(home_fd) && !error) error = "journal_close_failed";
    }
    if (error && !status) status = 1;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.gossip.v1")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("scope", semantic_symbol(sem, channel ? "home-channel-ring" : "process-local-ring")),
        semantic_field("channel", semantic_text(sem, channel)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("applied", jdv_value_bool(applied)),
        semantic_field("persisted", jdv_value_bool(channel && send && applied)),
        semantic_field("durable", jdv_value_bool(durable)),
        semantic_field("sequence", channel ? jdv_value_u64(sequence) : jdv_value_nothing()),
        semantic_field("messages", semantic_list(sem, items, count)),
        semantic_field("next_cursor", channel ? jdv_value_u64(next) : jdv_value_nothing()),
        semantic_field("retention_items", jdv_value_u64(GOSSIP_MAX)),
        semantic_field("complete", jdv_value_bool(!error)),
        semantic_field("has_more", jdv_value_bool(channel && receive && !error && next < sequence))
    };
    if (!channel && receive && call->mode == 1 && !sem->error)
        return value_output_json(call->writer, call->context, "gossip", &fields[9].value, &sem->limits, SEMANTIC_OUTPUT_BYTES, status);
    return semantic_finish(call, semantic_record(sem, fields, sizeof fields / sizeof *fields), status);
}

static int gossip_native(jash_writer_t writer, void* context, int argc, char** argv) {
    int mode = 0, index = 0; const char* channel = NULL;
    while (index < argc) {
        const char* arg = argv[index];
        if (!strcmp(arg, "--json") || !strcmp(arg, "--data") || !strcmp(arg, "--data-v2")) {
            int selected = !strcmp(arg, "--json") ? 1 : 2;
            if (mode && mode != selected) return jash_discovery_error(writer, context, "gossip", mode, "conflicting_formats", 2);
            mode = selected; index++;
        } else if (!strcmp(arg, "--channel") && !channel && index + 1 < argc) { channel = argv[index + 1]; index += 2; }
        else break;
    }
    if (!mode && channel) mode = 1;
    if (!mode && argc == 2 && !strcmp(argv[0], "receive") && !strcmp(argv[1], "--json")) { mode = 1; argc = 1; }
    if (!mode) return -1;
    semantic_builtin_t call = {.command="gossip", .schema_id="jash.gossip.v1", .mode=mode, .writer=writer, .context=context};
    if (semantic_value_init(&call.values, 262144, 131072)) return 125;
    int result = gossip_native_provider(&call, argc - index, argv + index, channel);
    jdv_wire_push_v2_close(call.push);
    if (call.output.restore_sigpipe) (void)jash_structured_finish(&call.output, 125);
    free(call.workspace); semantic_value_free(&call.values); return result;
}
