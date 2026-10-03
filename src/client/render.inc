#ifdef GMUX_CLIENT
static bool render_client_resize(RenderClientState *client, uint16_t rows)
{
    if (rows == client->row_count)
        return true;
    if (rows < client->row_count) {
        for (uint16_t i = rows; i < client->row_count; i++)
            free(client->rows[i].cells);
    }
    CachedRow *next = realloc(client->rows, (size_t)rows * sizeof(*next));
    if (rows && !next)
        return false;
    if (rows > client->row_count)
        memset(next + client->row_count, 0,
               (size_t)(rows - client->row_count) * sizeof(*next));
    client->rows = next;
    client->row_count = rows;
    return true;
}

static CachedRow *render_client_row(RenderClientState *client,
                                    int32_t viewport_y)
{
    int32_t index = viewport_y + client->overscan_above;
    return index >= 0 && index < client->row_count
        ? &client->rows[index] : NULL;
}

static const CachedRow *render_client_row_const(const RenderClientState *client,
                                                int32_t viewport_y)
{
    int32_t index = viewport_y + client->overscan_above;
    return index >= 0 && index < client->row_count
        ? &client->rows[index] : NULL;
}

static void render_client_apply(RenderClientState *client,
                                const RenderSnapshotWire *wire)
{
    const SnapshotHeader *header = snapshot_header(wire);
    if (!header) return;
    uint32_t total = (uint32_t)header->rows
        + header->overscan_above + header->overscan_below;
    if (total > UINT16_MAX) return;
    if ((client->overscan_above != header->overscan_above
         || client->overscan_below != header->overscan_below)) {
        render_client_resize(client, 0);
        client->overscan_above = header->overscan_above;
        client->overscan_below = header->overscan_below;
    }
    if (!render_client_resize(client, (uint16_t)total))
        return;
    client->header = *header;

    size_t offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len)
            return;
        if (record.kind == SNAPSHOT_ROW && record.size >= sizeof(SnapshotRow)) {
            SnapshotRow row;
            memcpy(&row, wire->data + offset, sizeof(row));
            size_t expected = sizeof(row)
                + (size_t)row.cell_count * sizeof(SnapshotCell);
            CachedRow *cached = render_client_row(client, row.row);
            if (cached && expected == record.size) {
                SnapshotCell *cells = realloc(cached->cells,
                    (size_t)row.cell_count * sizeof(*cells));
                if (row.cell_count == 0 || cells) {
                    cached->cells = cells;
                    cached->cell_count = row.cell_count;
                    cached->wrapped = row.wrapped;
                    if (row.cell_count)
                        memcpy(cached->cells,
                               wire->data + offset + sizeof(row),
                               (size_t)row.cell_count * sizeof(*cells));
                }
            }
        }
        offset += record.size;
    }
}

static void apply_snapshot_title(GtkWindow *window,
                                 const RenderSnapshotWire *wire)
{
    size_t offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len)
            return;
        if (record.kind == SNAPSHOT_TITLE) {
            char title[256];
            size_t len = record.size < sizeof(title) - 1
                ? record.size : sizeof(title) - 1;
            memcpy(title, wire->data + offset, len);
            title[len] = '\0';
            gtk_window_set_title(window, title);
        }
        offset += record.size;
    }
}

static void render_client_free(RenderClientState *client)
{
    for (uint16_t i = 0; i < client->row_count; i++)
        free(client->rows[i].cells);
    free(client->rows);
    *client = (RenderClientState){0};
}


static void cairo_color(cairo_t *cr, SnapshotRgb color, double alpha)
{
    cairo_set_source_rgba(cr, color.r / 255.0, color.g / 255.0,
                         color.b / 255.0, alpha);
}

static void cairo_cell_decorations(cairo_t *cr, const SnapshotCell *cell,
                                   double x, double y,
                                   int cell_width, int cell_height,
                                   double alpha)
{
    SnapshotRgb color = cell->has_underline_color
        ? cell->underline_color : cell->foreground;
    cairo_save(cr);
    cairo_color(cr, color, alpha);
    cairo_set_line_width(cr, 1.0);

    if (cell->underline) {
        double baseline = y + cell_height - 2.0;
        if (cell->underline == GHOSTTY_SGR_UNDERLINE_CURLY) {
            double step = cell_width / 4.0;
            cairo_move_to(cr, x, baseline);
            cairo_curve_to(cr, x + step, baseline - 2.0,
                           x + step, baseline + 2.0,
                           x + 2.0 * step, baseline);
            cairo_curve_to(cr, x + 3.0 * step, baseline - 2.0,
                           x + 3.0 * step, baseline + 2.0,
                           x + cell_width, baseline);
        } else {
            if (cell->underline == GHOSTTY_SGR_UNDERLINE_DOTTED) {
                const double dash[] = { 1.0, 2.0 };
                cairo_set_dash(cr, dash, 2, 0);
            } else if (cell->underline == GHOSTTY_SGR_UNDERLINE_DASHED) {
                const double dash[] = { 3.0, 2.0 };
                cairo_set_dash(cr, dash, 2, 0);
            }
            cairo_move_to(cr, x, baseline);
            cairo_line_to(cr, x + cell_width, baseline);
            if (cell->underline == GHOSTTY_SGR_UNDERLINE_DOUBLE) {
                cairo_move_to(cr, x, baseline - 2.0);
                cairo_line_to(cr, x + cell_width, baseline - 2.0);
            }
        }
        cairo_stroke(cr);
        cairo_set_dash(cr, NULL, 0, 0);
    }
    if (cell->strikethrough) {
        cairo_move_to(cr, x, y + cell_height * 0.5);
        cairo_line_to(cr, x + cell_width, y + cell_height * 0.5);
        cairo_stroke(cr);
    }
    if (cell->overline) {
        cairo_move_to(cr, x, y + 1.0);
        cairo_line_to(cr, x + cell_width, y + 1.0);
        cairo_stroke(cr);
    }
    cairo_restore(cr);
}

static bool snapshot_rgb_equal(SnapshotRgb a, SnapshotRgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static bool selection_contains_cell(const ClientSelection *selection,
                                    uint16_t row, uint16_t col)
{
    if (!selection->active) return false;
    uint16_t start_row = selection->start_row;
    uint16_t start_col = selection->start_col;
    uint16_t end_row = selection->end_row;
    uint16_t end_col = selection->end_col;
    if (start_row > end_row
        || (start_row == end_row && start_col > end_col)) {
        uint16_t swap_row = start_row, swap_col = start_col;
        start_row = end_row;
        start_col = end_col;
        end_row = swap_row;
        end_col = swap_col;
    }
    if (row < start_row || row > end_row) return false;
    if (row == start_row && col < start_col) return false;
    if (row == end_row && col > end_col) return false;
    return true;
}

// Return whether Pango may shape two neighboring cells as one text run.
// Backgrounds intentionally do not participate: they are painted in a
// separate pass, and allowing them to differ preserves shaping across syntax
// highlighting that changes only the background.
static bool cells_shape_together(const SnapshotCell *a,
                                 const SnapshotCell *b)
{
    return a->has_text && b->has_text
        && !a->invisible && !b->invisible
        && b->col == a->col + 1
        && snapshot_rgb_equal(a->foreground, b->foreground)
        && a->bold == b->bold
        && a->italic == b->italic
        && a->faint == b->faint
        && a->blink == b->blink
        && a->inverse == b->inverse
        && a->strikethrough == b->strikethrough
        && a->overline == b->overline
        && a->underline == b->underline
        && a->has_underline_color == b->has_underline_color
        && (!a->has_underline_color
            || snapshot_rgb_equal(a->underline_color, b->underline_color));
}

static void cairo_snapshot_cells(cairo_t *cr,
                                 const RenderClientState *client,
                                 const ClientSelection *selection,
                                 PangoLayout *layout,
                                 PangoFontDescription *regular,
                                 PangoFontDescription *italic,
                                 PangoFontDescription *bold,
                                 PangoFontDescription *bold_italic,
                                 int cell_width, int cell_height,
                                 int pad, double scroll_y, bool text_pass)
{
    for (uint16_t index = 0; index < client->row_count; index++) {
        int32_t viewport_y = (int32_t)index - client->overscan_above;
        const CachedRow *cached = &client->rows[index];
        if (!text_pass) {
            for (uint16_t i = 0; i < cached->cell_count; i++) {
                const SnapshotCell *cell = &cached->cells[i];
                if (!cell->has_background) continue;
                double x = pad + cell->col * cell_width;
                double y = pad + viewport_y * cell_height + scroll_y;
                cairo_color(cr, cell->background, 1.0);
                cairo_rectangle(cr, x, y, cell_width, cell_height);
                cairo_fill(cr);
            }
            continue;
        }

        // Shape a compatible sequence once so HarfBuzz can apply ligatures
        // and contextual alternates such as Monaspace texture healing. The
        // logical terminal grid remains cell based; only glyph ink may cross
        // the internal boundaries of this run.
        uint16_t i = 0;
        while (i < cached->cell_count) {
            const SnapshotCell *first = &cached->cells[i];
            if (!first->has_text || first->invisible) {
                i++;
                continue;
            }
            uint16_t end = i + 1;
            while (end < cached->cell_count
                   && cells_shape_together(&cached->cells[end - 1],
                                           &cached->cells[end])) {
                if (client->header.has_selection_foreground
                    && viewport_y >= 0
                    && selection_contains_cell(selection, viewport_y,
                                               cached->cells[end - 1].col)
                        != selection_contains_cell(selection, viewport_y,
                                                   cached->cells[end].col))
                    break;
                // Keep the cursor cell isolated so moving the cursor through
                // a ligature exposes the underlying terminal characters.
                if (client->header.cursor_visible
                    && client->header.cursor_row == viewport_y
                    && (cached->cells[end - 1].col
                            == client->header.cursor_col
                        || cached->cells[end].col
                            == client->header.cursor_col))
                    break;
                end++;
            }

            GString *text = g_string_sized_new(
                (gsize)(end - i) * sizeof(first->text));
            if (!text) return;
            for (uint16_t j = i; j < end; j++)
                g_string_append(text, cached->cells[j].text);

            double x = pad + first->col * cell_width;
            double y = pad + viewport_y * cell_height + scroll_y;
            double alpha = first->faint ? 0.5 : 1.0;
            PangoFontDescription *font = first->bold
                ? (first->italic ? bold_italic : bold)
                : (first->italic ? italic : regular);
            pango_layout_set_font_description(layout, font);
            pango_layout_set_text(layout, text->str, (int)text->len);
            SnapshotRgb foreground =
                client->header.has_selection_foreground
                && viewport_y >= 0
                && selection_contains_cell(selection, viewport_y, first->col)
                ? client->header.selection_foreground : first->foreground;
            cairo_color(cr, foreground, alpha);
            cairo_move_to(cr, x, y);
            pango_cairo_show_layout(cr, layout);
            g_string_free(text, TRUE);

            for (uint16_t j = i; j < end; j++) {
                const SnapshotCell *cell = &cached->cells[j];
                SnapshotCell selected = *cell;
                if (client->header.has_selection_foreground
                    && viewport_y >= 0
                    && selection_contains_cell(selection, viewport_y,
                                               cell->col)) {
                    selected.foreground =
                        client->header.selection_foreground;
                    selected.has_underline_color = false;
                    cell = &selected;
                }
                double cell_x = pad + cell->col * cell_width;
                cairo_cell_decorations(cr, cell, cell_x, y,
                                       cell_width, cell_height, alpha);
            }
            i = end;
        }
    }
}

// Convert libghostty's RGBA pixels to Cairo's native premultiplied ARGB32.
// This is intentionally a correctness-first bridge; a later GTK-hosted GPU
// renderer can cache/upload images without changing the wire representation.
static void cairo_snapshot_images(cairo_t *cr,
                                  const RenderSnapshotWire *wire,
                                  GhosttyKittyPlacementLayer layer,
                                  int cell_width, int cell_height, int pad,
                                  double scroll_y)
{
    size_t offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) return;
        if (record.kind == SNAPSHOT_IMAGE
            && record.size >= sizeof(SnapshotImage)) {
            SnapshotImage image;
            memcpy(&image, wire->data + offset, sizeof(image));
            const uint8_t *rgba = wire->data + offset + sizeof(image);
            size_t pixels = (size_t)image.image_w * image.image_h;
            if (image.layer == (int32_t)layer
                && image.src_w && image.src_h
                && sizeof(image) + image.pixel_len <= record.size
                && image.pixel_len >= pixels * 4) {
                int stride = cairo_format_stride_for_width(
                    CAIRO_FORMAT_ARGB32, (int)image.image_w);
                uint8_t *argb = calloc((size_t)stride, image.image_h);
                if (argb) {
                    for (uint32_t y = 0; y < image.image_h; y++) {
                        uint32_t *dst = (uint32_t *)(argb + (size_t)y * stride);
                        for (uint32_t x = 0; x < image.image_w; x++) {
                            const uint8_t *src = rgba + ((size_t)y * image.image_w + x) * 4;
                            uint32_t a = src[3];
                            dst[x] = (a << 24)
                                | ((src[0] * a / 255) << 16)
                                | ((src[1] * a / 255) << 8)
                                | (src[2] * a / 255);
                        }
                    }
                    cairo_surface_t *surface = cairo_image_surface_create_for_data(
                        argb, CAIRO_FORMAT_ARGB32, image.image_w,
                        image.image_h, stride);
                    double dx = pad + image.viewport_col * cell_width + image.x_offset;
                    double dy = pad + image.viewport_row * cell_height
                        + image.y_offset + scroll_y;
                    double dw = image.grid_cols * cell_width;
                    double dh = image.grid_rows * cell_height;
                    cairo_save(cr);
                    cairo_rectangle(cr, dx, dy, dw, dh);
                    cairo_clip(cr);
                    cairo_translate(cr, dx, dy);
                    cairo_scale(cr, dw / image.src_w, dh / image.src_h);
                    cairo_set_source_surface(cr, surface,
                                             -(double)image.src_x,
                                             -(double)image.src_y);
                    cairo_pattern_set_filter(cairo_get_source(cr),
                                             CAIRO_FILTER_BILINEAR);
                    cairo_paint(cr);
                    cairo_restore(cr);
                    cairo_surface_destroy(surface);
                    free(argb);
                }
            }
        }
        offset += record.size;
    }
}

enum { SELECTION_CELL = 1, SELECTION_WORD, SELECTION_LINE };

static void selection_bounds(const ClientSelection *selection,
                             uint16_t *start_row, uint16_t *start_col,
                             uint16_t *end_row, uint16_t *end_col)
{
    *start_row = selection->start_row;
    *start_col = selection->start_col;
    *end_row = selection->end_row;
    *end_col = selection->end_col;
    if (*start_row > *end_row
        || (*start_row == *end_row && *start_col > *end_col)) {
        uint16_t row = *start_row, col = *start_col;
        *start_row = *end_row;
        *start_col = *end_col;
        *end_row = row;
        *end_col = col;
    }
}

static void cairo_selection(cairo_t *cr, const ClientSelection *selection,
                            const SnapshotHeader *header,
                            int cell_width, int cell_height, int pad,
                            double scroll_y)
{
    if (!selection->active || !header->cols) return;
    uint16_t start_row, start_col, end_row, end_col;
    selection_bounds(selection, &start_row, &start_col, &end_row, &end_col);
    if (header->has_selection_background)
        cairo_color(cr, header->selection_background, 1.0);
    else
        cairo_set_source_rgba(cr, 0.35, 0.55, 0.95, 0.45);
    for (uint16_t row = start_row; row <= end_row; row++) {
        uint16_t first = row == start_row ? start_col : 0;
        uint16_t last = row == end_row ? end_col : header->cols - 1;
        cairo_rectangle(cr, pad + first * cell_width,
                        pad + row * cell_height + scroll_y,
                        (last - first + 1) * cell_width, cell_height);
    }
    cairo_fill(cr);
}

static void cairo_render_snapshot(cairo_t *cr,
                                  const RenderClientState *client,
                                  const RenderSnapshotWire *wire,
                                  const ClientSelection *selection,
                                  PangoFontDescription *regular,
                                  PangoFontDescription *italic,
                                  PangoFontDescription *bold,
                                  PangoFontDescription *bold_italic,
                                  int cell_width, int cell_height,
                                  int pad, int width, int height,
                                  double scroll_y)
{
    const SnapshotHeader *header = &client->header;
    cairo_color(cr, header->background, 1.0);
    cairo_paint(cr);
    PangoLayout *layout = pango_cairo_create_layout(cr);
    cairo_save(cr);
    cairo_rectangle(cr, pad, pad, header->cols * cell_width,
                    header->rows * cell_height);
    cairo_clip(cr);

    cairo_snapshot_images(cr, wire, GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_BG,
                          cell_width, cell_height, pad, scroll_y);
    cairo_snapshot_cells(cr, client, selection, layout, regular, italic,
                         bold, bold_italic,
                         cell_width, cell_height, pad, scroll_y, false);
    cairo_selection(cr, selection, header, cell_width, cell_height, pad,
                    scroll_y);
    cairo_snapshot_images(cr, wire, GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT,
                          cell_width, cell_height, pad, scroll_y);
    cairo_snapshot_cells(cr, client, selection, layout, regular, italic,
                         bold, bold_italic,
                         cell_width, cell_height, pad, scroll_y, true);
    if (header->cursor_visible) {
        cairo_color(cr, header->cursor_color, 0.5);
        cairo_rectangle(cr, pad + header->cursor_col * cell_width,
                        pad + header->cursor_row * cell_height + scroll_y,
                        cell_width, cell_height);
        cairo_fill(cr);
    }
    cairo_snapshot_images(cr, wire, GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT,
                          cell_width, cell_height, pad, scroll_y);
    cairo_restore(cr);

    if (header->scrollbar_visible
        && header->scrollbar_total > header->scrollbar_len) {
        int thumb_height = (int)(height
            * ((double)header->scrollbar_len / header->scrollbar_total));
        if (thumb_height < 10) thumb_height = 10;
        double fraction = (double)header->scrollbar_offset
            / (header->scrollbar_total - header->scrollbar_len);
        cairo_set_source_rgba(cr, 0.78, 0.78, 0.78, 0.5);
        cairo_rectangle(cr, width - 8,
                        fraction * (height - thumb_height), 6, thumb_height);
        cairo_fill(cr);
    }
    g_object_unref(layout);
}

#endif
