#ifdef GMUX_CLIENT

static gboolean gtk_key_pressed(GtkEventControllerKey *controller,
                                guint keyval, guint keycode,
                                GdkModifierType state, gpointer userdata)
{
    GtkClient *client = userdata;
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
            == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)
        && (keyval == GDK_KEY_plus || keyval == GDK_KEY_equal
            || keyval == GDK_KEY_KP_Add)) {
        gtk_clear_selection(client);
        if (client->font_size < 96) client->font_size++;
        gtk_apply_font(client);
        return TRUE;
    }
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
            == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)
        && (keyval == GDK_KEY_minus || keyval == GDK_KEY_underscore
            || keyval == GDK_KEY_KP_Subtract)) {
        gtk_clear_selection(client);
        if (client->font_size > 6) client->font_size--;
        gtk_apply_font(client);
        return TRUE;
    }
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
            == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)
        && (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
        gtk_copy_selection(client);
        return TRUE;
    }
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
            == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)
        && (keyval == GDK_KEY_v || keyval == GDK_KEY_V)) {
        gtk_clear_selection(client);
        gtk_paste(client);
        return TRUE;
    }
    switch (keyval) {
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R:
    case GDK_KEY_Control_L: case GDK_KEY_Control_R:
    case GDK_KEY_Alt_L: case GDK_KEY_Alt_R:
    case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L: case GDK_KEY_Super_R:
    case GDK_KEY_Hyper_L: case GDK_KEY_Hyper_R:
    case GDK_KEY_Caps_Lock: case GDK_KEY_Num_Lock:
        break;
    default:
        gtk_clear_selection(client);
        break;
    }
    InputKey message = {
        .key = gdk_key_to_ghostty(keyval),
        .action = keycode < 256 && client->keys[keycode]
            ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS,
        .mods = gdk_mods(state),
        .unshifted_codepoint = gdk_unshifted_codepoint(keyval),
    };
    GdkEvent *event = gtk_event_controller_get_current_event(
        GTK_EVENT_CONTROLLER(controller));
    if (event)
        message.consumed_mods = gdk_mods(
            gdk_key_event_get_consumed_modifiers(event));
    gunichar codepoint = gdk_keyval_to_unicode(keyval);
    if (codepoint && !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK))) {
        int len = g_unichar_to_utf8(codepoint, message.text);
        if (len > 0) message.text_len = (uint8_t)len;
    }
    if (keycode < 256) client->keys[keycode] = true;
    gtk_client_send(client, INPUT_KEY, &message, sizeof(message));
    return TRUE;
}

static void gtk_key_released(GtkEventControllerKey *controller,
                             guint keyval, guint keycode,
                             GdkModifierType state, gpointer userdata)
{
    (void)controller;
    GtkClient *client = userdata;
    if ((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
            == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)
        && (keyval == GDK_KEY_c || keyval == GDK_KEY_C
            || keyval == GDK_KEY_v || keyval == GDK_KEY_V
            || keyval == GDK_KEY_plus || keyval == GDK_KEY_equal
            || keyval == GDK_KEY_KP_Add || keyval == GDK_KEY_minus
            || keyval == GDK_KEY_underscore
            || keyval == GDK_KEY_KP_Subtract)) {
        if (keycode < 256) client->keys[keycode] = false;
        return;
    }
    InputKey message = {
        .key = gdk_key_to_ghostty(keyval),
        .action = GHOSTTY_KEY_ACTION_RELEASE,
        .mods = gdk_mods(state),
        .unshifted_codepoint = gdk_unshifted_codepoint(keyval),
    };
    if (keycode < 256) client->keys[keycode] = false;
    gtk_client_send(client, INPUT_KEY, &message, sizeof(message));
}

static void gtk_mouse_click(GtkGestureClick *gesture, int presses,
                            double x, double y, gpointer userdata,
                            GhosttyMouseAction action)
{
    GtkClient *client = userdata;
    guint button = gtk_gesture_single_get_current_button(
        GTK_GESTURE_SINGLE(gesture));
    GhosttyMouseButton ghostty_button = gtk_mouse_button(button);
    if (ghostty_button == GHOSTTY_MOUSE_BUTTON_UNKNOWN) return;
    if (button == 1 && !client->render.header.mouse_tracking) {
        uint16_t row, col;
        gtk_selection_position(client, x, y, &row, &col);
        if (action == GHOSTTY_MOUSE_ACTION_PRESS) {
            client->selection = (ClientSelection){
                .dragging = true,
                .active = presses > 1,
                .start_row = row,
                .start_col = col,
                .end_row = row,
                .end_col = col,
                .anchor_row = row,
                .anchor_col = col,
                .mode = presses >= 3 ? SELECTION_LINE
                    : presses == 2 ? SELECTION_WORD : SELECTION_CELL,
            };
            gtk_selection_expand(client, row, col);
        } else {
            gtk_selection_expand(client, row, col);
            client->selection.dragging = false;
            if (client->selection.mode == SELECTION_CELL
                && client->selection.start_row == row
                && client->selection.start_col == col)
                client->selection.active = false;
        }
        gtk_widget_queue_draw(client->area);
        return;
    }
    if (action == GHOSTTY_MOUSE_ACTION_PRESS)
        client->mouse_buttons |= 1u << (button < 31 ? button : 0);
    else
        client->mouse_buttons &= ~(1u << (button < 31 ? button : 0));
    client->mouse_x = x;
    client->mouse_y = y;
    InputMouse message = {
        .action = action,
        .button = ghostty_button,
        .mods = gdk_mods(gtk_current_mods(GTK_EVENT_CONTROLLER(gesture))),
        .x = (float)x,
        .y = (float)y,
        .has_button = true,
        .any_button_pressed = client->mouse_buttons != 0,
    };
    gtk_client_send(client, INPUT_MOUSE, &message, sizeof(message));
}

static void gtk_mouse_pressed(GtkGestureClick *gesture, int presses,
                              double x, double y, gpointer userdata)
{
    gtk_set_pending_clipboard(userdata);
    gtk_mouse_click(gesture, presses, x, y, userdata,
                    GHOSTTY_MOUSE_ACTION_PRESS);
}

static void gtk_mouse_released(GtkGestureClick *gesture, int presses,
                               double x, double y, gpointer userdata)
{
    gtk_mouse_click(gesture, presses, x, y, userdata,
                    GHOSTTY_MOUSE_ACTION_RELEASE);
}

static void gtk_mouse_motion(GtkEventControllerMotion *controller,
                             double x, double y, gpointer userdata)
{
    GtkClient *client = userdata;
    client->mouse_x = x;
    client->mouse_y = y;
    if (!client->render.header.mouse_tracking) {
        if (client->selection.dragging) {
            gtk_selection_position(client, x, y,
                                   &client->selection.end_row,
                                   &client->selection.end_col);
            gtk_selection_expand(client, client->selection.end_row,
                                 client->selection.end_col);
            gtk_widget_queue_draw(client->area);
        }
        return;
    }
    InputMouse message = {
        .action = GHOSTTY_MOUSE_ACTION_MOTION,
        .mods = gdk_mods(gtk_current_mods(GTK_EVENT_CONTROLLER(controller))),
        .x = (float)x,
        .y = (float)y,
        .any_button_pressed = client->mouse_buttons != 0,
    };
    if (client->mouse_buttons & (1u << 1)) {
        message.button = GHOSTTY_MOUSE_BUTTON_LEFT;
        message.has_button = true;
    } else if (client->mouse_buttons & (1u << 2)) {
        message.button = GHOSTTY_MOUSE_BUTTON_MIDDLE;
        message.has_button = true;
    } else if (client->mouse_buttons & (1u << 3)) {
        message.button = GHOSTTY_MOUSE_BUTTON_RIGHT;
        message.has_button = true;
    }
    gtk_client_send(client, INPUT_MOUSE, &message, sizeof(message));
}

static gboolean gtk_mouse_scroll(GtkEventControllerScroll *controller,
                                 double dx, double dy, gpointer userdata)
{
    (void)dx;
    if (dy == 0) return FALSE;
    GtkClient *client = userdata;
    InputMouse message = {
        .mods = gdk_mods(gtk_current_mods(GTK_EVENT_CONTROLLER(controller))),
        .x = (float)client->mouse_x,
        .y = (float)client->mouse_y,
        .wheel = dy < 0 ? 1 : -1,
        .any_button_pressed = client->mouse_buttons != 0,
    };
    gtk_client_send(client, INPUT_MOUSE, &message, sizeof(message));
    return TRUE;
}

static void gtk_focus_changed(GtkWidget *widget, GParamSpec *spec,
                              gpointer userdata)
{
    (void)spec;
    GtkClient *client = userdata;
    InputFocus focus = { .focused = gtk_widget_has_focus(widget) };
    gtk_client_send(client, INPUT_FOCUS, &focus, sizeof(focus));
}

#endif
