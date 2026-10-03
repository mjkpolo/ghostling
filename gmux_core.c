#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pwd.h>
#include <limits.h>
#include <math.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#ifdef GMUX_CLIENT
#include <fontconfig/fontconfig.h>
#include <gtk/gtk.h>
#include <glib-unix.h>
#include <gdk/gdkkeysyms.h>
#endif
#include <ghostty/vt.h>
#include <msgpack.h>
#ifdef GMUX_SERVER
#include "server_image.h"
#endif

#ifdef GMUX_CLIENT
extern const unsigned char font_monaspace_argon[];
extern const unsigned char font_monaspace_argon_end[];
extern const unsigned char font_monaspace_argon_italic[];
extern const unsigned char font_monaspace_argon_italic_end[];
extern const unsigned char font_monaspace_argon_bold[];
extern const unsigned char font_monaspace_argon_bold_end[];
extern const unsigned char font_monaspace_argon_bold_italic[];
extern const unsigned char font_monaspace_argon_bold_italic_end[];
#define EMBEDDED_LEN(name) ((size_t)(name##_end - name))
#endif
#include "src/shared/config.inc"
// Keep the client and server as single translation units while grouping the
// implementation by responsibility. This avoids an internal API layer whose
// only purpose would be to split this small program across files.
#include "src/server/theme_pty.inc"
#include "src/shared/input_wire.inc"
#include "src/client/keymap.inc"
#include "src/server/utf8.inc"
#include "src/server/input.inc"
#include "src/shared/snapshot_wire.inc"
#include "src/client/snapshot_helpers.inc"
#include "src/server/snapshot.inc"
#include "src/client/render.inc"
#include "src/server/effects.inc"
#include "src/server/run.inc"
#include "src/client/setup.inc"
#include "src/client/selection.inc"
#include "src/client/input.inc"
#include "src/client/app.inc"

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    gmux_ensure_default_config();
#ifdef GMUX_SERVER
    if (argc == 3 && strcmp(argv[1], "--check") == 0)
        return check_server(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--kill") == 0)
        return kill_server_process(argv[2]);
    if (argc == 2) return daemonize_server(argv[1]);
    fprintf(stderr,
            "usage: %s SOCKET\n       %s --check SOCKET\n"
            "       %s --kill SOCKET\n", argv[0], argv[0], argv[0]);
#else
    if (argc == 3 && (strcmp(argv[1], "--kill") == 0 || strcmp(argv[1], "-kill") == 0))
        return kill_server(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--connect") == 0) {
        int socket_fd = unix_socket(argv[2], false);
        if (socket_fd < 0) { perror("connect to Unix socket"); return 1; }
        return run_client(socket_fd);
    }
    fprintf(stderr, "usage: %s --connect SOCKET\n       %s --kill SOCKET\n", argv[0], argv[0]);
#endif
    return 1;
}
