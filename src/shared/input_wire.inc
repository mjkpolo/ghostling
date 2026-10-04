// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------

typedef struct { uint8_t *data; size_t len, cap; } InputWire;

typedef enum {
    INPUT_ATTACH = 1,
    INPUT_DETACH,
    INPUT_RESIZE,
    INPUT_KEY,
    INPUT_MOUSE,
    INPUT_FOCUS,
    INPUT_SHUTDOWN,
    INPUT_PASTE,
    INPUT_PING,
    INPUT_FONT_DELTA,
} InputKind;

typedef struct { uint32_t kind, size; } InputRecord;

typedef struct {
    uint16_t cols, rows;
    uint32_t cell_width, cell_height;
    uint32_t screen_width, screen_height;
    uint32_t padding;
} InputResize;

typedef struct {
    GhosttyKey key;
    GhosttyKeyAction action;
    GhosttyMods mods, consumed_mods;
    uint32_t unshifted_codepoint;
    uint8_t text_len;
    char text[64];
} InputKey;

typedef struct {
    GhosttyMouseAction action;
    GhosttyMouseButton button;
    GhosttyMods mods;
    float x, y;
    int8_t wheel;
    bool has_button, any_button_pressed;
    uint32_t scroll_serial;
} InputMouse;

typedef struct { bool focused; } InputFocus;

#define MAX_WIRE_MESSAGE (128u * 1024u * 1024u)
#define MAX_WIRE_QUEUE (256u * 1024u * 1024u)

static bool input_append(InputWire *wire, InputKind kind,
                         const void *payload, size_t size)
{
    if (wire->len > MAX_WIRE_QUEUE - sizeof(InputRecord)
        || size > MAX_WIRE_QUEUE - sizeof(InputRecord) - wire->len)
        return false;
    size_t needed = wire->len + sizeof(InputRecord) + size;
    if (needed > wire->cap) {
        size_t cap = wire->cap ? wire->cap : 1024;
        while (cap < needed) cap *= 2;
        void *next = realloc(wire->data, cap);
        if (!next) return false;
        wire->data = next;
        wire->cap = cap;
    }
    InputRecord record = { (uint32_t)kind, (uint32_t)size };
    memcpy(wire->data + wire->len, &record, sizeof(record));
    wire->len += sizeof(record);
    if (size) {
        memcpy(wire->data + wire->len, payload, size);
        wire->len += size;
    }
    return true;
}

// MessagePack's pack calls return allocation errors individually. Remember the
// first one so a long record cannot accidentally emit a truncated message.
typedef struct {
    msgpack_sbuffer *buffer;
    bool failed;
} WirePacker;

static int wire_pack_write(void *userdata, const char *data, size_t len)
{
    WirePacker *packer = userdata;
    if (packer->failed || len > MAX_WIRE_MESSAGE - packer->buffer->size
        || msgpack_sbuffer_write(packer->buffer, data, len) != 0) {
        packer->failed = true;
        return -1;
    }
    return 0;
}

typedef struct {
    uint8_t *data;
    size_t start, end, cap;
} WireBuffer;

typedef struct {
    int fd;
    WireBuffer input, output;
} WireConnection;

#ifdef GMUX_SERVER
static volatile sig_atomic_t server_stop = 0;

static void stop_server(int signal_number)
{
    (void)signal_number;
    server_stop = 1;
}
#endif

static int unix_socket(const char *path, bool server)
{
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(address.sun_path, path, strlen(path) + 1);

    if (!server) {
        struct stat socket_stat;
        if (lstat(path, &socket_stat) < 0) return -1;
        if (!S_ISSOCK(socket_stat.st_mode)
            || socket_stat.st_uid != getuid()
            || (socket_stat.st_mode & 0077) != 0) {
            errno = EACCES;
            return -1;
        }
    } else {
        char parent[PATH_MAX];
        if (strlen(path) >= sizeof(parent)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        strcpy(parent, path);
        char *slash = strrchr(parent, '/');
        if (!slash) strcpy(parent, ".");
        else if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
        struct stat parent_stat;
        if (lstat(parent, &parent_stat) < 0) return -1;
        if (!S_ISDIR(parent_stat.st_mode)
            || parent_stat.st_uid != getuid()
            || (parent_stat.st_mode & 0022) != 0) {
            errno = EACCES;
            return -1;
        }
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (server) {
        struct stat existing;
        if (lstat(path, &existing) == 0) {
            close(fd);
            errno = EADDRINUSE;
            return -1;
        }
        if (errno != ENOENT) {
            close(fd);
            return -1;
        }
        mode_t old_mask = umask(0077);
        int result = bind(fd, (struct sockaddr *)&address, sizeof(address));
        umask(old_mask);
        if (result < 0 || chmod(path, 0600) < 0 || listen(fd, 1) < 0) {
            int saved = errno;
            close(fd);
            // A competing bind can win after lstat; never unlink its socket.
            if (result == 0) unlink(path);
            errno = saved;
            return -1;
        }
    } else if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0
               || connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static bool wire_buffer_reserve(WireBuffer *buffer, size_t len)
{
    size_t used = buffer->end - buffer->start;
    if (len > MAX_WIRE_QUEUE - used) {
        errno = ENOBUFS;
        return false;
    }
    if (buffer->cap - buffer->end < len && buffer->start) {
        memmove(buffer->data, buffer->data + buffer->start, used);
        buffer->start = 0;
        buffer->end = used;
    }
    if (buffer->cap - buffer->end < len) {
        size_t cap = buffer->cap ? buffer->cap : 4096;
        while (cap < buffer->end + len) cap *= 2;
        if (cap > MAX_WIRE_QUEUE) cap = MAX_WIRE_QUEUE;
        void *next = realloc(buffer->data, cap);
        if (!next) return false;
        buffer->data = next;
        buffer->cap = cap;
    }
    return true;
}

static bool wire_buffer_append(WireBuffer *buffer,
                               const void *data, size_t len)
{
    if (!len) return true;
    if (!wire_buffer_reserve(buffer, len)) return false;
    memcpy(buffer->data + buffer->end, data, len);
    buffer->end += len;
    return true;
}

static bool wire_connection_init(WireConnection *connection, int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return false;
    *connection = (WireConnection){ .fd = fd };
    return true;
}

static void wire_connection_close(WireConnection *connection)
{
    if (connection->fd >= 0) close(connection->fd);
    free(connection->input.data);
    free(connection->output.data);
    *connection = (WireConnection){ .fd = -1 };
}

static bool wire_queue_frame(WireConnection *connection,
                             const void *payload, size_t len)
{
    WireBuffer *buffer = &connection->output;
    if (len > MAX_WIRE_MESSAGE
        || !wire_buffer_reserve(buffer, sizeof(uint32_t) + len))
        return false;
    uint32_t network_len = htonl((uint32_t)len);
    memcpy(buffer->data + buffer->end, &network_len, sizeof(network_len));
    buffer->end += sizeof(network_len);
    memcpy(buffer->data + buffer->end, payload, len);
    buffer->end += len;
    return true;
}

static bool wire_buffer_flush(WireBuffer *buffer, int fd)
{
    while (buffer->start < buffer->end) {
        ssize_t n = write(fd, buffer->data + buffer->start,
                          buffer->end - buffer->start);
        if (n > 0) buffer->start += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return true;
        else return false;
    }
    buffer->start = buffer->end = 0;
    return true;
}

static bool wire_flush(WireConnection *connection)
{
    return wire_buffer_flush(&connection->output, connection->fd);
}

static bool wire_read(WireConnection *connection)
{
    uint8_t bytes[65536];
    // Return to dispatch periodically even if a producer is continuously busy.
    for (size_t received = 0; received < 1024 * 1024;) {
        ssize_t n = read(connection->fd, bytes, sizeof(bytes));
        if (n > 0) {
            received += (size_t)n;
            if (!wire_buffer_append(&connection->input, bytes, (size_t)n))
                return false;
        } else if (n == 0) {
            errno = 0;
            return false;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        } else {
            return false;
        }
    }
    return true;
}

// Returns 1 for a complete frame, 0 when more bytes are needed, and -1 for
// an invalid frame or allocation failure. The caller owns *payload.
static int wire_take_frame(WireConnection *connection,
                           uint8_t **payload, size_t *len)
{
    WireBuffer *buffer = &connection->input;
    size_t available = buffer->end - buffer->start;
    if (available < sizeof(uint32_t)) return 0;
    uint32_t network_len;
    memcpy(&network_len, buffer->data + buffer->start, sizeof(network_len));
    *len = ntohl(network_len);
    if (*len > MAX_WIRE_MESSAGE) return -1;
    if (available - sizeof(network_len) < *len) return 0;
    *payload = malloc(*len ? *len : 1);
    if (!*payload) return -1;
    memcpy(*payload, buffer->data + buffer->start + sizeof(network_len), *len);
    buffer->start += sizeof(network_len) + *len;
    if (buffer->start == buffer->end)
        buffer->start = buffer->end = 0;
    return 1;
}

static bool object_u64(msgpack_object object, uint64_t *value)
{
    if (object.type == MSGPACK_OBJECT_POSITIVE_INTEGER) {
        *value = object.via.u64;
        return true;
    }
    return false;
}

static bool object_i64(msgpack_object object, int64_t *value)
{
    if (object.type == MSGPACK_OBJECT_POSITIVE_INTEGER
        && object.via.u64 <= INT64_MAX) {
        *value = (int64_t)object.via.u64;
        return true;
    }
    if (object.type == MSGPACK_OBJECT_NEGATIVE_INTEGER) {
        *value = object.via.i64;
        return true;
    }
    return false;
}

#ifdef GMUX_SERVER
static bool object_float(msgpack_object object, double *value)
{
    if (object.type == MSGPACK_OBJECT_FLOAT32
        || object.type == MSGPACK_OBJECT_FLOAT64) {
        *value = object.via.f64;
        return true;
    }
    return false;
}
#endif

static bool object_bool(msgpack_object object, bool *value)
{
    if (object.type != MSGPACK_OBJECT_BOOLEAN) return false;
    *value = object.via.boolean;
    return true;
}

#if defined(GMUX_CLIENT) || defined(GMUX_SERVER)
static size_t input_record_count(const InputWire *wire)
{
    size_t count = 0, offset = 0;
    while (offset + sizeof(InputRecord) <= wire->len) {
        InputRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) break;
        offset += record.size;
        count++;
    }
    return count;
}

static bool input_pack(const InputWire *wire, msgpack_sbuffer *buffer)
{
    WirePacker output = { .buffer = buffer };
    msgpack_packer packer;
    msgpack_packer_init(&packer, &output, wire_pack_write);
    msgpack_pack_array(&packer, input_record_count(wire));
    size_t offset = 0;
    while (offset + sizeof(InputRecord) <= wire->len) {
        InputRecord record;
        memcpy(&record, wire->data + offset, sizeof(record));
        offset += sizeof(record);
        if (offset + record.size > wire->len) return false;
        const uint8_t *payload = wire->data + offset;
        if (record.kind == INPUT_ATTACH || record.kind == INPUT_DETACH
            || record.kind == INPUT_SHUTDOWN || record.kind == INPUT_PING) {
            msgpack_pack_array(&packer, 1);
            msgpack_pack_uint32(&packer, record.kind);
        } else if (record.kind == INPUT_RESIZE
                   && record.size == sizeof(InputResize)) {
            InputResize value;
            memcpy(&value, payload, sizeof(value));
            msgpack_pack_array(&packer, 8);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_uint16(&packer, value.cols);
            msgpack_pack_uint16(&packer, value.rows);
            msgpack_pack_uint32(&packer, value.cell_width);
            msgpack_pack_uint32(&packer, value.cell_height);
            msgpack_pack_uint32(&packer, value.screen_width);
            msgpack_pack_uint32(&packer, value.screen_height);
            msgpack_pack_uint32(&packer, value.padding);
        } else if (record.kind == INPUT_KEY
                   && record.size == sizeof(InputKey)) {
            InputKey value;
            memcpy(&value, payload, sizeof(value));
            if (value.text_len > sizeof(value.text)) return false;
            msgpack_pack_array(&packer, 8);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_uint32(&packer, value.key);
            msgpack_pack_uint32(&packer, value.action);
            msgpack_pack_uint32(&packer, value.mods);
            msgpack_pack_uint32(&packer, value.consumed_mods);
            msgpack_pack_uint32(&packer, value.unshifted_codepoint);
            msgpack_pack_bin(&packer, value.text_len);
            msgpack_pack_bin_body(&packer, value.text, value.text_len);
            msgpack_pack_uint8(&packer, value.text_len);
        } else if (record.kind == INPUT_MOUSE
                   && record.size == sizeof(InputMouse)) {
            InputMouse value;
            memcpy(&value, payload, sizeof(value));
            msgpack_pack_array(&packer, 10);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_uint32(&packer, value.action);
            msgpack_pack_uint32(&packer, value.button);
            msgpack_pack_uint32(&packer, value.mods);
            msgpack_pack_float(&packer, value.x);
            msgpack_pack_float(&packer, value.y);
            msgpack_pack_int8(&packer, value.wheel);
            value.has_button ? msgpack_pack_true(&packer)
                             : msgpack_pack_false(&packer);
            value.any_button_pressed ? msgpack_pack_true(&packer)
                                     : msgpack_pack_false(&packer);
            msgpack_pack_uint32(&packer, value.scroll_serial);
        } else if (record.kind == INPUT_FOCUS
                   && record.size == sizeof(InputFocus)) {
            InputFocus value;
            memcpy(&value, payload, sizeof(value));
            msgpack_pack_array(&packer, 2);
            msgpack_pack_uint32(&packer, record.kind);
            value.focused ? msgpack_pack_true(&packer)
                          : msgpack_pack_false(&packer);
        } else if (record.kind == INPUT_FONT_DELTA && record.size == sizeof(int8_t)) {
            msgpack_pack_array(&packer, 2);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_int8(&packer, *(const int8_t *)payload);
        } else if (record.kind == INPUT_PASTE) {
            msgpack_pack_array(&packer, 2);
            msgpack_pack_uint32(&packer, record.kind);
            msgpack_pack_bin(&packer, record.size);
            msgpack_pack_bin_body(&packer, payload, record.size);
        } else {
            return false;
        }
        offset += record.size;
    }
    return !output.failed;
}
#endif

#ifdef GMUX_SERVER
static bool input_unpack(const void *data, size_t len, InputWire *wire)
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
        if ((kind == INPUT_ATTACH || kind == INPUT_DETACH
             || kind == INPUT_SHUTDOWN || kind == INPUT_PING)
            && message.via.array.size == 1) {
            if (!input_append(wire, (InputKind)kind, NULL, 0)) goto fail;
        } else if (kind == INPUT_RESIZE && message.via.array.size == 8) {
            uint64_t value[7];
            for (int j = 0; j < 7; j++)
                if (!object_u64(field[j + 1], &value[j])) goto fail;
            if (!value[0] || value[0] > UINT16_MAX
                || !value[1] || value[1] > UINT16_MAX
                || !value[2] || !value[3]) goto fail;
            for (int j = 2; j < 7; j++)
                if (value[j] > INT_MAX) goto fail;
            InputResize resize = {
                (uint16_t)value[0], (uint16_t)value[1],
                (uint32_t)value[2], (uint32_t)value[3],
                (uint32_t)value[4], (uint32_t)value[5],
                (uint32_t)value[6],
            };
            if (!input_append(wire, INPUT_RESIZE, &resize, sizeof(resize)))
                goto fail;
        } else if (kind == INPUT_KEY && message.via.array.size == 8) {
            uint64_t value[5], text_len;
            for (int j = 0; j < 5; j++)
                if (!object_u64(field[j + 1], &value[j])
                    || value[j] > UINT32_MAX) goto fail;
            if (field[6].type != MSGPACK_OBJECT_BIN
                || !object_u64(field[7], &text_len)
                || text_len != field[6].via.bin.size
                || text_len > sizeof(((InputKey *)0)->text)
                || value[1] > GHOSTTY_KEY_ACTION_REPEAT) goto fail;
            InputKey key = {
                .key = (GhosttyKey)value[0],
                .action = (GhosttyKeyAction)value[1],
                .mods = (GhosttyMods)value[2],
                .consumed_mods = (GhosttyMods)value[3],
                .unshifted_codepoint = (uint32_t)value[4],
                .text_len = (uint8_t)text_len,
            };
            memcpy(key.text, field[6].via.bin.ptr, key.text_len);
            if (!input_append(wire, INPUT_KEY, &key, sizeof(key))) goto fail;
        } else if (kind == INPUT_MOUSE
                   && (message.via.array.size == 9
                       || message.via.array.size == 10)) {
            uint64_t action, button, mods;
            int64_t wheel;
            double x, y;
            InputMouse mouse = {0};
            if (!object_u64(field[1], &action)
                || !object_u64(field[2], &button)
                || !object_u64(field[3], &mods)
                || !object_float(field[4], &x)
                || !object_float(field[5], &y)
                || !object_i64(field[6], &wheel)
                || !object_bool(field[7], &mouse.has_button)
                || !object_bool(field[8], &mouse.any_button_pressed)
                || action > GHOSTTY_MOUSE_ACTION_MOTION
                || button > GHOSTTY_MOUSE_BUTTON_ELEVEN
                || mods > UINT32_MAX || wheel < INT8_MIN || wheel > INT8_MAX
                || !isfinite(x) || !isfinite(y)
                || x < INT_MIN || x > INT_MAX
                || y < INT_MIN || y > INT_MAX) goto fail;
            mouse.action = (GhosttyMouseAction)action;
            mouse.button = (GhosttyMouseButton)button;
            mouse.mods = (GhosttyMods)mods;
            mouse.x = (float)x;
            mouse.y = (float)y;
            mouse.wheel = (int8_t)wheel;
            if (message.via.array.size == 10) {
                uint64_t serial;
                if (!object_u64(field[9], &serial)
                    || serial > UINT32_MAX) goto fail;
                mouse.scroll_serial = (uint32_t)serial;
            }
            if (!input_append(wire, INPUT_MOUSE, &mouse, sizeof(mouse)))
                goto fail;
        } else if (kind == INPUT_FOCUS && message.via.array.size == 2) {
            InputFocus focus;
            if (!object_bool(field[1], &focus.focused)
                || !input_append(wire, INPUT_FOCUS, &focus, sizeof(focus)))
                goto fail;
        } else if (kind == INPUT_FONT_DELTA && message.via.array.size == 2) {
            int64_t delta;
            if (!object_i64(field[1], &delta) || (delta != -1 && delta != 1)) goto fail;
            int8_t value = (int8_t)delta;
            if (!input_append(wire, INPUT_FONT_DELTA, &value, sizeof(value))) goto fail;
        } else if (kind == INPUT_PASTE && message.via.array.size == 2
                   && field[1].type == MSGPACK_OBJECT_BIN) {
            if (!input_append(wire, INPUT_PASTE, field[1].via.bin.ptr,
                              field[1].via.bin.size)) goto fail;
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
