#ifdef GMUX_SERVER
static bool snapshot_kitty_images(
    RenderSnapshotWire *wire, GhosttyTerminal terminal,
    GhosttyKittyGraphics graphics,
    GhosttyKittyGraphicsPlacementIterator placement_iter,
    GhosttyKittyPlacementLayer layer)
{
    ghostty_kitty_graphics_placement_iterator_set(placement_iter,
        GHOSTTY_KITTY_GRAPHICS_PLACEMENT_ITERATOR_OPTION_LAYER, &layer);
    if (ghostty_kitty_graphics_get(graphics,
            GHOSTTY_KITTY_GRAPHICS_DATA_PLACEMENT_ITERATOR,
            &placement_iter) != GHOSTTY_SUCCESS)
        return false;

    while (ghostty_kitty_graphics_placement_next(placement_iter)) {
        uint32_t image_id = 0;
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_IMAGE_ID, &image_id);
        GhosttyKittyGraphicsImage handle =
            ghostty_kitty_graphics_image(graphics, image_id);
        if (!handle) continue;

        SnapshotImage image = { .layer = (int32_t)layer };
        if (ghostty_kitty_graphics_placement_viewport_pos(
                placement_iter, handle, terminal,
                &image.viewport_col, &image.viewport_row) != GHOSTTY_SUCCESS)
            continue;

        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_WIDTH, &image.image_w);
        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_HEIGHT, &image.image_h);
        if (!image.image_w || !image.image_h) continue;

        GhosttyKittyImageFormat format = GHOSTTY_KITTY_IMAGE_FORMAT_RGBA;
        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_FORMAT, &format);
        if (format != GHOSTTY_KITTY_IMAGE_FORMAT_RGBA
            && format != GHOSTTY_KITTY_IMAGE_FORMAT_RGB) continue;

        const uint8_t *pixels = NULL;
        size_t pixel_len = 0;
        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, &pixels);
        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &pixel_len);
        uint64_t count = (uint64_t)image.image_w * image.image_h;
        unsigned components = format == GHOSTTY_KITTY_IMAGE_FORMAT_RGB ? 3 : 4;
        if (!pixels || count > (UINT32_MAX - sizeof(image)) / 4
            || pixel_len != count * components)
            continue;
        image.pixel_len = (uint32_t)(count * 4);

        if (ghostty_kitty_graphics_placement_grid_size(
                placement_iter, handle, terminal,
                &image.grid_cols, &image.grid_rows) != GHOSTTY_SUCCESS
            || !image.grid_cols || !image.grid_rows)
            continue;
        if (ghostty_kitty_graphics_placement_source_rect(
                placement_iter, handle, &image.src_x, &image.src_y,
                &image.src_w, &image.src_h) != GHOSTTY_SUCCESS)
            continue;
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_X_OFFSET, &image.x_offset);
        ghostty_kitty_graphics_placement_get(placement_iter,
            GHOSTTY_KITTY_GRAPHICS_PLACEMENT_DATA_Y_OFFSET, &image.y_offset);

        // Kitty f=24 stays RGB in libghostty; the wire uses one RGBA format
        // for both raw Kitty images and decoded PNGs.
        uint8_t *rgba = NULL;
        if (components == 3) {
            rgba = malloc(image.pixel_len);
            if (!rgba) return false;
            for (size_t i = 0; i < count; i++) {
                memcpy(rgba + 4 * i, pixels + 3 * i, 3);
                rgba[4 * i + 3] = 255;
            }
            pixels = rgba;
        }
        SnapshotRecord record = { SNAPSHOT_IMAGE, sizeof(image) + image.pixel_len };
        bool ok = snapshot_append(wire, &record, sizeof(record))
            && snapshot_append(wire, &image, sizeof(image))
            && snapshot_append(wire, pixels, image.pixel_len);
        free(rgba);
        if (!ok) return false;
    }
    return true;
}

static bool serialize_render_snapshot(
    RenderSnapshotWire *wire, GhosttyRenderState render_state,
    GhosttyRenderStateRowIterator row_iter,
    GhosttyRenderStateRowCells cells,
    GhosttyRenderStateDirty dirty,
    const GhosttyTerminalScrollbar *scrollbar,
    GhosttyTerminal terminal,
    GhosttyKittyGraphicsPlacementIterator placement_iter,
    const ServerTheme *theme, uint32_t scroll_serial)
{
    GhosttyRenderStateColors colors =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_COLORS, &colors) != GHOSTTY_SUCCESS)
        return false;

    SnapshotHeader header = {
        .background = snapshot_rgb(colors.background),
        .cursor_color = snapshot_rgb(
            colors.cursor_has_value ? colors.cursor : colors.foreground),
        .has_selection_background = theme->has_selection_background,
        .has_selection_foreground = theme->has_selection_foreground,
    };
    if (theme->has_selection_background)
        header.selection_background = snapshot_rgb(
            theme->selection_background);
    if (theme->has_selection_foreground)
        header.selection_foreground = snapshot_rgb(
            theme->selection_foreground);
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_ROWS, &header.rows);
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_COLS, &header.cols);
    ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_MOUSE_TRACKING,
                         &header.mouse_tracking);
    GhosttyTerminalScreen screen = GHOSTTY_TERMINAL_SCREEN_PRIMARY;
    ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_ACTIVE_SCREEN,
                         &screen);
    ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_VIEWPORT_ACTIVE,
                         &header.viewport_active);
    header.viewport_scrollable =
        screen == GHOSTTY_TERMINAL_SCREEN_PRIMARY && !header.mouse_tracking;
    header.scroll_serial = scroll_serial;
    GhosttyRenderStateOverscan overscan = {0};
    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_OVERSCAN, &overscan) == GHOSTTY_SUCCESS) {
        header.overscan_above = overscan.above;
        header.overscan_below = overscan.below;
    }

    bool cursor_in_viewport = false;
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VISIBLE, &header.cursor_visible);
    ghostty_render_state_get(render_state,
        GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_HAS_VALUE,
        &cursor_in_viewport);
    header.cursor_visible &= cursor_in_viewport;
    if (header.cursor_visible) {
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_X, &header.cursor_col);
        ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_CURSOR_VIEWPORT_Y, &header.cursor_row);
    }
    if (scrollbar && scrollbar->total > scrollbar->len) {
        header.scrollbar_visible = true;
        header.scrollbar_total = scrollbar->total;
        header.scrollbar_len = scrollbar->len;
        header.scrollbar_offset = scrollbar->offset;
    }
    if (!snapshot_record(wire, SNAPSHOT_HEADER, &header, sizeof(header)))
        return false;

    GhosttyKittyGraphics graphics = NULL;
    bool has_kitty = ghostty_terminal_get(terminal,
        GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics) == GHOSTTY_SUCCESS
        && graphics != NULL;
    if (has_kitty && placement_iter
        && !snapshot_kitty_images(wire, terminal, graphics, placement_iter,
            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_BG)) return false;

    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter) != GHOSTTY_SUCCESS)
        return false;

    const bool send_all = dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL;
    while (ghostty_render_state_row_iterator_next(row_iter)) {
        bool row_dirty = false;
        ghostty_render_state_row_get(row_iter,
            GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
        if (!send_all && !row_dirty) {
            continue;
        }

        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells) != GHOSTTY_SUCCESS) {
            return false;
        }

        // One record is one complete row replacement. Even an empty row is
        // emitted so the client clears any cells that used to be present.
        const size_t record_offset = wire->len;
        SnapshotRecord record = { SNAPSHOT_ROW, sizeof(SnapshotRow) };
        SnapshotRow row = {0};
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_VIEWPORT_Y,
                &row.row) != GHOSTTY_SUCCESS)
            return false;
        GhosttyRow raw_row = 0;
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &raw_row) == GHOSTTY_SUCCESS)
            ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_WRAP, &row.wrapped);
        if (!snapshot_append(wire, &record, sizeof(record))) return false;
        const size_t row_offset = wire->len;
        if (!snapshot_append(wire, &row, sizeof(row))) return false;

        uint16_t col = 0;
        while (ghostty_render_state_row_cells_next(cells)) {
            SnapshotCell cell = {
                .col = col,
                .foreground = snapshot_rgb(colors.foreground),
                .background = snapshot_rgb(colors.background),
            };
            GhosttyCell raw_cell = 0;
            GhosttyCellWide wide = GHOSTTY_CELL_WIDE_NARROW;
            if (ghostty_render_state_row_cells_get(cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_RAW,
                    &raw_cell) == GHOSTTY_SUCCESS)
                ghostty_cell_get(raw_cell, GHOSTTY_CELL_DATA_WIDE, &wide);
            cell.wide = (uint8_t)wide;
            GhosttyColorRgb fg = colors.foreground;
            GhosttyColorRgb bg = colors.background;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &fg);
            cell.has_background = ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &bg)
                == GHOSTTY_SUCCESS;

            GhosttyStyle style = GHOSTTY_INIT_SIZED(GhosttyStyle);
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE, &style);
            if (style.inverse) {
                GhosttyColorRgb swap = fg;
                fg = bg;
                bg = swap;
                cell.has_background = true;
            }
            cell.foreground = snapshot_rgb(fg);
            cell.background = snapshot_rgb(bg);
            cell.bold = style.bold;
            cell.italic = style.italic;
            cell.faint = style.faint;
            cell.blink = style.blink;
            cell.inverse = style.inverse;
            cell.invisible = style.invisible;
            cell.strikethrough = style.strikethrough;
            cell.overline = style.overline;
            cell.underline = (uint8_t)style.underline;
            cell.has_underline_color = snapshot_style_color(
                style.underline_color, &colors, &cell.underline_color);

            // Let libghostty encode the cluster with a checked capacity. The
            // codepoint-buffer API copies the entire cluster, not just 16
            // codepoints, so using it with a fixed array could overflow.
            GhosttyBuffer text = {
                .ptr = (uint8_t *)cell.text, .cap = sizeof(cell.text) - 1,
            };
            GhosttyResult text_result = ghostty_render_state_row_cells_get(
                cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8,
                &text);
            if (text_result == GHOSTTY_OUT_OF_SPACE)
                memcpy(cell.text, "\xef\xbf\xbd", 4);
            else if (text_result != GHOSTTY_SUCCESS)
                return false;
            cell.has_text = cell.text[0] != '\0';

            if (cell.has_text || cell.has_background) {
                if (!snapshot_append(wire, &cell, sizeof(cell))) return false;
                row.cell_count++;
            }
            col++;
        }

        record.size = (uint32_t)(sizeof(row)
            + (size_t)row.cell_count * sizeof(SnapshotCell));
        memcpy(wire->data + record_offset, &record, sizeof(record));
        memcpy(wire->data + row_offset, &row, sizeof(row));

        bool clean = false;
        ghostty_render_state_row_set(row_iter,
            GHOSTTY_RENDER_STATE_ROW_OPTION_DIRTY, &clean);
    }

    if (has_kitty && placement_iter) {
        if (!snapshot_kitty_images(wire, terminal, graphics, placement_iter,
                GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT)
            || !snapshot_kitty_images(wire, terminal, graphics, placement_iter,
                GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT)) return false;
    }

    GhosttyRenderStateDirty clean = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(render_state,
        GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean);
    return true;
}

#endif
