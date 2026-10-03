#ifdef GMUX_CLIENT
static int kill_server(const char *path)
{
    int fd = unix_socket(path, false);
    if (fd < 0) {
        perror("connect to Unix socket");
        return 1;
    }
    WireConnection connection = { .fd = -1 };
    InputWire command = {0};
    int result = 1;
    if (!wire_connection_init(&connection, fd)
        || !input_append(&command, INPUT_SHUTDOWN, NULL, 0)
        || !input_queue(&connection, &command))
        goto cleanup;

    while (connection.output.start < connection.output.end) {
        if (!wire_flush(&connection)) goto cleanup;
        if (connection.output.start == connection.output.end) break;
        struct pollfd ready = { .fd = connection.fd, .events = POLLOUT };
        if (poll(&ready, 1, 5000) <= 0) goto cleanup;
    }
    result = 0;

cleanup:
    free(command.data);
    wire_connection_close(&connection);
    return result;
}


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
} GtkClient;

static bool gtk_client_send(GtkClient *client, InputKind kind,
                            const void *payload, size_t size)
{
    return input_append(&client->input, kind, payload, size)
        && input_queue(&client->connection, &client->input)
        && wire_flush(&client->connection);
}

static bool register_embedded_font(const unsigned char *data, size_t len)
{
    char path[] = "/tmp/gmux-font-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return false;
    const unsigned char *cursor = data;
    size_t remaining = len;
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            close(fd);
            unlink(path);
            return false;
        }
    }
    close(fd);
    bool ok = FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
    unlink(path);
    return ok;
}

static void gtk_load_config(GtkClient *client)
{
    snprintf(client->font_family, sizeof(client->font_family),
             "Monaspace Argon Frozen");
    client->font_size = 24;
    char *path = g_build_filename(g_get_user_config_dir(), "gmux", "config",
                                  NULL);
    FILE *file = fopen(path, "r");
    g_free(path);
    if (!file) return;

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        char *value = strchr(line, '=');
        if (!value) continue;
        *value++ = '\0';
        g_strstrip(line);
        g_strstrip(value);
        if (strcmp(line, "font") == 0 && *value) {
            snprintf(client->font_family, sizeof(client->font_family),
                     "%s", value);
        } else if (strcmp(line, "font-size") == 0) {
            char *end = NULL;
            long size = strtol(value, &end, 10);
            if (end != value && *end == '\0' && size >= 6 && size <= 96)
                client->font_size = (int)size;
        }
    }
    fclose(file);
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
