#ifdef GMUX_SERVER
// ---------------------------------------------------------------------------
// PTY helpers
// ---------------------------------------------------------------------------

typedef struct {
    GhosttyColorRgb foreground, background, cursor;
    GhosttyColorRgb selection_foreground, selection_background;
    GhosttyColorRgb palette[256];
    GhosttyColorPaletteMask palette_mask;
    bool has_foreground, has_background, has_cursor, has_palette;
    bool has_selection_foreground, has_selection_background;
} ServerTheme;

static bool server_theme_path(const char *name, char *out, size_t size)
{
    if (*name == '/' && access(name, R_OK) == 0)
        return snprintf(out, size, "%s", name) < (int)size;

    char config_dir[PATH_MAX];
    if (gmux_config_dir(config_dir, sizeof(config_dir))
        && snprintf(out, size, "%s/themes/%s", config_dir, name) < (int)size
        && access(out, R_OK) == 0) return true;

    const char *theme_dir = getenv("GMUX_THEME_DIR");
    if (theme_dir && *theme_dir
        && snprintf(out, size, "%s/%s", theme_dir, name) < (int)size
        && access(out, R_OK) == 0) return true;

#if defined(__linux__)
    char executable[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", executable,
                           sizeof(executable) - 1);
    if (len > 0) {
        executable[len] = '\0';
        char *slash = strrchr(executable, '/');
        if (slash) {
            *slash = '\0';
            if (snprintf(out, size, "%s/themes/%s", executable, name)
                    < (int)size && access(out, R_OK) == 0) return true;
        }
    }
#endif

    const char *shared[] = {
        "/usr/local/share/gmux/themes", "/usr/share/gmux/themes"
    };
    for (size_t i = 0; i < sizeof(shared) / sizeof(shared[0]); i++)
        if (snprintf(out, size, "%s/%s", shared[i], name) < (int)size
            && access(out, R_OK) == 0) return true;
    return false;
}

static bool server_theme_load(ServerTheme *theme)
{
    char path[PATH_MAX];
    if (!*gmux_config.theme) return false;
    if (!server_theme_path(gmux_config.theme, path, sizeof(path))) {
        config_error(gmux_config.path, gmux_config.theme_line, "theme",
                     gmux_config.theme, "Theme not found or not readable");
        return false;
    }
    FILE *file = fopen(path, "r");
    if (!file) {
        config_error(gmux_config.path, gmux_config.theme_line, "theme",
                     gmux_config.theme, strerror(errno));
        return false;
    }

    ghostty_color_palette_default(theme->palette);
    char line[512];
    unsigned line_number = 0;
    while (fgets(line, sizeof(line), file)) {
        line_number++;
        char *entry = trim_ascii(line);
        if (!*entry || *entry == '#') continue;
        char *equals = strchr(entry, '=');
        if (!equals) continue;
        *equals++ = '\0';
        char *key = trim_ascii(entry);
        char *value = trim_ascii(equals);
        GhosttyColorRgb *color = NULL;
        bool *present = NULL;
        if (strcmp(key, "foreground") == 0) {
            color = &theme->foreground;
            present = &theme->has_foreground;
        } else if (strcmp(key, "background") == 0) {
            color = &theme->background;
            present = &theme->has_background;
        } else if (strcmp(key, "cursor-color") == 0) {
            color = &theme->cursor;
            present = &theme->has_cursor;
        } else if (strcmp(key, "selection-foreground") == 0) {
            color = &theme->selection_foreground;
            present = &theme->has_selection_foreground;
        } else if (strcmp(key, "selection-background") == 0) {
            color = &theme->selection_background;
            present = &theme->has_selection_background;
        } else if (strcmp(key, "palette") == 0) {
            uint8_t index;
            GhosttyColorRgb rgb;
            if (ghostty_color_parse_palette_entry(value, strlen(value),
                    &index, &rgb) == GHOSTTY_SUCCESS) {
                theme->palette[index] = rgb;
                GHOSTTY_COLOR_PALETTE_MASK_SET(&theme->palette_mask, index);
                theme->has_palette = true;
            } else config_error(path, line_number, key, value, "Invalid palette entry");
            continue;
        } else {
            continue;
        }
        *present = ghostty_color_parse(value, strlen(value), color)
            == GHOSTTY_SUCCESS;
        if (!*present) config_error(path, line_number, key, value, "Invalid color");
    }
    bool readable = !ferror(file);
    if (!readable) config_error(gmux_config.path, gmux_config.theme_line, "theme",
                                gmux_config.theme, "Cannot read theme file");
    fclose(file);
    if (!readable) return false;

    if (theme->has_palette) {
        GhosttyColorRgb background = theme->has_background
            ? theme->background : (GhosttyColorRgb){0, 0, 0};
        GhosttyColorRgb foreground = theme->has_foreground
            ? theme->foreground : (GhosttyColorRgb){255, 255, 255};
        ghostty_color_palette_generate(theme->palette, &theme->palette_mask,
            &background, &foreground, false, theme->palette);
    }
    return true;
}

static void server_theme_apply(GhosttyTerminal terminal,
                               const ServerTheme *theme)
{
    // RenderState updates foreground/background only when both are set. Supply
    // its white/black defaults explicitly, including when clearing a theme.
    // Application OSC overrides remain intact.
    GhosttyColorRgb foreground = {255, 255, 255}, background = {0, 0, 0};
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_FOREGROUND,
        theme->has_foreground ? &theme->foreground : &foreground);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_BACKGROUND,
        theme->has_background ? &theme->background : &background);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_CURSOR,
        theme->has_cursor ? &theme->cursor : NULL);
    ghostty_terminal_set(terminal, GHOSTTY_TERMINAL_OPT_COLOR_PALETTE,
        theme->has_palette ? theme->palette : NULL);
}

static bool server_terminfo_dir(char *out, size_t size)
{
    const char *configured = getenv("GMUX_TERMINFO");
    if (configured && *configured
        && snprintf(out, size, "%s", configured) < (int)size)
        return true;
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg == '/'
        && snprintf(out, size, "%s/gmux/terminfo", xdg) < (int)size)
        return true;
    const char *home = getenv("HOME");
    return home && *home
        && snprintf(out, size, "%s/.local/share/gmux/terminfo", home)
            < (int)size;
}

static bool server_has_ghostty_terminfo(char *directory, size_t size)
{
    if (!server_terminfo_dir(directory, size)) return false;
    char entry[PATH_MAX];
    return (snprintf(entry, sizeof(entry), "%s/x/xterm-ghostty", directory)
                < (int)sizeof(entry) && access(entry, R_OK) == 0)
        || (snprintf(entry, sizeof(entry), "%s/78/xterm-ghostty", directory)
                < (int)sizeof(entry) && access(entry, R_OK) == 0);
}

// Spawn the user's default shell in a new pseudo-terminal.
//
// Creates a pty pair via forkpty(), sets the initial window size, execs the
// shell in the child, and puts the master fd into non-blocking mode so we
// can service shell output and client events in the same poll loop.
//
// The shell is chosen by checking, in order:
//   1. $SHELL environment variable
//   2. The pw_shell field from the passwd database
//   3. /bin/sh as a last resort
//
// Returns the master fd on success (>= 0) and stores the child pid in
// *child_out.  Returns -1 on failure.
static int pty_spawn(pid_t *child_out, uint16_t cols, uint16_t rows,
                     int cell_width, int cell_height)
{
    int pty_fd;
    struct winsize ws = {
        .ws_row = rows,
        .ws_col = cols,
        .ws_xpixel = (unsigned short)(cols * cell_width),
        .ws_ypixel = (unsigned short)(rows * cell_height),
    };

    // forkpty() combines openpty + fork + login_tty into one call.
    // In the child it sets up the slave side as stdin/stdout/stderr.
    pid_t child = forkpty(&pty_fd, NULL, NULL, &ws);
    if (child < 0) {
        perror("forkpty");
        return -1;
    }
    if (child == 0) {
        // Determine the user's preferred shell.  We try $SHELL first (the
        // standard convention), then fall back to the passwd entry, and
        // finally to /bin/sh if nothing else is available.
        const char *shell = getenv("SHELL");
        if (!shell || shell[0] == '\0') {
            struct passwd *pw = getpwuid(getuid());
            if (pw && pw->pw_shell && pw->pw_shell[0] != '\0')
                shell = pw->pw_shell;
            else
                shell = "/bin/sh";
        }

        // Extract just the program name for argv[0] (e.g. "/bin/zsh" → "zsh").
        const char *shell_name = strrchr(shell, '/');
        shell_name = shell_name ? shell_name + 1 : shell;

        // Child process — replace ourselves with the shell.
        // TERM tells programs what escape sequences we understand.
        char terminfo[PATH_MAX];
        if (server_has_ghostty_terminfo(terminfo, sizeof(terminfo))) {
            setenv("TERM", "xterm-ghostty", 1);
            setenv("TERMINFO", terminfo, 1);
        } else {
            setenv("TERM", "xterm-256color", 1);
        }
        execl(shell, shell_name, NULL);
        _exit(127); // execl only returns on error
    }

    // Neither draining output nor a large paste may block the event loop.
    int flags = fcntl(pty_fd, F_GETFL);
    if (flags < 0 || fcntl(pty_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK");
        close(pty_fd);
        return -1;
    }

    *child_out = child;
    return pty_fd;
}

typedef struct {
    int fd;
    WireBuffer output;
    bool failed;
} Pty;

// Retain the unwritten suffix on EAGAIN. POLLOUT resumes it, preserving the
// ordering of pasted text, keys, and responses generated by the VT parser.
static void pty_write(Pty *pty, const char *buf, size_t len)
{
    if (pty->fd < 0) return;
    if (!pty->failed)
        pty->failed = !wire_buffer_append(&pty->output, buf, len)
            || !wire_buffer_flush(&pty->output, pty->fd);
}

// Result of draining the pty master fd.
typedef enum {
    PTY_READ_OK,    // data was drained (or EAGAIN, i.e. nothing available right now)
    PTY_READ_EOF,   // the child closed its end of the pty
    PTY_READ_ERROR, // a real read error occurred
} PtyReadResult;

// Drain all available output from the pty master and feed it into the
// ghostty terminal.  The terminal's VT parser will process any escape
// sequences and update its internal screen/cursor/style state.
//
// Because the fd is non-blocking, read() returns -1 with EAGAIN once
// the kernel buffer is empty, at which point we stop.
static PtyReadResult pty_read(int pty_fd, GhosttyTerminal terminal,
                              bool *terminal_changed)
{
    uint8_t buf[4096];
    // Bound a batch so continuous output cannot starve input or snapshots.
    for (size_t received = 0; received < 65536;) {
        ssize_t n = read(pty_fd, buf, sizeof(buf));
        if (n > 0) {
            received += (size_t)n;
            *terminal_changed = true;
            ghostty_terminal_vt_write(terminal, buf, (size_t)n);
        } else if (n == 0) {
            // EOF — the child closed its side of the pty.
            return PTY_READ_EOF;
        } else {
            // n == -1: distinguish "no data right now" from real errors.
            if (errno == EAGAIN)
                return PTY_READ_OK;
            if (errno == EINTR)
                continue; // retry the read
            // On Linux, the slave closing often produces EIO rather
            // than a clean EOF (read returning 0).  Treat it the same.
            if (errno == EIO)
                return PTY_READ_EOF;
            perror("pty read");
            return PTY_READ_ERROR;
        }
    }
    return PTY_READ_OK;
}

#endif
