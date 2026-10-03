#ifdef GMUX_CLIENT

static void gtk_draw(GtkDrawingArea *area, cairo_t *cr,
                     int width, int height, gpointer userdata)
{
    GtkClient *client = userdata;
    if (width != client->width || height != client->height) {
        client->width = width;
        client->height = height;
        int cols = (width - 2 * client->pad) / client->cell_width;
        int rows = (height - 2 * client->pad) / client->cell_height;
        if (cols < 1) cols = 1;
        if (rows < 1) rows = 1;
        InputResize resize = {
            .cols = (uint16_t)cols,
            .rows = (uint16_t)rows,
            .cell_width = (uint32_t)client->cell_width,
            .cell_height = (uint32_t)client->cell_height,
            .screen_width = (uint32_t)width,
            .screen_height = (uint32_t)height,
            .padding = (uint32_t)client->pad,
        };
        gtk_client_send(client, INPUT_RESIZE, &resize, sizeof(resize));
    }
    cairo_render_snapshot(cr, &client->render, &client->snapshot,
                          &client->selection,
                          client->regular, client->italic,
                          client->bold, client->bold_italic,
                          client->cell_width, client->cell_height,
                          client->pad, width, height);
    (void)area;
}

static gboolean gtk_socket_ready(gint fd, GIOCondition condition,
                                 gpointer userdata)
{
    (void)fd;
    GtkClient *client = userdata;
    bool connected = !(condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
        && wire_read(&client->connection);
    while (connected) {
        uint8_t *packed = NULL;
        size_t packed_len = 0;
        int frame = wire_take_frame(&client->connection, &packed, &packed_len);
        if (frame == 0) break;
        if (frame < 0
            || !snapshot_decode(packed, packed_len, &client->snapshot)) {
            connected = false;
        } else {
            if (snapshot_updates_selection(&client->snapshot,
                                           &client->selection))
                gtk_clear_selection(client);
            render_client_apply(&client->render, &client->snapshot);
            apply_snapshot_title(client->window, &client->snapshot);
            apply_snapshot_clipboard(client, &client->snapshot);
            gtk_widget_queue_draw(client->area);
        }
        free(packed);
    }
    if (!connected) {
        client->socket_source = 0;
        g_application_quit(G_APPLICATION(client->application));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean gtk_close_requested(GtkWindow *window, gpointer userdata)
{
    (void)window;
    GtkClient *client = userdata;
    gtk_client_send(client, INPUT_DETACH, NULL, 0);
    return FALSE;
}

static void gtk_activate(GtkApplication *application, gpointer userdata)
{
    GtkClient *client = userdata;
    client->window = GTK_WINDOW(gtk_application_window_new(application));
    gtk_window_set_title(client->window, "gmux");
    gtk_window_set_default_size(client->window, 800, 600);

    client->area = gtk_drawing_area_new();
    gtk_widget_set_focusable(client->area, TRUE);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(client->area),
                                   gtk_draw, client, NULL);
    gtk_window_set_child(client->window, client->area);

    gtk_apply_font(client);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(gtk_key_pressed), client);
    g_signal_connect(key, "key-released", G_CALLBACK(gtk_key_released), client);
    gtk_widget_add_controller(client->area, key);

    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);
    g_signal_connect(click, "pressed", G_CALLBACK(gtk_mouse_pressed), client);
    g_signal_connect(click, "released", G_CALLBACK(gtk_mouse_released), client);
    gtk_widget_add_controller(client->area, GTK_EVENT_CONTROLLER(click));

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(gtk_mouse_motion), client);
    gtk_widget_add_controller(client->area, motion);

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(gtk_mouse_scroll), client);
    gtk_widget_add_controller(client->area, scroll);

    g_signal_connect(client->area, "notify::has-focus",
                     G_CALLBACK(gtk_focus_changed), client);
    g_signal_connect(client->window, "close-request",
                     G_CALLBACK(gtk_close_requested), client);

    client->socket_source = g_unix_fd_add(client->connection.fd,
        G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
        gtk_socket_ready, client);
    gtk_client_send(client, INPUT_ATTACH, NULL, 0);
    gtk_window_present(client->window);
    gtk_widget_grab_focus(client->area);
}

static int run_client(int socket_fd)
{
    GtkClient client = { .connection.fd = -1, .pad = 4 };
    if (!wire_connection_init(&client.connection, socket_fd)) {
        close(socket_fd);
        return 1;
    }
    FcInit();
    if (!register_embedded_font(font_monaspace_argon,
                                sizeof(font_monaspace_argon))
        || !register_embedded_font(font_monaspace_argon_italic,
                                   sizeof(font_monaspace_argon_italic))
        || !register_embedded_font(font_monaspace_argon_bold,
                                   sizeof(font_monaspace_argon_bold))
        || !register_embedded_font(font_monaspace_argon_bold_italic,
                                   sizeof(font_monaspace_argon_bold_italic))) {
        fprintf(stderr, "failed to register embedded Monaspace fonts\n");
    }
    gtk_load_config(&client);
    client.regular = pango_font_description_new();
    client.italic = pango_font_description_new();
    client.bold = pango_font_description_new();
    client.bold_italic = pango_font_description_new();
    gtk_apply_font(&client);
    GApplicationFlags flags = G_APPLICATION_NON_UNIQUE;
    client.application = gtk_application_new("io.github.mjkpolo.gmux", flags);
    g_signal_connect(client.application, "activate",
                     G_CALLBACK(gtk_activate), &client);
    int result = g_application_run(G_APPLICATION(client.application), 0, NULL);

    if (client.socket_source) g_source_remove(client.socket_source);
    render_client_free(&client.render);
    free(client.input.data);
    free(client.snapshot.data);
    g_free(client.pending_clipboard);
    wire_connection_close(&client.connection);
    pango_font_description_free(client.italic);
    pango_font_description_free(client.regular);
    pango_font_description_free(client.bold_italic);
    pango_font_description_free(client.bold);
    g_object_unref(client.application);
    return result;
}

#endif
