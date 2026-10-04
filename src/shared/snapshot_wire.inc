// Semantic render snapshot. The producer copies libghostty's render values;
// the client performs toolkit-specific layout and drawing.
typedef struct { uint8_t *data; size_t len, cap; } RenderSnapshotWire;
typedef enum {
    SNAPSHOT_HEADER = 1,
    SNAPSHOT_ROW = 2,
    SNAPSHOT_IMAGE = 3,
    SNAPSHOT_TITLE = 4,
    SNAPSHOT_CLIPBOARD = 5,
    SNAPSHOT_PONG = 6,
} SnapshotRecordKind;
typedef struct { uint32_t kind, size; } SnapshotRecord;
typedef struct { uint8_t r, g, b; } SnapshotRgb;
typedef struct {
    uint16_t rows, cols;
    SnapshotRgb background;
    SnapshotRgb selection_background, selection_foreground;
    bool has_selection_background, has_selection_foreground;
    bool mouse_tracking;
    bool viewport_scrollable;
    bool viewport_active;
    bool cursor_visible;
    uint16_t cursor_col, cursor_row;
    SnapshotRgb cursor_color;
    bool scrollbar_visible;
    uint64_t scrollbar_total, scrollbar_len, scrollbar_offset;
    uint32_t scroll_serial;
    uint16_t overscan_above, overscan_below;
} SnapshotHeader;
typedef struct {
    int32_t row;
    uint16_t cell_count;
    bool wrapped;
} SnapshotRow;
typedef struct {
    uint16_t col;
    SnapshotRgb foreground, background;
    SnapshotRgb underline_color;
    bool has_text, has_background, has_underline_color;
    bool bold, italic, faint, blink, inverse, invisible;
    bool strikethrough, overline;
    uint8_t underline;
    uint8_t wide;
    char text[64];
} SnapshotCell;
typedef struct {
    SnapshotCell *cells;
    uint16_t cell_count;
    bool wrapped;
} CachedRow;
typedef struct {
    SnapshotHeader header;
    CachedRow *rows;
    uint16_t row_count;
    uint16_t overscan_above, overscan_below;
} RenderClientState;
typedef struct {
    bool active, dragging;
    uint16_t start_row, start_col;
    uint16_t end_row, end_col;
    uint16_t anchor_row, anchor_col;
    uint8_t mode;
} ClientSelection;
typedef struct {
    int32_t viewport_col, viewport_row;
    uint32_t x_offset, y_offset;
    uint32_t grid_cols, grid_rows;
    uint32_t image_w, image_h;
    uint32_t src_x, src_y, src_w, src_h;
    uint32_t pixel_len;
    int32_t layer;
} SnapshotImage;
typedef struct {
    uint32_t location;
    uint32_t data_len;
} SnapshotClipboard;

static bool snapshot_append(RenderSnapshotWire *wire,
                            const void *data, size_t len);
static bool snapshot_record(RenderSnapshotWire *wire, uint32_t kind,
                            const void *payload, size_t payload_len);

#ifdef GMUX_SERVER
static void pack_rgb(msgpack_packer *packer, SnapshotRgb color)
{
    msgpack_pack_array(packer, 3);
    msgpack_pack_uint8(packer, color.r);
    msgpack_pack_uint8(packer, color.g);
    msgpack_pack_uint8(packer, color.b);
}
#endif

#ifdef GMUX_CLIENT
static bool unpack_rgb(msgpack_object object, SnapshotRgb *color)
{
    if (object.type != MSGPACK_OBJECT_ARRAY || object.via.array.size != 3)
        return false;
    uint64_t value[3];
    for (int i = 0; i < 3; i++)
        if (!object_u64(object.via.array.ptr[i], &value[i]) || value[i] > 255)
            return false;
    *color = (SnapshotRgb){
        (uint8_t)value[0], (uint8_t)value[1], (uint8_t)value[2]
    };
    return true;
}
#endif

#ifdef GMUX_SERVER
static size_t snapshot_record_count(const RenderSnapshotWire *wire)
{
    size_t count = 0, offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) break;
        offset += record.size;
        count++;
    }
    return count;
}

static bool snapshot_pack(const RenderSnapshotWire *wire,
                          msgpack_sbuffer *buffer)
{
    WirePacker output = { .buffer = buffer };
    msgpack_packer packer;
    msgpack_packer_init(&packer, &output, wire_pack_write);
    msgpack_pack_array(&packer, snapshot_record_count(wire));
    size_t offset = 0;
    while (offset + sizeof(SnapshotRecord) <= wire->len) {
        SnapshotRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) return false;
        const uint8_t *payload = wire->data + offset;

        if (record.kind == SNAPSHOT_HEADER
            && record.size == sizeof(SnapshotHeader)) {
            SnapshotHeader header;
            memcpy(&header, payload, sizeof(header));
            msgpack_pack_array(&packer, 22);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_uint16(&packer, header.rows);
            msgpack_pack_uint16(&packer, header.cols);
            pack_rgb(&packer, header.background);
            header.mouse_tracking ? msgpack_pack_true(&packer)
                                  : msgpack_pack_false(&packer);
            header.cursor_visible ? msgpack_pack_true(&packer)
                                  : msgpack_pack_false(&packer);
            msgpack_pack_uint16(&packer, header.cursor_col);
            msgpack_pack_uint16(&packer, header.cursor_row);
            pack_rgb(&packer, header.cursor_color);
            header.scrollbar_visible ? msgpack_pack_true(&packer)
                                     : msgpack_pack_false(&packer);
            msgpack_pack_uint64(&packer, header.scrollbar_total);
            msgpack_pack_uint64(&packer, header.scrollbar_len);
            msgpack_pack_uint64(&packer, header.scrollbar_offset);
            pack_rgb(&packer, header.selection_background);
            pack_rgb(&packer, header.selection_foreground);
            header.has_selection_background
                ? msgpack_pack_true(&packer) : msgpack_pack_false(&packer);
            header.has_selection_foreground
                ? msgpack_pack_true(&packer) : msgpack_pack_false(&packer);
            header.viewport_scrollable ? msgpack_pack_true(&packer)
                                       : msgpack_pack_false(&packer);
            header.viewport_active ? msgpack_pack_true(&packer)
                                   : msgpack_pack_false(&packer);
            msgpack_pack_uint32(&packer, header.scroll_serial);
            msgpack_pack_uint16(&packer, header.overscan_above);
            msgpack_pack_uint16(&packer, header.overscan_below);
        } else if (record.kind == SNAPSHOT_ROW
                   && record.size >= sizeof(SnapshotRow)) {
            SnapshotRow row;
            memcpy(&row, payload, sizeof(row));
            size_t expected = sizeof(row)
                + (size_t)row.cell_count * sizeof(SnapshotCell);
            if (expected != record.size) return false;
            msgpack_pack_array(&packer, 4);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_int32(&packer, row.row);
            row.wrapped ? msgpack_pack_true(&packer)
                        : msgpack_pack_false(&packer);
            msgpack_pack_array(&packer, row.cell_count);
            const SnapshotCell *cells =
                (const SnapshotCell *)(payload + sizeof(row));
            for (uint16_t i = 0; i < row.cell_count; i++) {
                const SnapshotCell *cell = &cells[i];
                size_t text_len = strnlen(cell->text, sizeof(cell->text));
                msgpack_pack_array(&packer, 18);
                msgpack_pack_uint16(&packer, cell->col);
                pack_rgb(&packer, cell->foreground);
                pack_rgb(&packer, cell->background);
                pack_rgb(&packer, cell->underline_color);
                cell->has_text ? msgpack_pack_true(&packer)
                               : msgpack_pack_false(&packer);
                cell->has_background ? msgpack_pack_true(&packer)
                                     : msgpack_pack_false(&packer);
                cell->has_underline_color ? msgpack_pack_true(&packer)
                                          : msgpack_pack_false(&packer);
                cell->bold ? msgpack_pack_true(&packer)
                           : msgpack_pack_false(&packer);
                cell->italic ? msgpack_pack_true(&packer)
                             : msgpack_pack_false(&packer);
                cell->faint ? msgpack_pack_true(&packer)
                            : msgpack_pack_false(&packer);
                cell->blink ? msgpack_pack_true(&packer)
                            : msgpack_pack_false(&packer);
                cell->inverse ? msgpack_pack_true(&packer)
                              : msgpack_pack_false(&packer);
                cell->invisible ? msgpack_pack_true(&packer)
                                : msgpack_pack_false(&packer);
                cell->strikethrough ? msgpack_pack_true(&packer)
                                    : msgpack_pack_false(&packer);
                cell->overline ? msgpack_pack_true(&packer)
                               : msgpack_pack_false(&packer);
                msgpack_pack_uint8(&packer, cell->underline);
                msgpack_pack_uint8(&packer, cell->wide);
                msgpack_pack_str(&packer, text_len);
                msgpack_pack_str_body(&packer, cell->text, text_len);
            }
        } else if (record.kind == SNAPSHOT_IMAGE
                   && record.size >= sizeof(SnapshotImage)) {
            SnapshotImage image;
            memcpy(&image, payload, sizeof(image));
            if (sizeof(image) + image.pixel_len != record.size) return false;
            msgpack_pack_array(&packer, 15);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_int32(&packer, image.viewport_col);
            msgpack_pack_int32(&packer, image.viewport_row);
            msgpack_pack_uint32(&packer, image.x_offset);
            msgpack_pack_uint32(&packer, image.y_offset);
            msgpack_pack_uint32(&packer, image.grid_cols);
            msgpack_pack_uint32(&packer, image.grid_rows);
            msgpack_pack_uint32(&packer, image.image_w);
            msgpack_pack_uint32(&packer, image.image_h);
            msgpack_pack_uint32(&packer, image.src_x);
            msgpack_pack_uint32(&packer, image.src_y);
            msgpack_pack_uint32(&packer, image.src_w);
            msgpack_pack_uint32(&packer, image.src_h);
            msgpack_pack_int32(&packer, image.layer);
            msgpack_pack_bin(&packer, image.pixel_len);
            msgpack_pack_bin_body(&packer, payload + sizeof(image),
                                  image.pixel_len);
        } else if (record.kind == SNAPSHOT_TITLE) {
            msgpack_pack_array(&packer, 2);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_str(&packer, record.size);
            msgpack_pack_str_body(&packer, payload, record.size);
        } else if (record.kind == SNAPSHOT_CLIPBOARD
                   && record.size >= sizeof(SnapshotClipboard)) {
            SnapshotClipboard clipboard;
            memcpy(&clipboard, payload, sizeof(clipboard));
            if (sizeof(clipboard) + clipboard.data_len != record.size)
                return false;
            msgpack_pack_array(&packer, 3);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_uint32(&packer, clipboard.location);
            msgpack_pack_bin(&packer, clipboard.data_len);
            msgpack_pack_bin_body(&packer, payload + sizeof(clipboard),
                                  clipboard.data_len);
        } else if (record.kind == SNAPSHOT_PONG
                   && (record.size == 0 || record.size == sizeof(bool))) {
            msgpack_pack_array(&packer, record.size ? 2 : 1);
            msgpack_pack_uint32(&packer, record.kind);
            if (record.size) {
                bool attached;
                memcpy(&attached, payload, sizeof(attached));
                attached ? msgpack_pack_true(&packer)
                         : msgpack_pack_false(&packer);
            }
        } else {
            return false;
        }
        offset += record.size;
    }
    return !output.failed && offset == wire->len;
}
#endif

#ifdef GMUX_CLIENT
static bool snapshot_unpack(const void *data, size_t len,
                            RenderSnapshotWire *wire)
{
    msgpack_unpacked unpacked;
    msgpack_unpacked_init(&unpacked);
    size_t offset = 0;
    if (msgpack_unpack_next(&unpacked, data, len, &offset)
            != MSGPACK_UNPACK_SUCCESS
        || offset != len
        || unpacked.data.type != MSGPACK_OBJECT_ARRAY) {
        msgpack_unpacked_destroy(&unpacked);
        return false;
    }
    msgpack_object_array messages = unpacked.data.via.array;
    for (uint32_t i = 0; i < messages.size; i++) {
        msgpack_object message = messages.ptr[i];
        if (message.type != MSGPACK_OBJECT_ARRAY || message.via.array.size < 1)
            goto fail;
        msgpack_object *field = message.via.array.ptr;
        uint64_t kind;
        if (!object_u64(field[0], &kind)) goto fail;
        if (kind == SNAPSHOT_HEADER
            && (message.via.array.size == 12
                || message.via.array.size == 13
                || message.via.array.size == 17
                || message.via.array.size == 22)) {
            uint64_t rows, cols, cursor_col, cursor_row;
            SnapshotHeader header = {0};
            uint32_t base = message.via.array.size == 12 ? 0 : 1;
            if (!object_u64(field[1], &rows)
                || !object_u64(field[2], &cols)
                || !unpack_rgb(field[3], &header.background)
                || (base && !object_bool(field[4], &header.mouse_tracking))
                || !object_bool(field[4 + base], &header.cursor_visible)
                || !object_u64(field[5 + base], &cursor_col)
                || !object_u64(field[6 + base], &cursor_row)
                || !unpack_rgb(field[7 + base], &header.cursor_color)
                || !object_bool(field[8 + base], &header.scrollbar_visible)
                || !object_u64(field[9 + base], &header.scrollbar_total)
                || !object_u64(field[10 + base], &header.scrollbar_len)
                || !object_u64(field[11 + base], &header.scrollbar_offset))
                goto fail;
            if (!rows || rows > UINT16_MAX || !cols || cols > UINT16_MAX
                || cursor_col > UINT16_MAX || cursor_row > UINT16_MAX)
                goto fail;
            if (message.via.array.size >= 17
                && (!unpack_rgb(field[13], &header.selection_background)
                    || !unpack_rgb(field[14], &header.selection_foreground)
                    || !object_bool(field[15],
                                    &header.has_selection_background)
                    || !object_bool(field[16],
                                    &header.has_selection_foreground)))
                goto fail;
            if (message.via.array.size == 22) {
                uint64_t serial, above, below;
                if (!object_bool(field[17], &header.viewport_scrollable)
                    || !object_bool(field[18], &header.viewport_active)
                    || !object_u64(field[19], &serial)
                    || !object_u64(field[20], &above)
                    || !object_u64(field[21], &below)
                    || serial > UINT32_MAX || above > UINT16_MAX
                    || below > UINT16_MAX)
                    goto fail;
                header.scroll_serial = (uint32_t)serial;
                header.overscan_above = (uint16_t)above;
                header.overscan_below = (uint16_t)below;
            }
            if (rows + header.overscan_above + header.overscan_below
                    > UINT16_MAX) goto fail;
            header.rows = (uint16_t)rows;
            header.cols = (uint16_t)cols;
            header.cursor_col = (uint16_t)cursor_col;
            header.cursor_row = (uint16_t)cursor_row;
            if (!snapshot_record(wire, SNAPSHOT_HEADER,
                                 &header, sizeof(header))) goto fail;
        } else if (kind == SNAPSHOT_ROW
                   && (message.via.array.size == 3
                       || message.via.array.size == 4)) {
            uint32_t cells_index = message.via.array.size == 4 ? 3 : 2;
            int64_t row_index;
            if (!object_i64(field[1], &row_index)
                || row_index < INT32_MIN || row_index > INT32_MAX
                || field[cells_index].type != MSGPACK_OBJECT_ARRAY
                || field[cells_index].via.array.size > UINT16_MAX) goto fail;
            SnapshotRow row = {
                .row = (int32_t)row_index,
                .cell_count = (uint16_t)field[cells_index].via.array.size,
            };
            if (cells_index == 3 && !object_bool(field[2], &row.wrapped))
                goto fail;
            SnapshotRecord record = {
                SNAPSHOT_ROW,
                (uint32_t)(sizeof(row)
                    + (size_t)row.cell_count * sizeof(SnapshotCell)),
            };
            if (!snapshot_append(wire, &record, sizeof(record))
                || !snapshot_append(wire, &row, sizeof(row))) goto fail;
            for (uint16_t j = 0; j < row.cell_count; j++) {
                msgpack_object cell_object = field[cells_index].via.array.ptr[j];
                if (cell_object.type != MSGPACK_OBJECT_ARRAY
                    || (cell_object.via.array.size != 8
                        && cell_object.via.array.size != 17
                        && cell_object.via.array.size != 18)) goto fail;
                msgpack_object *cell_field = cell_object.via.array.ptr;
                uint64_t col;
                SnapshotCell cell = {0};
                if (!object_u64(cell_field[0], &col)
                    || col > UINT16_MAX
                    || !unpack_rgb(cell_field[1], &cell.foreground)
                    || !unpack_rgb(cell_field[2], &cell.background)) goto fail;
                uint32_t text_index;
                if (cell_object.via.array.size == 8) {
                    // Compatibility with the first published Rocky server.
                    if (!object_bool(cell_field[3], &cell.has_text)
                        || !object_bool(cell_field[4], &cell.has_background)
                        || !object_bool(cell_field[5], &cell.italic)
                        || !object_bool(cell_field[6], &cell.bold)) goto fail;
                    text_index = 7;
                } else {
                    uint64_t underline;
                    if (!unpack_rgb(cell_field[3], &cell.underline_color)
                        || !object_bool(cell_field[4], &cell.has_text)
                        || !object_bool(cell_field[5], &cell.has_background)
                        || !object_bool(cell_field[6],
                                        &cell.has_underline_color)
                        || !object_bool(cell_field[7], &cell.bold)
                        || !object_bool(cell_field[8], &cell.italic)
                        || !object_bool(cell_field[9], &cell.faint)
                        || !object_bool(cell_field[10], &cell.blink)
                        || !object_bool(cell_field[11], &cell.inverse)
                        || !object_bool(cell_field[12], &cell.invisible)
                        || !object_bool(cell_field[13], &cell.strikethrough)
                        || !object_bool(cell_field[14], &cell.overline)
                        || !object_u64(cell_field[15], &underline)
                        || underline > UINT8_MAX) goto fail;
                    cell.underline = (uint8_t)underline;
                    text_index = 16;
                    if (cell_object.via.array.size == 18) {
                        uint64_t wide;
                        if (!object_u64(cell_field[16], &wide)
                            || wide > UINT8_MAX) goto fail;
                        cell.wide = (uint8_t)wide;
                        text_index = 17;
                    }
                }
                if (cell_field[text_index].type != MSGPACK_OBJECT_STR
                    || cell_field[text_index].via.str.size
                        >= sizeof(cell.text)) goto fail;
                cell.col = (uint16_t)col;
                memcpy(cell.text, cell_field[text_index].via.str.ptr,
                       cell_field[text_index].via.str.size);
                if (!snapshot_append(wire, &cell, sizeof(cell))) goto fail;
            }
        } else if (kind == SNAPSHOT_IMAGE && message.via.array.size == 15
                   && field[14].type == MSGPACK_OBJECT_BIN) {
            int64_t viewport_col, viewport_row, layer;
            uint64_t value[10];
            if (!object_i64(field[1], &viewport_col)
                || !object_i64(field[2], &viewport_row)) goto fail;
            for (int j = 0; j < 10; j++)
                if (!object_u64(field[j + 3], &value[j])
                    || value[j] > UINT32_MAX) goto fail;
            if (!object_i64(field[13], &layer)
                || viewport_col < INT32_MIN || viewport_col > INT32_MAX
                || viewport_row < INT32_MIN || viewport_row > INT32_MAX
                || layer < INT32_MIN || layer > INT32_MAX
                // Cairo uses signed int dimensions/stride. Bound them before
                // the renderer allocates or multiplies, and validate RGBA data.
                || !value[4] || value[4] > INT_MAX / 4
                || !value[5] || value[5] > INT_MAX
                || value[4] * value[5] * 4 != field[14].via.bin.size
                || value[6] > value[4] || value[7] > value[5]
                || value[8] > value[4] - value[6]
                || value[9] > value[5] - value[7]) goto fail;
            SnapshotImage image = {
                .viewport_col = (int32_t)viewport_col,
                .viewport_row = (int32_t)viewport_row,
                .x_offset = (uint32_t)value[0],
                .y_offset = (uint32_t)value[1],
                .grid_cols = (uint32_t)value[2],
                .grid_rows = (uint32_t)value[3],
                .image_w = (uint32_t)value[4],
                .image_h = (uint32_t)value[5],
                .src_x = (uint32_t)value[6],
                .src_y = (uint32_t)value[7],
                .src_w = (uint32_t)value[8],
                .src_h = (uint32_t)value[9],
                .pixel_len = field[14].via.bin.size,
                .layer = (int32_t)layer,
            };
            SnapshotRecord record = {
                SNAPSHOT_IMAGE,
                (uint32_t)(sizeof(image) + image.pixel_len),
            };
            if (!snapshot_append(wire, &record, sizeof(record))
                || !snapshot_append(wire, &image, sizeof(image))
                || !snapshot_append(wire, field[14].via.bin.ptr,
                                    image.pixel_len)) goto fail;
        } else if (kind == SNAPSHOT_TITLE && message.via.array.size == 2
                   && field[1].type == MSGPACK_OBJECT_STR) {
            if (!snapshot_record(wire, SNAPSHOT_TITLE,
                    field[1].via.str.ptr, field[1].via.str.size)) goto fail;
        } else if (kind == SNAPSHOT_CLIPBOARD
                   && message.via.array.size == 3
                   && field[2].type == MSGPACK_OBJECT_BIN) {
            uint64_t location;
            if (!object_u64(field[1], &location)
                || location > UINT32_MAX
                || field[2].via.bin.size > UINT32_MAX) goto fail;
            SnapshotClipboard clipboard = {
                .location = (uint32_t)location,
                .data_len = field[2].via.bin.size,
            };
            SnapshotRecord record = {
                SNAPSHOT_CLIPBOARD,
                (uint32_t)(sizeof(clipboard) + clipboard.data_len),
            };
            if (!snapshot_append(wire, &record, sizeof(record))
                || !snapshot_append(wire, &clipboard, sizeof(clipboard))
                || !snapshot_append(wire, field[2].via.bin.ptr,
                                    clipboard.data_len)) goto fail;
        } else if (kind == SNAPSHOT_PONG && message.via.array.size == 1) {
            if (!snapshot_record(wire, SNAPSHOT_PONG, NULL, 0)) goto fail;
        } else {
            goto fail;
        }
    }
    msgpack_unpacked_destroy(&unpacked);
    return true;

fail:
    msgpack_unpacked_destroy(&unpacked);
    return false;
}
#endif

#if defined(GMUX_CLIENT) || defined(GMUX_SERVER)
static bool input_queue(WireConnection *connection, InputWire *outgoing)
{
    msgpack_sbuffer packed;
    msgpack_sbuffer_init(&packed);
    bool ok = input_pack(outgoing, &packed)
        && wire_queue_frame(connection, packed.data, packed.size);
    outgoing->len = 0;
    msgpack_sbuffer_destroy(&packed);
    return ok;
}

static int kill_server_process(const char *path)
{
    int fd = unix_socket(path, false);
    if (fd < 0) return 1;
    WireConnection connection = { .fd = -1 };
    if (!wire_connection_init(&connection, fd)) {
        close(fd);
        return 1;
    }
    InputWire input = {0};
    int result = 1;
    if (!input_append(&input, INPUT_SHUTDOWN, NULL, 0)
        || !input_queue(&connection, &input)) goto cleanup;
    while (connection.output.start < connection.output.end) {
        if (!wire_flush(&connection)) goto cleanup;
        if (connection.output.start == connection.output.end) break;
        struct pollfd ready = { .fd = connection.fd, .events = POLLOUT };
        if (poll(&ready, 1, 5000) <= 0) goto cleanup;
    }
    result = 0;

cleanup:
    free(input.data);
    wire_connection_close(&connection);
    return result;
}
#endif

#ifdef GMUX_SERVER
static bool input_decode(const void *packed, size_t packed_len,
                         InputWire *incoming)
{
    incoming->len = 0;
    return input_unpack(packed, packed_len, incoming);
}

static bool snapshot_queue(WireConnection *connection,
                           const RenderSnapshotWire *outgoing)
{
    msgpack_sbuffer packed;
    msgpack_sbuffer_init(&packed);
    bool ok = snapshot_pack(outgoing, &packed)
        && wire_queue_frame(connection, packed.data, packed.size);
    msgpack_sbuffer_destroy(&packed);
    return ok;
}
#endif

#ifdef GMUX_CLIENT
static bool snapshot_decode(const void *packed, size_t packed_len,
                            RenderSnapshotWire *incoming)
{
    incoming->len = 0;
    return snapshot_unpack(packed, packed_len, incoming);
}
#endif

#ifdef GMUX_SERVER
static SnapshotRgb snapshot_rgb(GhosttyColorRgb color)
{
    return (SnapshotRgb){ color.r, color.g, color.b };
}

static bool snapshot_style_color(GhosttyStyleColor color,
                                 const GhosttyRenderStateColors *colors,
                                 SnapshotRgb *out)
{
    if (color.tag == GHOSTTY_STYLE_COLOR_RGB) {
        *out = snapshot_rgb(color.value.rgb);
        return true;
    }
    if (color.tag == GHOSTTY_STYLE_COLOR_PALETTE) {
        *out = snapshot_rgb(colors->palette[color.value.palette]);
        return true;
    }
    return false;
}

#endif
static bool snapshot_append(RenderSnapshotWire *wire,
                            const void *data, size_t len)
{
    if (!len) return true;
    if (len > MAX_WIRE_QUEUE - wire->len) return false;
    if (wire->len + len > wire->cap) {
        size_t cap = wire->cap ? wire->cap : 4096;
        while (cap < wire->len + len) cap *= 2;
        void *next = realloc(wire->data, cap);
        if (!next) return false;
        wire->data = next;
        wire->cap = cap;
    }
    memcpy(wire->data + wire->len, data, len);
    wire->len += len;
    return true;
}

static bool snapshot_record(RenderSnapshotWire *wire, uint32_t kind,
                            const void *payload, size_t payload_len)
{
    if (payload_len > UINT32_MAX) return false;
    SnapshotRecord record = { kind, (uint32_t)payload_len };
    return snapshot_append(wire, &record, sizeof(record))
        && snapshot_append(wire, payload, payload_len);
}
