#ifdef GMUX_SERVER
static int run_server(int listener_fd, int ready_fd)
{
    const uint16_t initial_cols = 80, initial_rows = 24;
    const int initial_cell_width = 10, initial_cell_height = 20;
    GhosttyTerminal terminal = NULL;
    GhosttyKeyEncoder key_encoder = NULL;
    GhosttyKeyEvent key_event = NULL;
    GhosttyMouseEncoder mouse_encoder = NULL;
    GhosttyMouseEvent mouse_event = NULL;
    GhosttyRenderState render_state = NULL;
    GhosttyRenderStateRowIterator row_iter = NULL;
    GhosttyRenderStateRowCells row_cells = NULL;
    GhosttyKittyGraphicsPlacementIterator placement_iter = NULL;
    InputWire input = {0};
    RenderSnapshotWire snapshot = {0};
    EffectsContext effects = {0};
    ServerTheme theme = {0};
    InputResize size = {
        initial_cols, initial_rows,
        initial_cell_width, initial_cell_height,
        initial_cols * initial_cell_width,
        initial_rows * initial_cell_height, 0,
    };
    pid_t child = -1;
    WireConnection connection = { .fd = -1 };
    WireConnection control = { .fd = -1 };
    Pty pty = { .fd = -1 };
    int result = 1;
    bool attached = false, child_exited = false, sent_snapshot = false;
    bool terminal_changed = false;
    uint32_t scroll_serial = 0;
    uint32_t sent_scroll_serial = 0;

    signal(SIGPIPE, SIG_IGN);
    ghostty_sys_set(GHOSTTY_SYS_OPT_DECODE_PNG, (const void *)ghostling_decode_png);
    GhosttyResult err = ghostty_terminal_new(
        NULL, &terminal, initial_cols, initial_rows);
    if (err != GHOSTTY_SUCCESS) goto cleanup;
    if (server_theme_load(&theme)) server_theme_apply(terminal, &theme);

    size_t scrollback_max_lines = 1000;
    ghostty_terminal_set(terminal,
        GHOSTTY_TERMINAL_OPT_SCROLLBACK_MAX_LINES, &scrollback_max_lines);
    ghostty_terminal_resize(terminal, initial_cols, initial_rows,
                            initial_cell_width, initial_cell_height);
    pty.fd = pty_spawn(&child, initial_cols, initial_rows,
                       initial_cell_width, initial_cell_height);
    if (pty.fd < 0) goto cleanup;

    effects = (EffectsContext){
        .pty = &pty,
        .cell_width = initial_cell_width,
        .cell_height = initial_cell_height,
        .cols = initial_cols,
        .rows = initial_rows,
        .color_scheme = !theme.has_background
            || ghostty_color_perceived_luminance(&theme.background) <= 0.5
            ? GHOSTTY_COLOR_SCHEME_DARK : GHOSTTY_COLOR_SCHEME_LIGHT,
    };
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_USERDATA, &effects);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_WRITE_PTY,
                         (const void *)effect_write_pty);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_SIZE,
                         (const void *)effect_size);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_DEVICE_ATTRIBUTES,
                         (const void *)effect_device_attributes);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_XTVERSION,
                         (const void *)effect_xtversion);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_TITLE_CHANGED,
                         (const void *)effect_title_changed);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE,
                         (const void *)effect_clipboard_write);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_SCHEME,
                         (const void *)effect_color_scheme);

    uint64_t kitty_storage_limit = 64 * 1024 * 1024;
    bool medium_enabled = true;
    ghostty_terminal_set(terminal,
        GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &kitty_storage_limit);
    ghostty_terminal_set(terminal,
        GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_FILE, &medium_enabled);
    ghostty_terminal_set(terminal,
        GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_MEDIUM_SHARED_MEM, &medium_enabled);

    if (ghostty_key_encoder_new(NULL, &key_encoder) != GHOSTTY_SUCCESS
        || ghostty_key_event_new(NULL, &key_event) != GHOSTTY_SUCCESS
        || ghostty_mouse_encoder_new(NULL, &mouse_encoder) != GHOSTTY_SUCCESS
        || ghostty_mouse_event_new(NULL, &mouse_event) != GHOSTTY_SUCCESS
        || ghostty_render_state_new(NULL, &render_state) != GHOSTTY_SUCCESS
        || ghostty_render_state_row_iterator_new(NULL, &row_iter)
            != GHOSTTY_SUCCESS
        || ghostty_render_state_row_cells_new(NULL, &row_cells)
            != GHOSTTY_SUCCESS
        || ghostty_kitty_graphics_placement_iterator_new(NULL, &placement_iter)
            != GHOSTTY_SUCCESS)
        goto cleanup;
    GhosttyRenderStateOverscan overscan = { .above = 8, .below = 8 };
    ghostty_render_state_set(render_state,
        GHOSTTY_RENDER_STATE_OPTION_OVERSCAN, &overscan);
    if (ready_fd >= 0) {
        int status = 0;
        write(ready_fd, &status, sizeof(status));
        close(ready_fd);
        ready_fd = -1;
    }

    while (!server_stop) {
        short socket_events = connection.fd >= 0 ? POLLIN : 0;
        if (connection.output.start < connection.output.end)
            socket_events |= POLLOUT;
        struct pollfd ready[4] = {
            { .fd = listener_fd,
              .events = connection.fd < 0 || control.fd < 0 ? POLLIN : 0 },
            { .fd = connection.fd, .events = socket_events },
            { .fd = child_exited ? -1 : pty.fd,
              .events = POLLIN | (pty.output.start < pty.output.end
                                  ? POLLOUT : 0) },
            { .fd = control.fd,
              .events = POLLIN | (control.output.start < control.output.end
                                  ? POLLOUT : 0) },
        };
        if (poll(ready, 4, -1) < 0) {
            if (errno == EINTR) continue;
            goto cleanup;
        }

        bool ping_requested = false;
        // Keep one control slot available while a GUI is attached. Health and
        // kill commands must not wait in the listener backlog for it to leave.
        if (control.fd >= 0 && ready[3].revents) {
            bool connected = true, ping = false;
            if (ready[3].revents & (POLLIN | POLLHUP | POLLERR)) {
                connected = wire_read(&control);
                for (;;) {
                    uint8_t *packed = NULL;
                    size_t packed_len = 0;
                    int frame = wire_take_frame(&control, &packed, &packed_len);
                    if (frame == 0) break;
                    bool valid = frame > 0
                        && input_decode(packed, packed_len, &input);
                    free(packed);
                    if (valid) {
                        for (size_t offset = 0; offset < input.len;
                             offset += sizeof(InputRecord)) {
                            InputRecord record;
                            memcpy(&record, input.data + offset, sizeof(record));
                            if (record.size == 0 && record.kind == INPUT_PING)
                                ping = true;
                            else if (record.size == 0
                                     && record.kind == INPUT_SHUTDOWN)
                                server_stop = 1;
                            else {
                                valid = false;
                                break;
                            }
                        }
                    }
                    if (!valid) { connected = false; break; }
                }
            }
            if (connected && ping) {
                snapshot.len = 0;
                connected = snapshot_record(&snapshot, SNAPSHOT_PONG,
                    &attached, sizeof(attached))
                    && snapshot_queue(&control, &snapshot);
            }
            if (connected) connected = wire_flush(&control);
            if (!connected) wire_connection_close(&control);
        }

        if (connection.fd >= 0
            && ready[1].revents & (POLLIN | POLLOUT | POLLHUP | POLLERR)) {
            bool connected = true;
            if (ready[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                connected = wire_read(&connection);
                for (;;) {
                    uint8_t *packed = NULL;
                    size_t packed_len = 0;
                    int frame = wire_take_frame(
                        &connection, &packed, &packed_len);
                    if (frame == 0) break;
                    if (frame < 0
                        || !input_decode(packed, packed_len, &input)) {
                        free(packed);
                        connected = false;
                        break;
                    }
                    free(packed);
                    terminal_changed |= apply_input_wire(
                        &input, &pty, terminal, key_encoder, key_event,
                        mouse_encoder, mouse_event, &size, &attached,
                        &ping_requested, &scroll_serial);
                    effects.cols = size.cols;
                    effects.rows = size.rows;
                    effects.cell_width = (int)size.cell_width;
                    effects.cell_height = (int)size.cell_height;
                }
            }
            if (connected && ready[1].revents & POLLOUT)
                connected = wire_flush(&connection);
            if (!connected) {
                wire_connection_close(&connection);
                attached = false;
                sent_snapshot = false;
                continue;
            }
        }

        if (connection.fd >= 0 && ping_requested) {
            snapshot.len = 0;
            if (!snapshot_record(&snapshot, SNAPSHOT_PONG,
                    &attached, sizeof(attached))
                || !snapshot_queue(&connection, &snapshot)
                || !wire_flush(&connection)) {
                wire_connection_close(&connection);
                attached = false;
                sent_snapshot = false;
                continue;
            }
        }

        // Retire disconnected clients before assigning a newly accepted one.
        // A health check closing as a GUI connects must not put that GUI into
        // the control-only slot just because both descriptors woke poll().
        if (ready[0].revents & POLLIN) {
            int client;
            do client = accept(listener_fd, NULL, NULL);
            while (client < 0 && errno == EINTR);
            if (client >= 0) {
                WireConnection *slot = connection.fd < 0
                    ? &connection : &control;
                if (!wire_connection_init(slot, client))
                    close(client);
                if (slot == &connection) {
                    attached = false;
                    sent_snapshot = false;
                }
            }
        }

        if (ready[2].revents & (POLLIN | POLLHUP | POLLERR)) {
            if (pty_read(pty.fd, terminal, &terminal_changed) != PTY_READ_OK) {
                child_exited = true;
                close(pty.fd);
                pty.fd = -1;
                pty.output.start = pty.output.end = 0;
            }
        }
        if (!child_exited && ready[2].revents & POLLOUT)
            pty.failed |= !wire_buffer_flush(&pty.output, pty.fd);
        if (pty.failed) {
            fprintf(stderr, "gmux: failed to deliver input to PTY\n");
            goto cleanup;
        }

        // Parse PTY output continuously, but accumulate dirty rows while the
        // client is behind. Queue a new snapshot only after the last one drains.
        if (terminal_changed && attached
            && connection.output.start == connection.output.end) {
            ghostty_render_state_update(render_state, terminal);
            GhosttyRenderStateDirty dirty = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
            ghostty_render_state_get(render_state,
                GHOSTTY_RENDER_STATE_DATA_DIRTY, &dirty);
            if (!sent_snapshot) dirty = GHOSTTY_RENDER_STATE_DIRTY_FULL;
            if (dirty != GHOSTTY_RENDER_STATE_DIRTY_FALSE
                || scroll_serial != sent_scroll_serial
                || effects.title_changed || effects.clipboard_changed) {
                GhosttyTerminalScrollbar scrollbar = {0};
                bool has_scrollbar = ghostty_terminal_get(terminal,
                    GHOSTTY_TERMINAL_DATA_SCROLLBAR, &scrollbar)
                    == GHOSTTY_SUCCESS;
                snapshot.len = 0;
                bool snapshot_ok = serialize_render_snapshot(
                    &snapshot, render_state, row_iter, row_cells, dirty,
                    has_scrollbar ? &scrollbar : NULL,
                    terminal, placement_iter, &theme, scroll_serial);
                if (effects.title_changed || (!sent_snapshot && *effects.title))
                    snapshot_ok &= snapshot_record(&snapshot, SNAPSHOT_TITLE,
                                    effects.title, strlen(effects.title));
                if (effects.clipboard_changed
                    && effects.clipboard_len <= UINT32_MAX - sizeof(SnapshotClipboard)) {
                    SnapshotClipboard clipboard = {
                        .location = effects.clipboard_location,
                        .data_len = (uint32_t)effects.clipboard_len,
                    };
                    SnapshotRecord record = {
                        SNAPSHOT_CLIPBOARD,
                        (uint32_t)(sizeof(clipboard)
                            + effects.clipboard_len),
                    };
                    snapshot_ok &= snapshot_append(&snapshot, &record, sizeof(record));
                    snapshot_ok &= snapshot_append(&snapshot, &clipboard,
                                    sizeof(clipboard));
                    snapshot_ok &= snapshot_append(&snapshot, effects.clipboard_data,
                                    effects.clipboard_len);
                }
                if (!snapshot_ok || !snapshot_queue(&connection, &snapshot)
                    || !wire_flush(&connection)) {
                    wire_connection_close(&connection);
                    attached = false;
                    sent_snapshot = false;
                    continue;
                }
                effects.title_changed = false;
                effects.clipboard_changed = false;
                sent_scroll_serial = scroll_serial;
                sent_snapshot = true;
            }
            terminal_changed = false;
        }
    }
    result = 0;

cleanup:
    if (ready_fd >= 0) {
        int status = EIO;
        write(ready_fd, &status, sizeof(status));
        close(ready_fd);
    }
    free(effects.clipboard_data);
    free(input.data);
    free(snapshot.data);
    wire_connection_close(&connection);
    wire_connection_close(&control);
    free(pty.output.data);
    if (pty.fd >= 0) close(pty.fd);
    if (child > 0) {
        if (!child_exited) kill(child, SIGHUP);
        waitpid(child, NULL, 0);
    }
    if (placement_iter) ghostty_kitty_graphics_placement_iterator_free(placement_iter);
    if (row_cells) ghostty_render_state_row_cells_free(row_cells);
    if (row_iter) ghostty_render_state_row_iterator_free(row_iter);
    if (render_state) ghostty_render_state_free(render_state);
    if (mouse_event) ghostty_mouse_event_free(mouse_event);
    if (mouse_encoder) ghostty_mouse_encoder_free(mouse_encoder);
    if (key_event) ghostty_key_event_free(key_event);
    if (key_encoder) ghostty_key_encoder_free(key_encoder);
    if (terminal) ghostty_terminal_free(terminal);
    return result;
}

static int run_server_socket(const char *path, int ready_fd)
{
    int listener = unix_socket(path, true);
    if (listener < 0) {
        int status = errno ? errno : EIO;
        if (ready_fd >= 0) write(ready_fd, &status, sizeof(status));
        if (ready_fd >= 0) close(ready_fd);
        return 1;
    }
    struct sigaction action = { .sa_handler = stop_server };
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    int result = run_server(listener, ready_fd);
    close(listener);
    unlink(path);
    return result;
}

static int daemonize_server(const char *path)
{
    char theme_path[PATH_MAX];
    if (*gmux_config.theme
        && !server_theme_path(gmux_config.theme, theme_path, sizeof(theme_path))) {
        fprintf(stderr, "gmux theme not found: %s\n", gmux_config.theme);
        return 1;
    }

    int ready[2];
    if (pipe(ready) < 0) {
        perror("pipe");
        return 1;
    }
    fcntl(ready[0], F_SETFD, FD_CLOEXEC);
    fcntl(ready[1], F_SETFD, FD_CLOEXEC);

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        close(ready[0]);
        close(ready[1]);
        return 1;
    }
    if (child == 0) {
        close(ready[0]);
        if (setsid() < 0) {
            int status = errno ? errno : EIO;
            write(ready[1], &status, sizeof(status));
            _exit(1);
        }
        pid_t daemon = fork();
        if (daemon < 0) {
            int status = errno ? errno : EIO;
            write(ready[1], &status, sizeof(status));
            _exit(1);
        }
        if (daemon > 0) _exit(0);

        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        _exit(run_server_socket(path, ready[1]));
    }

    close(ready[1]);
    int status = EIO;
    ssize_t n;
    do n = read(ready[0], &status, sizeof(status)); while (n < 0 && errno == EINTR);
    close(ready[0]);
    waitpid(child, NULL, 0);
    if (n != sizeof(status) || status != 0) {
        fprintf(stderr, "failed to start server on %s: %s\n", path,
                strerror(status));
        return 1;
    }
    printf("gmux server listening on %s\n", path);
    return 0;
}

static int health_response(const void *data, size_t len)
{
    msgpack_unpacked unpacked;
    msgpack_unpacked_init(&unpacked);
    size_t offset = 0;
    bool healthy = msgpack_unpack_next(&unpacked, data, len, &offset)
            == MSGPACK_UNPACK_SUCCESS
        && offset == len
        && unpacked.data.type == MSGPACK_OBJECT_ARRAY
        && unpacked.data.via.array.size == 1;
    bool attached = false;
    if (healthy) {
        msgpack_object message = unpacked.data.via.array.ptr[0];
        uint64_t kind = 0;
        healthy = message.type == MSGPACK_OBJECT_ARRAY
            && (message.via.array.size == 1 || message.via.array.size == 2)
            && object_u64(message.via.array.ptr[0], &kind)
            && kind == SNAPSHOT_PONG
            && (message.via.array.size == 1
                || object_bool(message.via.array.ptr[1], &attached));
    }
    msgpack_unpacked_destroy(&unpacked);
    return healthy ? (attached ? 2 : 0) : 1;
}

static int check_server(const char *path)
{
    int fd = unix_socket(path, false);
    if (fd < 0) return errno == EAGAIN || errno == EWOULDBLOCK ? 2 : 1;
    WireConnection connection = { .fd = -1 };
    InputWire input = {0};
    // Exit 2 means busy, never stale. Older servers cannot answer while a GUI
    // is attached; newer servers include that state in their pong.
    int result = 2;
    if (!wire_connection_init(&connection, fd)) {
        close(fd);
        return 1;
    }
    if (!input_append(&input, INPUT_PING, NULL, 0)
        || !input_queue(&connection, &input)
        || !wire_flush(&connection)) goto cleanup;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + 1000;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t remaining = deadline
            - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        if (remaining <= 0) break;
        struct pollfd ready = { .fd = connection.fd, .events = POLLIN };
        if (connection.output.start < connection.output.end)
            ready.events |= POLLOUT;
        int polled = poll(&ready, 1, (int)remaining);
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) break;
        if ((ready.revents & POLLOUT) && !wire_flush(&connection)) break;
        bool connected = wire_read(&connection);
        uint8_t *packed = NULL;
        size_t packed_len = 0;
        int frame = wire_take_frame(&connection, &packed, &packed_len);
        if (frame == 1) result = health_response(packed, packed_len);
        free(packed);
        if (frame != 0 || !connected) break;
    }

cleanup:
    free(input.data);
    wire_connection_close(&connection);
    return result;
}

#endif
