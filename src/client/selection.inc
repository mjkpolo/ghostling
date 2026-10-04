
#ifdef GMUX_CLIENT

static void gtk_selection_position(GtkClient *client, double x, double y,
                                   uint16_t *row, uint16_t *col)
{
    if (!client->render.header.rows || !client->render.header.cols) {
        *row = *col = 0;
        return;
    }
    int r = (int)((y - client->pad - client->scroll_y)
                  / client->cell_height);
    int c = ((int)x - client->pad) / client->cell_width;
    if (r < 0) r = 0;
    if (c < 0) c = 0;
    if (r >= client->render.header.rows) r = client->render.header.rows - 1;
    if (c >= client->render.header.cols) c = client->render.header.cols - 1;
    *row = (uint16_t)r;
    *col = (uint16_t)c;
}

static const SnapshotCell *cached_cell(const CachedRow *row, uint16_t col)
{
    for (uint16_t i = 0; i < row->cell_count; i++)
        if (row->cells[i].col == col) return &row->cells[i];
    return NULL;
}

static bool gtk_word_boundary(const SnapshotCell *cell)
{
    static const char *boundaries = " \t'\"│`|:;,()[]{}<>$";
    if (!cell || !cell->has_text) return true;
    gunichar value = g_utf8_get_char_validated(cell->text, -1);
    if (value == (gunichar)-1 || value == (gunichar)-2) return true;
    return g_utf8_strchr(boundaries, -1, value) != NULL;
}

static bool gtk_word_at(GtkClient *client, uint16_t row, uint16_t col,
                        uint16_t *first, uint16_t *last)
{
    const CachedRow *cached = render_client_row_const(&client->render, row);
    if (!cached) return false;
    const SnapshotCell *cell = cached_cell(cached, col);
    if (!cell || !cell->has_text) return false;
    bool boundary = gtk_word_boundary(cell);
    *first = *last = col;
    while (*first > 0) {
        const SnapshotCell *previous = cached_cell(cached, *first - 1);
        if (!previous || !previous->has_text
            || gtk_word_boundary(previous) != boundary) break;
        (*first)--;
    }
    while (*last + 1 < client->render.header.cols) {
        const SnapshotCell *next = cached_cell(cached, *last + 1);
        if (!next || !next->has_text
            || gtk_word_boundary(next) != boundary) break;
        (*last)++;
    }
    return true;
}

static bool gtk_line_at(GtkClient *client, uint16_t row,
                        uint16_t *first, uint16_t *last)
{
    const CachedRow *cached = render_client_row_const(&client->render, row);
    if (!cached) return false;
    *first = client->render.header.cols;
    *last = 0;
    for (uint16_t col = 0; col < client->render.header.cols; col++) {
        const SnapshotCell *cell = cached_cell(cached, col);
        if (!cell || !cell->has_text) continue;
        gunichar value = g_utf8_get_char_validated(cell->text, -1);
        if (value == ' ' || value == '\t' || value == 0) continue;
        if (*first == client->render.header.cols) *first = col;
        *last = col;
    }
    if (*first == client->render.header.cols) {
        *first = 0;
        *last = client->render.header.cols - 1;
    }
    return true;
}

static void gtk_selection_expand(GtkClient *client, uint16_t row,
                                 uint16_t col)
{
    ClientSelection *selection = &client->selection;
    uint16_t anchor_first, anchor_last, current_first, current_last;
    if (selection->mode == SELECTION_WORD) {
        if (!gtk_word_at(client, selection->anchor_row,
                         selection->anchor_col,
                         &anchor_first, &anchor_last)
            || !gtk_word_at(client, row, col,
                            &current_first, &current_last)) return;
    } else if (selection->mode == SELECTION_LINE) {
        if (!gtk_line_at(client, selection->anchor_row,
                         &anchor_first, &anchor_last)
            || !gtk_line_at(client, row,
                            &current_first, &current_last)) return;
    } else {
        selection->end_row = row;
        selection->end_col = col;
        selection->active = selection->start_row != row
            || selection->start_col != col;
        return;
    }

    if (row < selection->anchor_row
        || (row == selection->anchor_row && col < selection->anchor_col)) {
        selection->start_row = row;
        selection->start_col = current_first;
        selection->end_row = selection->anchor_row;
        selection->end_col = anchor_last;
    } else {
        selection->start_row = selection->anchor_row;
        selection->start_col = anchor_first;
        selection->end_row = row;
        selection->end_col = current_last;
    }
    selection->active = true;
}

static void gtk_copy_selection(GtkClient *client)
{
    if (!client->selection.active) return;
    uint16_t start_row, start_col, end_row, end_col;
    selection_bounds(&client->selection, &start_row, &start_col,
                     &end_row, &end_col);
    GString *text = g_string_new(NULL);
    if (!text) return;
    for (uint16_t row = start_row; row <= end_row; row++) {
        uint16_t first = row == start_row ? start_col : 0;
        uint16_t last = row == end_row
            ? end_col : client->render.header.cols - 1;
        gsize line_start = text->len;
        const CachedRow *cached = render_client_row_const(&client->render, row);
        if (cached) {
            for (uint16_t col = first; col <= last; col++) {
                const SnapshotCell *cell = cached_cell(cached, col);
                if (cell && cell->wide != GHOSTTY_CELL_WIDE_SPACER_TAIL
                    && cell->wide != GHOSTTY_CELL_WIDE_SPACER_HEAD
                    && cell->has_text)
                    g_string_append(text, cell->text);
                else if (!cell
                         || (cell->wide != GHOSTTY_CELL_WIDE_SPACER_TAIL
                             && cell->wide != GHOSTTY_CELL_WIDE_SPACER_HEAD))
                    g_string_append_c(text, ' ');
            }
        }
        while (text->len > line_start
               && text->str[text->len - 1] == ' ')
            g_string_truncate(text, text->len - 1);
        if (row != end_row
            && (!cached || !cached->wrapped))
            g_string_append_c(text, '\n');
    }
    g_clear_pointer(&client->pending_clipboard, g_free);
    gdk_clipboard_set_text(gtk_widget_get_clipboard(client->area), text->str);
    g_string_free(text, TRUE);
}

static void gtk_clear_selection(GtkClient *client)
{
    if (!client->selection.active && !client->selection.dragging) return;
    client->selection = (ClientSelection){0};
    gtk_widget_queue_draw(client->area);
}

static void gtk_set_pending_clipboard(GtkClient *client)
{
    if (!client->pending_clipboard) return;
    GdkClipboard *clipboard =
        client->pending_clipboard_location
            == GHOSTTY_CLIPBOARD_LOCATION_STANDARD
        ? gtk_widget_get_clipboard(client->area)
        : gtk_widget_get_primary_clipboard(client->area);
    gdk_clipboard_set_text(clipboard, client->pending_clipboard);
}

static bool snapshot_updates_selection(const RenderSnapshotWire *wire,
                                       const ClientSelection *selection)
{
    if (!selection->active) return false;
    uint16_t start_row, start_col, end_row, end_col;
    selection_bounds(selection, &start_row, &start_col, &end_row, &end_col);
    (void)start_col;
    (void)end_col;
    size_t offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) return true;
        if (record.kind == SNAPSHOT_ROW
            && record.size >= sizeof(SnapshotRow)) {
            SnapshotRow row;
            memcpy(&row, wire->data + offset, sizeof(row));
            if (row.row >= 0 && row.row >= start_row
                && row.row <= end_row) return true;
        }
        offset += record.size;
    }
    return false;
}

static void apply_snapshot_clipboard(GtkClient *client,
                                     const RenderSnapshotWire *wire)
{
    size_t offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) return;
        if (record.kind == SNAPSHOT_CLIPBOARD
            && record.size >= sizeof(SnapshotClipboard)) {
            SnapshotClipboard clipboard;
            memcpy(&clipboard, wire->data + offset, sizeof(clipboard));
            const char *data = (const char *)(wire->data + offset
                                              + sizeof(clipboard));
            if (sizeof(clipboard) + clipboard.data_len == record.size
                && g_utf8_validate(data, clipboard.data_len, NULL)) {
                g_free(client->pending_clipboard);
                client->pending_clipboard =
                    g_strndup(data, clipboard.data_len);
                client->pending_clipboard_location = clipboard.location;
                gtk_set_pending_clipboard(client);
            }
        }
        offset += record.size;
    }
}

static void gtk_paste_ready(GObject *source, GAsyncResult *result,
                            gpointer userdata)
{
    GtkClient *client = userdata;
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(
        GDK_CLIPBOARD(source), result, &error);
    if (text) {
        gtk_client_send(client, INPUT_PASTE, text, strlen(text));
        g_free(text);
    }
    if (error) g_error_free(error);
}

static void gtk_paste(GtkClient *client)
{
    gdk_clipboard_read_text_async(gtk_widget_get_clipboard(client->area),
                                  NULL, gtk_paste_ready, client);
}

#endif
