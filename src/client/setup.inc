#ifdef GMUX_CLIENT
typedef struct {
    GtkApplication *application;
    GtkWindow *window;
    GtkWidget *area;
    WireConnection connection;
    InputWire input;
    RenderSnapshotWire snapshot;
    RenderClientState render;
    PangoFontDescription *regular;
    PangoFontDescription *italic;
    PangoFontDescription *bold;
    PangoFontDescription *bold_italic;
    char font_family[128];
    int font_size;
    int cell_width, cell_height;
    int width, height;
    int pad;
    double mouse_x, mouse_y;
    guint mouse_buttons;
    ClientSelection selection;
    char *pending_clipboard;
    GhosttyClipboardLocation pending_clipboard_location;
    bool keys[256];
    guint socket_source;
    guint write_source;
    bool failed;
    double scroll_y, momentum_velocity;
    gint64 momentum_last_us;
    guint momentum_tick;
    uint32_t next_scroll_serial, inflight_scroll_serial;
    int inflight_scroll_rows;
} GtkClient;

static void gtk_client_fail(GtkClient *client, const char *message)
{
    fprintf(stderr, "gmux: %s\n", message);
    client->failed = true;
    g_application_quit(G_APPLICATION(client->application));
}

static gboolean gtk_socket_writable(gint fd, GIOCondition condition,
                                    gpointer userdata)
{
    (void)fd;
    GtkClient *client = userdata;
    if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL))
        || !wire_flush(&client->connection)) {
        client->write_source = 0;
        gtk_client_fail(client, "socket write failed");
        return G_SOURCE_REMOVE;
    }
    if (client->connection.output.start == client->connection.output.end) {
        client->write_source = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static bool gtk_client_send(GtkClient *client, InputKind kind,
                            const void *payload, size_t size)
{
    if (!input_append(&client->input, kind, payload, size)
        || !input_queue(&client->connection, &client->input)
        || !wire_flush(&client->connection)) {
        gtk_client_fail(client, "cannot send terminal input");
        return false;
    }
    // A full socket is normal. Resume the queued write when it becomes
    // writable, even if the user never generates another input event.
    if (!client->write_source
        && client->connection.output.start < client->connection.output.end)
        client->write_source = g_unix_fd_add(client->connection.fd,
            G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
            gtk_socket_writable, client);
    return true;
}

static void gtk_apply_font(GtkClient *client)
{
    PangoFontDescription *fonts[] = {
        client->regular, client->italic, client->bold, client->bold_italic,
    };
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        pango_font_description_set_family(fonts[i], client->font_family);
        pango_font_description_set_size(fonts[i],
                                        client->font_size * PANGO_SCALE);
    }
    pango_font_description_set_style(client->italic, PANGO_STYLE_ITALIC);
    pango_font_description_set_weight(client->bold, PANGO_WEIGHT_BOLD);
    pango_font_description_set_style(client->bold_italic,
                                     PANGO_STYLE_ITALIC);
    pango_font_description_set_weight(client->bold_italic,
                                      PANGO_WEIGHT_BOLD);
    if (!client->area) return;
    PangoLayout *measure = gtk_widget_create_pango_layout(client->area, "M");
    pango_layout_set_font_description(measure, client->regular);
    pango_layout_get_pixel_size(measure, &client->cell_width,
                                &client->cell_height);
    g_object_unref(measure);
    if (client->cell_width < 1) client->cell_width = 1;
    if (client->cell_height < 1) client->cell_height = 1;
    client->width = client->height = -1;
    gtk_widget_queue_draw(client->area);
}

static GhosttyMouseButton gtk_mouse_button(guint button)
{
    switch (button) {
    case 1: return GHOSTTY_MOUSE_BUTTON_LEFT;
    case 2: return GHOSTTY_MOUSE_BUTTON_MIDDLE;
    case 3: return GHOSTTY_MOUSE_BUTTON_RIGHT;
    case 8: return GHOSTTY_MOUSE_BUTTON_FOUR;
    case 9: return GHOSTTY_MOUSE_BUTTON_FIVE;
    default: return GHOSTTY_MOUSE_BUTTON_UNKNOWN;
    }
}

static GdkModifierType gtk_current_mods(GtkEventController *controller)
{
    return gtk_event_controller_get_current_event_state(controller);
}

#endif
