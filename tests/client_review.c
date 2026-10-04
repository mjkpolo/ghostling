// Regression checks for client bookkeeping. No window or display is needed.
#define GMUX_CLIENT
#define main gmux_main
#include "../gmux_core.c"
#undef main
#include <assert.h>

static void test_queued_paste(void)
{
    int sockets[2];
    int result = socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
    assert(result == 0);
    int size = 4096;
    result = setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    assert(result == 0);
    GtkClient client = { .connection.fd = -1 };
    WireConnection receiver = { .fd = -1 };
    bool ok = wire_connection_init(&client.connection, sockets[0]);
    assert(ok);
    ok = wire_connection_init(&receiver, sockets[1]);
    assert(ok);
    const size_t text_size = 1024 * 1024;
    char *text = malloc(text_size);
    assert(text);
    memset(text, 'x', text_size);
    ok = gtk_client_send(&client, INPUT_PASTE, text, text_size);
    assert(ok && client.write_source != 0);

    // The peer starts reading after the original send blocked. The GLib
    // writable source must finish the paste without another input event.
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (client.write_source && g_get_monotonic_time() < deadline) {
        ok = wire_read(&receiver);
        assert(ok);
        g_main_context_iteration(NULL, FALSE);
    }
    assert(client.write_source == 0);
    ok = wire_read(&receiver);
    assert(ok);
    uint8_t *packed = NULL;
    size_t packed_len = 0;
    result = wire_take_frame(&receiver, &packed, &packed_len);
    assert(result == 1);
    msgpack_unpacked decoded;
    msgpack_unpacked_init(&decoded);
    ok = msgpack_unpack_next(&decoded, (const char *)packed, packed_len, NULL);
    assert(ok && decoded.data.type == MSGPACK_OBJECT_ARRAY);
    assert(decoded.data.via.array.size == 1);
    msgpack_object message = decoded.data.via.array.ptr[0];
    assert(message.type == MSGPACK_OBJECT_ARRAY && message.via.array.size == 2);
    msgpack_object *fields = message.via.array.ptr;
    assert(fields[0].via.u64 == INPUT_PASTE);
    assert(fields[1].type == MSGPACK_OBJECT_BIN);
    assert(fields[1].via.bin.size == text_size);
    assert(memcmp(fields[1].via.bin.ptr, text, text_size) == 0);
    msgpack_unpacked_destroy(&decoded);
    free(packed);
    free(text);
    free(client.input.data);
    wire_connection_close(&receiver);
    wire_connection_close(&client.connection);
}

static void test_shortcut_release(void)
{
    // Ctrl+Shift+C's press was consumed locally. Even if the modifiers have
    // already been released, its C release must not reach the server.
    GtkClient client = { .connection.fd = -1 };
    gtk_key_released(NULL, GDK_KEY_c, 54, 0, &client);
    assert(client.input.len == 0 && client.connection.output.end == 0);
}

static void test_selection_bounds(void)
{
    ClientSelection selection = {
        .active = true, .start_row = 3, .start_col = 2,
        .end_row = 1, .end_col = 5,
    };
    assert(!selection_contains_cell(&selection, 1, 4));
    assert(selection_contains_cell(&selection, 1, 5));
    assert(selection_contains_cell(&selection, 2, 0));
    assert(selection_contains_cell(&selection, 3, 2));
    assert(!selection_contains_cell(&selection, 3, 3));
}

static void test_row_resize(void)
{
    RenderClientState render = {0};
    bool ok = render_client_resize(&render, 4);
    assert(ok);
    for (uint16_t row = 0; row < render.row_count; row++) {
        render.rows[row].cells = calloc(1, sizeof(SnapshotCell));
        assert(render.rows[row].cells);
        render.rows[row].cell_count = 1;
    }
    ok = render_client_resize(&render, 2);
    assert(ok && render.row_count == 2);
    ok = render_client_resize(&render, 4);
    assert(ok && render.rows[2].cells == NULL && render.rows[3].cells == NULL);
    render_client_free(&render);
}

static void test_font_fallback(void)
{
    GtkClient client = { .font_size = 24 };
    client.regular = pango_font_description_new();
    client.italic = pango_font_description_new();
    client.bold = pango_font_description_new();
    client.bold_italic = pango_font_description_new();
    snprintf(client.font_family, sizeof(client.font_family),
        "gmux-test-missing-font, DejaVu Sans Mono");
    gtk_apply_font(&client);
    assert(strcmp(pango_font_description_get_family(client.regular),
        "gmux-test-missing-font, DejaVu Sans Mono, monospace") == 0);
    assert(strcmp(pango_font_description_get_family(client.bold_italic),
        "gmux-test-missing-font, DejaVu Sans Mono, monospace") == 0);

    // Config strings are untrusted bytes; do not hand invalid UTF-8 to Pango.
    strcpy(client.font_family, "\xff");
    gtk_apply_font(&client);
    assert(strcmp(pango_font_description_get_family(client.regular), "monospace") == 0);
    pango_font_description_free(client.regular);
    pango_font_description_free(client.italic);
    pango_font_description_free(client.bold);
    pango_font_description_free(client.bold_italic);
}

int main(void)
{
    test_queued_paste();
    test_shortcut_release();
    test_selection_bounds();
    test_row_resize();
    test_font_fallback();
    puts("client regressions: queued paste, shortcut release, selection, row resize, font fallback passed");
    return 0;
}
