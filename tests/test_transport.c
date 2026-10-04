#define _GNU_SOURCE
#define GMUX_SERVER
#define GMUX_CLIENT
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>
#include <ghostty/vt.h>
#include <msgpack.h>

#include "../src/shared/config.inc"
#include "../src/shared/input_wire.inc"
#include "../src/server/theme_pty.inc"
#include "../src/shared/snapshot_wire.inc"
#include "../src/server/snapshot.inc"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

static void test_socket_backpressure(void)
{
    int sockets[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    int small = 1024;
    CHECK(setsockopt(sockets[0], SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
    WireConnection sender, receiver;
    CHECK(wire_connection_init(&sender, sockets[0]));
    CHECK(wire_connection_init(&receiver, sockets[1]));
    size_t size = 1024 * 1024;
    uint8_t *expected = malloc(size);
    CHECK(expected != NULL);
    for (size_t i = 0; i < size; i++) expected[i] = (uint8_t)i;
    CHECK(wire_queue_frame(&sender, expected, size));
    CHECK(wire_flush(&sender));
    CHECK(sender.output.start > 0 && sender.output.start < sender.output.end);
    uint8_t *received = NULL;
    size_t len = 0;
    for (int i = 0; i < 10000 && !received; i++) {
        CHECK(wire_read(&receiver));
        int frame = wire_take_frame(&receiver, &received, &len);
        CHECK(frame >= 0);
        CHECK(wire_flush(&sender));
    }
    CHECK(received && len == size && memcmp(received, expected, size) == 0);
    free(received);
    free(expected);

    // A final complete frame remains dispatchable even when read detects EOF.
    CHECK(wire_queue_frame(&sender, "last", 4));
    CHECK(wire_flush(&sender));
    wire_connection_close(&sender);
    CHECK(!wire_read(&receiver) && errno == 0);
    CHECK(wire_take_frame(&receiver, &received, &len) == 1);
    CHECK(len == 4 && memcmp(received, "last", len) == 0);
    free(received);
    wire_connection_close(&receiver);
}

static void test_pty_backpressure(void)
{
    Pty pty = { .fd = -1 };
    int slave;
    CHECK(openpty(&pty.fd, &slave, NULL, NULL, NULL) == 0);
    struct termios settings;
    CHECK(tcgetattr(slave, &settings) == 0);
    cfmakeraw(&settings);
    CHECK(tcsetattr(slave, TCSANOW, &settings) == 0);
    CHECK(fcntl(pty.fd, F_SETFL, O_NONBLOCK) == 0);
    CHECK(fcntl(slave, F_SETFL, O_NONBLOCK) == 0);
    const size_t size = 1024 * 1024;
    char *expected = malloc(size), *received = malloc(size);
    CHECK(expected && received);
    for (size_t i = 0; i < size; i++) expected[i] = (char)i;
    pty_write(&pty, expected, size / 2);
    pty_write(&pty, expected + size / 2, size / 2);
    CHECK(!pty.failed && pty.output.start < pty.output.end);
    size_t count = 0;
    for (int i = 0; i < 10000 && count < size; i++) {
        CHECK(wire_buffer_flush(&pty.output, pty.fd));
        ssize_t n = read(slave, received + count, size - count);
        if (n > 0) count += (size_t)n;
        else {
            CHECK(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
            struct pollfd ready = { .fd = slave, .events = POLLIN };
            CHECK(poll(&ready, 1, 1000) > 0);
        }
    }
    CHECK(count == size && memcmp(expected, received, size) == 0);
    free(expected);
    free(received);
    free(pty.output.data);
    close(slave);
    close(pty.fd);
}

static void test_validation(void)
{
    InputWire input = {0};
    const uint8_t invalid[] = { 0xc1 };
    CHECK(!input_decode(invalid, sizeof(invalid), &input));
    // Previously a 65536-column resize silently narrowed to zero.
    const uint8_t resize[] = {
        0x91, 0x98, INPUT_RESIZE, 0xce, 0, 1, 0, 0, 24, 10, 20, 80, 24, 0
    };
    CHECK(!input_decode(resize, sizeof(resize), &input));
    InputWire full = { .len = MAX_WIRE_QUEUE };
    CHECK(!input_append(&full, INPUT_PING, NULL, 0));
    InputKey key = { .text_len = 65 };
    CHECK(input_append(&input, INPUT_KEY, &key, sizeof(key)));
    msgpack_sbuffer packed;
    msgpack_sbuffer_init(&packed);
    CHECK(!input_pack(&input, &packed));
    msgpack_sbuffer_destroy(&packed);
    free(input.data);

    // Reject image dimensions before the GTK client computes signed strides.
    RenderSnapshotWire wire = {0}, decoded = {0};
    SnapshotImage image = { .image_w = UINT32_MAX, .image_h = 1 };
    CHECK(snapshot_record(&wire, SNAPSHOT_IMAGE, &image, sizeof(image)));
    msgpack_sbuffer_init(&packed);
    CHECK(snapshot_pack(&wire, &packed));
    CHECK(!snapshot_decode(packed.data, packed.size, &decoded));
    msgpack_sbuffer_destroy(&packed);
    free(wire.data);
    free(decoded.data);
}

static void test_graphemes(void)
{
    GhosttyTerminal terminal = NULL;
    GhosttyRenderState state = NULL;
    GhosttyRenderStateRowIterator rows = NULL;
    GhosttyRenderStateRowCells cells = NULL;
    CHECK(ghostty_terminal_new(NULL, &terminal, 4, 2) == GHOSTTY_SUCCESS);
    CHECK(ghostty_render_state_new(NULL, &state) == GHOSTTY_SUCCESS);
    CHECK(ghostty_render_state_row_iterator_new(NULL, &rows) == GHOSTTY_SUCCESS);
    CHECK(ghostty_render_state_row_cells_new(NULL, &cells) == GHOSTTY_SUCCESS);
    ServerTheme theme = {0};
    RenderSnapshotWire wire = {0};
    for (int marks = 20; marks <= 40; marks += 20) {
        ghostty_terminal_vt_write(terminal, (const uint8_t *)"\x1b[2J\x1b[H", 7);
        char text[81] = "A";
        for (int i = 0; i < marks; i++) {
            text[1 + 2 * i] = '\xcc';
            text[2 + 2 * i] = '\x81';
        }
        ghostty_terminal_vt_write(terminal, (const uint8_t *)text, 1 + 2 * marks);
        CHECK(ghostty_render_state_update(state, terminal) == GHOSTTY_SUCCESS);
        wire.len = 0;
        CHECK(serialize_render_snapshot(&wire, state, rows, cells,
            GHOSTTY_RENDER_STATE_DIRTY_FULL, NULL, terminal, NULL, &theme, 0));
        bool found = false;
        for (size_t offset = 0; offset < wire.len;) {
            SnapshotRecord record;
            memcpy(&record, wire.data + offset, sizeof(record));
            offset += sizeof(record);
            if (record.kind == SNAPSHOT_ROW) {
                SnapshotRow row;
                memcpy(&row, wire.data + offset, sizeof(row));
                if (row.row == 0 && row.cell_count) {
                    SnapshotCell cell;
                    memcpy(&cell, wire.data + offset + sizeof(row), sizeof(cell));
                    size_t expected = marks == 20 ? 41 : 3;
                    CHECK(strlen(cell.text) == expected);
                    CHECK(memcmp(cell.text, marks == 20 ? text : "\xef\xbf\xbd",
                                 expected) == 0);
                    found = true;
                }
            }
            offset += record.size;
        }
        CHECK(found);
    }
    free(wire.data);
    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(rows);
    ghostty_render_state_free(state);
    ghostty_terminal_free(terminal);
}

static void test_kitty_images(void)
{
    const char *commands[] = {
        "\x1b_Ga=T,f=24,s=1,v=1,i=1,q=2;/wAA\x1b\\",
        "\x1b_Ga=T,f=32,s=1,v=1,i=1,q=2;/wAAgA==\x1b\\",
    };
    for (size_t format = 0; format < 2; format++) {
        GhosttyTerminal terminal = NULL;
        GhosttyKittyGraphics graphics = NULL;
        GhosttyKittyGraphicsPlacementIterator placements = NULL;
        CHECK(ghostty_terminal_new(NULL, &terminal, 4, 2) == GHOSTTY_SUCCESS);
        CHECK(ghostty_terminal_resize(terminal, 4, 2, 10, 20) == GHOSTTY_SUCCESS);
        uint64_t limit = 1024;
        CHECK(ghostty_terminal_set(terminal,
            GHOSTTY_TERMINAL_OPT_KITTY_IMAGE_STORAGE_LIMIT, &limit) == GHOSTTY_SUCCESS);
        ghostty_terminal_vt_write(terminal, (const uint8_t *)commands[format],
                                 strlen(commands[format]));
        CHECK(ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_KITTY_GRAPHICS,
                                  &graphics) == GHOSTTY_SUCCESS);
        CHECK(graphics != NULL);
        CHECK(ghostty_kitty_graphics_placement_iterator_new(NULL, &placements)
              == GHOSTTY_SUCCESS);
        RenderSnapshotWire wire = {0};
        CHECK(snapshot_kitty_images(&wire, terminal, graphics, placements,
                                     GHOSTTY_KITTY_PLACEMENT_LAYER_ABOVE_TEXT));
        CHECK(wire.len == sizeof(SnapshotRecord) + sizeof(SnapshotImage) + 4);
        SnapshotImage image;
        memcpy(&image, wire.data + sizeof(SnapshotRecord), sizeof(image));
        CHECK(image.image_w == 1 && image.image_h == 1 && image.pixel_len == 4);
        const uint8_t expected[] = { 255, 0, 0, format == 0 ? 255 : 128 };
        CHECK(memcmp(wire.data + sizeof(SnapshotRecord) + sizeof(image),
                     expected, sizeof(expected)) == 0);
        free(wire.data);
        ghostty_kitty_graphics_placement_iterator_free(placements);
        ghostty_terminal_free(terminal);
    }
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    test_socket_backpressure();
    test_pty_backpressure();
    test_validation();
    test_graphemes();
    test_kitty_images();
    puts("transport: fragmented frames, PTY backpressure, validation, long graphemes, Kitty RGB/RGBA passed");
    return 0;
}
