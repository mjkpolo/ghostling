#ifdef GMUX_SERVER
static void mouse_encode_and_write(Pty *pty, GhosttyMouseEncoder encoder,
                                   GhosttyMouseEvent event)
{
    char buf[128];
    size_t written = 0;
    GhosttyResult result = ghostty_mouse_encoder_encode(
        encoder, event, buf, sizeof(buf), &written);
    if (result == GHOSTTY_SUCCESS && written > 0)
        pty_write(pty, buf, written);
}

static bool apply_input_wire(InputWire *wire, Pty *pty,
                             GhosttyTerminal terminal,
                             GhosttyKeyEncoder key_encoder,
                             GhosttyKeyEvent key_event,
                             GhosttyMouseEncoder mouse_encoder,
                             GhosttyMouseEvent mouse_event,
                             InputResize *size, bool *attached,
                             bool *ping_requested, uint32_t *scroll_serial)
{
    bool terminal_changed = false;
    size_t offset = 0;
    while (offset + sizeof(InputRecord) <= wire->len) {
        InputRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len)
            break;
        uint8_t *payload = wire->data + offset;

        if (record.kind == INPUT_ATTACH && record.size == 0) {
            *attached = true;
            terminal_changed = true;
        } else if (record.kind == INPUT_DETACH && record.size == 0) {
            *attached = false;
        } else if (record.kind == INPUT_SHUTDOWN && record.size == 0) {
            server_stop = 1;
        } else if (record.kind == INPUT_PING && record.size == 0) {
            *ping_requested = true;
        } else if (record.kind == INPUT_RESIZE
                   && record.size == sizeof(InputResize)) {
            memcpy(size, payload, sizeof(*size));
            ghostty_terminal_resize(terminal, size->cols, size->rows,
                                    size->cell_width, size->cell_height);
            struct winsize ws = {
                .ws_row = size->rows,
                .ws_col = size->cols,
                .ws_xpixel = (unsigned short)(size->cols * size->cell_width),
                .ws_ypixel = (unsigned short)(size->rows * size->cell_height),
            };
            ioctl(pty->fd, TIOCSWINSZ, &ws);
            terminal_changed = true;
        } else if (record.kind == INPUT_KEY
                   && record.size == sizeof(InputKey)) {
            InputKey message;
            memcpy(&message, payload, sizeof(message));
            bool encoded = false;
            if (message.key == GHOSTTY_KEY_UNIDENTIFIED) {
                if (message.text_len > 0
                    && message.text_len <= sizeof(message.text)) {
                    pty_write(pty, message.text, message.text_len);
                    encoded = true;
                }
            } else {
                ghostty_key_encoder_setopt_from_terminal(key_encoder, terminal);
                ghostty_key_event_set_key(key_event, message.key);
                ghostty_key_event_set_action(key_event, message.action);
                ghostty_key_event_set_mods(key_event, message.mods);
                ghostty_key_event_set_consumed_mods(
                    key_event, message.consumed_mods);
                ghostty_key_event_set_unshifted_codepoint(
                    key_event, message.unshifted_codepoint);
                ghostty_key_event_set_utf8(key_event, message.text,
                                           message.text_len);
                char buf[128];
                size_t written = 0;
                if (ghostty_key_encoder_encode(key_encoder, key_event,
                        buf, sizeof(buf), &written) == GHOSTTY_SUCCESS
                    && written > 0) {
                    pty_write(pty, buf, written);
                    encoded = true;
                }
            }
            // Ghostty returns to the live screen only after a key event really
            // encoded PTY bytes. Client-only bindings such as copy never arrive
            // here, so copying from scrollback leaves the viewport untouched.
            if (encoded && message.action != GHOSTTY_KEY_ACTION_RELEASE) {
                GhosttyTerminalScrollViewport bottom = {
                    .tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM,
                };
                ghostty_terminal_scroll_viewport(terminal, bottom);
                *scroll_serial = 0;
                terminal_changed = true;
            }
        } else if (record.kind == INPUT_MOUSE
                   && record.size == sizeof(InputMouse)) {
            InputMouse message;
            memcpy(&message, payload, sizeof(message));
            bool mouse_tracking = false;
            ghostty_terminal_get(terminal,
                GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING, &mouse_tracking);
            if (message.wheel && !mouse_tracking) {
                GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
                GhosttyTerminalModeConfig alt_scroll = {
                    .mode = GHOSTTY_MODE_ALT_SCROLL,
                };
                ghostty_terminal_get(terminal,
                    GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN, &screen);
                ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE,
                                     &alt_scroll);
                if (screen == GHOSTTY_TERMINAL_SCREEN_ALTERNATE
                    && alt_scroll.value) {
                    GhosttyTerminalModeConfig cursor_keys = {
                        .mode = GHOSTTY_MODE_DECCKM,
                    };
                    ghostty_terminal_get(terminal,
                        GHOSTTY_TERMINAL_DATA_MODE, &cursor_keys);
                    const char *sequence = message.wheel > 0
                        ? (cursor_keys.value ? "\x1bOA" : "\x1b[A")
                        : (cursor_keys.value ? "\x1bOB" : "\x1b[B");
                    int count = message.wheel > 0
                        ? message.wheel : -(int)message.wheel;
                    for (int i = 0; i < count; i++)
                        pty_write(pty, sequence, 3);
                } else {
                    GhosttyTerminalScrollViewport scroll = {
                        .tag = GHOSTTY_SCROLL_VIEWPORT_DELTA,
                        .value = { .delta = -message.wheel },
                    };
                    ghostty_terminal_scroll_viewport(terminal, scroll);
                    *scroll_serial = message.scroll_serial;
                    terminal_changed = true;
                }
            } else {
                ghostty_mouse_encoder_setopt_from_terminal(
                    mouse_encoder, terminal);
                GhosttyMouseEncoderSize encoder_size = {
                    .size = sizeof(GhosttyMouseEncoderSize),
                    .screen_width = size->screen_width,
                    .screen_height = size->screen_height,
                    .cell_width = size->cell_width,
                    .cell_height = size->cell_height,
                    .padding_top = size->padding,
                    .padding_bottom = size->padding,
                    .padding_left = size->padding,
                    .padding_right = size->padding,
                };
                ghostty_mouse_encoder_setopt(mouse_encoder,
                    GHOSTTY_MOUSE_ENCODER_OPT_SIZE, &encoder_size);
                ghostty_mouse_encoder_setopt(mouse_encoder,
                    GHOSTTY_MOUSE_ENCODER_OPT_ANY_BUTTON_PRESSED,
                    &message.any_button_pressed);
                bool track_cell = true;
                ghostty_mouse_encoder_setopt(mouse_encoder,
                    GHOSTTY_MOUSE_ENCODER_OPT_TRACK_LAST_CELL, &track_cell);
                ghostty_mouse_event_set_mods(mouse_event, message.mods);
                ghostty_mouse_event_set_position(mouse_event,
                    (GhosttyMousePosition){ message.x, message.y });
                ghostty_mouse_event_set_action(mouse_event,
                    message.wheel ? GHOSTTY_MOUSE_ACTION_PRESS : message.action);
                if (message.wheel)
                    ghostty_mouse_event_set_button(mouse_event,
                        message.wheel > 0 ? GHOSTTY_MOUSE_BUTTON_FOUR
                                          : GHOSTTY_MOUSE_BUTTON_FIVE);
                else if (message.has_button)
                    ghostty_mouse_event_set_button(mouse_event, message.button);
                else
                    ghostty_mouse_event_clear_button(mouse_event);
                mouse_encode_and_write(pty, mouse_encoder, mouse_event);
                if (message.wheel) {
                    ghostty_mouse_event_set_action(
                        mouse_event, GHOSTTY_MOUSE_ACTION_RELEASE);
                    mouse_encode_and_write(pty, mouse_encoder, mouse_event);
                }
            }
        } else if (record.kind == INPUT_FOCUS
                   && record.size == sizeof(InputFocus)) {
            InputFocus message;
            memcpy(&message, payload, sizeof(message));
            GhosttyTerminalModeConfig mode = {
                .mode = GHOSTTY_MODE_FOCUS_EVENT,
            };
            if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE,
                                     &mode) == GHOSTTY_SUCCESS && mode.value) {
                char buf[8];
                size_t written = 0;
                ghostty_focus_encode(message.focused ? GHOSTTY_FOCUS_GAINED
                                                      : GHOSTTY_FOCUS_LOST,
                                     buf, sizeof(buf), &written);
                pty_write(pty, buf, written);
            }
        } else if (record.kind == INPUT_PASTE) {
            bool bracketed = false;
            GhosttyTerminalModeConfig mode = {
                .mode = GHOSTTY_MODE_BRACKETED_PASTE,
            };
            if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MODE,
                                     &mode) == GHOSTTY_SUCCESS)
                bracketed = mode.value;
            size_t encoded_len = 0;
            // The encoder sanitizes in place. This input record is ours and is
            // discarded after dispatch, so no second copy of the paste is needed.
            if (ghostty_paste_encode((char *)payload, record.size,
                    bracketed, NULL, 0, &encoded_len) == GHOSTTY_OUT_OF_SPACE) {
                char *encoded = malloc(encoded_len);
                if (!encoded) {
                    pty->failed = true;
                } else if (ghostty_paste_encode((char *)payload,
                        record.size, bracketed, encoded, encoded_len,
                        &encoded_len) == GHOSTTY_SUCCESS) {
                    pty_write(pty, encoded, encoded_len);
                    GhosttyTerminalScrollViewport bottom = {
                        .tag = GHOSTTY_SCROLL_VIEWPORT_BOTTOM,
                    };
                    ghostty_terminal_scroll_viewport(terminal, bottom);
                    *scroll_serial = 0;
                    terminal_changed = true;
                }
                free(encoded);
            }
        }
        offset += record.size;
    }
    wire->len = 0;
    return terminal_changed;
}

#endif
