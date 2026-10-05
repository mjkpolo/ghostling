#ifdef GMUX_CLIENT
typedef struct {
    GtkApplication *application;
    GtkWindow *window;
    GFileMonitor *title_monitor;
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
    bool received_config;
    char last_server_error[4096];
    int cell_width, cell_height;
    int width, height;
    int pad;
    double mouse_x, mouse_y;
    guint mouse_buttons;
    ClientSelection selection;
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

static void manager_title_changed(GFileMonitor *monitor, GFile *file,
                                  GFile *other, GFileMonitorEvent event,
                                  gpointer userdata)
{
    (void)monitor; (void)other; (void)event;
    GtkClient *client = userdata;
    char *title = NULL;
    if (g_file_load_contents(file, NULL, &title, NULL, NULL, NULL)) {
        char *valid = g_utf8_make_valid(title, -1);
        gtk_window_set_title(client->window, valid);
        g_free(valid);
        g_free(title);
    }
}

// GtkAlertDialog requires GTK 4.10; Rocky's older GTK needs MessageDialog.
G_GNUC_BEGIN_IGNORE_DEPRECATIONS
static void gtk_config_error(GtkClient *client, const char *side, const char *message)
{
    char *valid = g_utf8_make_valid(message, -1);
    GtkWidget *dialog = gtk_message_dialog_new(client->window,
        GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
        "%s", side);
    gtk_window_set_title(GTK_WINDOW(dialog), side);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s", valid);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_window_destroy), dialog);
    gtk_window_present(GTK_WINDOW(dialog));
    g_free(valid);
}
G_GNUC_END_IGNORE_DEPRECATIONS

static bool gtk_font_available(GtkClient *client, const char *configured)
{
    // Validate the requested primary family, not the fallback Pango silently
    // chooses when a name is misspelled. Generic aliases are intentional.
    char **names = g_strsplit(configured, ",", 2);
    const char *name = g_strstrip(names[0]);
    bool found = !g_ascii_strcasecmp(name, "monospace")
        || !g_ascii_strcasecmp(name, "sans-serif")
        || !g_ascii_strcasecmp(name, "serif") || !g_ascii_strcasecmp(name, "sans");
    PangoFontMap *map = pango_context_get_font_map(
        gtk_widget_get_pango_context(client->area));
    found = found || pango_font_map_get_family(map, name) != NULL;
    g_strfreev(names);
    return found;
}

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
    // Let Pango try every requested family before our generic fallback.
    // A missing first family must not discard valid later families.
    char *family = g_utf8_validate(client->font_family, -1, NULL)
        ? g_strconcat(client->font_family, ", monospace", NULL)
        : g_strdup("monospace");
    PangoFontDescription *fonts[] = {
        client->regular, client->italic, client->bold, client->bold_italic,
    };
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
        pango_font_description_set_family(fonts[i], family);
        pango_font_description_set_size(fonts[i],
                                        client->font_size * PANGO_SCALE);
    }
    g_free(family);
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

static void gtk_apply_config(GtkClient *client)
{
    bool saw_config = false, saw_error = false;
    for (size_t offset = 0; offset + sizeof(SnapshotRecord) <= client->snapshot.len;) {
        SnapshotRecord record;
        memcpy(&record, client->snapshot.data + offset, sizeof(record));
        offset += sizeof(record);
        if (record.size > client->snapshot.len - offset) break;
        const void *payload = client->snapshot.data + offset;
        if (record.kind == SNAPSHOT_CONFIG && record.size == sizeof(SnapshotConfig)) {
            SnapshotConfig config;
            memcpy(&config, payload, sizeof(config));
            saw_config = true;
            bool font_changed = !client->received_config
                || strcmp(client->font_family, config.font);
            if (font_changed && (!g_utf8_validate(config.font, -1, NULL)
                    || !gtk_font_available(client, config.font))) {
                char message[PATH_MAX + 256];
                snprintf(message, sizeof(message), "%s:%u\nfont = %s\n"
                    "Font is not installed on this client. Using a fallback.",
                    config.path, config.font_line, config.font);
                gtk_config_error(client, "CLIENT ERROR", message);
            }
            if (font_changed || client->font_size != (int)config.font_size) {
                snprintf(client->font_family, sizeof(client->font_family), "%s", config.font);
                client->font_size = (int)config.font_size;
                gtk_apply_font(client);
            }
            client->received_config = true;
        } else if (record.kind == SNAPSHOT_ERROR) {
            char *message = g_strndup(payload, record.size);
            if (strcmp(message, client->last_server_error))
                gtk_config_error(client, "SERVER ERROR", message);
            snprintf(client->last_server_error, sizeof(client->last_server_error), "%s", message);
            g_free(message);
            saw_error = true;
        }
        offset += record.size;
    }
    if (saw_config && !saw_error) client->last_server_error[0] = '\0';
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
