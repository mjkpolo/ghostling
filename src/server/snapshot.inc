#ifdef GMUX_SERVER
static void snapshot_kitty_images(
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
        return;

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
        if (format != GHOSTTY_KITTY_IMAGE_FORMAT_RGBA) continue;

        const uint8_t *pixels = NULL;
        size_t pixel_len = 0;
        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_PTR, &pixels);
        ghostty_kitty_graphics_image_get(handle,
            GHOSTTY_KITTY_IMAGE_DATA_DATA_LEN, &pixel_len);
        if (!pixels || pixel_len > UINT32_MAX
            || pixel_len < (size_t)image.image_w * image.image_h * 4)
            continue;
        image.pixel_len = (uint32_t)pixel_len;

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

        SnapshotRecord record = {
            SNAPSHOT_IMAGE, (uint32_t)(sizeof(image) + pixel_len)
        };
        snapshot_append(wire, &record, sizeof(record));
        snapshot_append(wire, &image, sizeof(image));
        snapshot_append(wire, pixels, pixel_len);
    }
}

static void serialize_render_snapshot(
    RenderSnapshotWire *wire, GhosttyRenderState render_state,
    GhosttyRenderStateRowIterator row_iter,
    GhosttyRenderStateRowCells cells,
    GhosttyRenderStateDirty dirty,
    const GhosttyTerminalScrollbar *scrollbar,
    GhosttyTerminal terminal,
    GhosttyKittyGraphicsPlacementIterator placement_iter,
    const ServerTheme *theme)
{
    GhosttyRenderStateColors colors =
        GHOSTTY_INIT_SIZED(GhosttyRenderStateColors);
    if (ghostty_render_state_colors_get(render_state, &colors) != GHOSTTY_SUCCESS)
        return;

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
    snapshot_record(wire, SNAPSHOT_HEADER, &header, sizeof(header));

    GhosttyKittyGraphics graphics = NULL;
    bool has_kitty = ghostty_terminal_get(terminal,
        GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS, &graphics) == GHOSTTY_SUCCESS
        && graphics != NULL;
    if (has_kitty && placement_iter)
        snapshot_kitty_images(wire, terminal, graphics, placement_iter,
            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_BG);

    if (ghostty_render_state_get(render_state,
            GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &row_iter) != GHOSTTY_SUCCESS)
        return;

    const bool send_all = dirty == GHOSTTY_RENDER_STATE_DIRTY_FULL;
    uint16_t row_index = 0;
    while (ghostty_render_state_row_iterator_next(row_iter)) {
        bool row_dirty = false;
        ghostty_render_state_row_get(row_iter,
            GHOSTTY_RENDER_STATE_ROW_DATA_DIRTY, &row_dirty);
        if (!send_all && !row_dirty) {
            row_index++;
            continue;
        }

        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells) != GHOSTTY_SUCCESS) {
            row_index++;
            continue;
        }

        // One record is one complete row replacement. Even an empty row is
        // emitted so the client clears any cells that used to be present.
        const size_t record_offset = wire->len;
        SnapshotRecord record = { SNAPSHOT_ROW, sizeof(SnapshotRow) };
        SnapshotRow row = { .row = row_index };
        GhosttyRow raw_row = 0;
        if (ghostty_render_state_row_get(row_iter,
                GHOSTTY_RENDER_STATE_ROW_DATA_RAW, &raw_row) == GHOSTTY_SUCCESS)
            ghostty_row_get(raw_row, GHOSTTY_ROW_DATA_WRAP, &row.wrapped);
        snapshot_append(wire, &record, sizeof(record));
        const size_t row_offset = wire->len;
        snapshot_append(wire, &row, sizeof(row));

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
            uint32_t grapheme_len = 0;
            ghostty_render_state_row_cells_get(cells,
                GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_LEN,
                &grapheme_len);

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

            if (grapheme_len) {
                uint32_t codepoints[16];
                uint32_t len = grapheme_len < 16 ? grapheme_len : 16;
                ghostty_render_state_row_cells_get(cells,
                    GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_BUF,
                    codepoints);
                int pos = 0;
                for (uint32_t i = 0; i < len && pos < 60; i++) {
                    char utf8[4];
                    int count = utf8_encode(codepoints[i], utf8);
                    memcpy(cell.text + pos, utf8, (size_t)count);
                    pos += count;
                }
                cell.text[pos] = 0;
                cell.has_text = true;
            }

            if (cell.has_text || cell.has_background) {
                snapshot_append(wire, &cell, sizeof(cell));
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
        row_index++;
    }

    if (has_kitty && placement_iter) {
        snapshot_kitty_images(wire, terminal, graphics, placement_iter,
            GHOSTTY_KITTY_PLACEMENT_LAYER_BELOW_TEXT);
        snapshot_kitty_images(wire, terminal, graphics, placement_iter,
            GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT);
    }

    GhosttyRenderStateDirty clean = GHOSTTY_RENDER_STATE_DIRTY_FALSE;
    ghostty_render_state_set(render_state,
        GHOSTTY_RENDER_STATE_OPTION_DIRTY, &clean);
}

#endif
