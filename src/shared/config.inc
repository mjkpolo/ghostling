// Create the same self-documenting starter configuration on the client and
// server. Each side reads only the keys it owns, but keeping one format makes
// copying a configuration between machines unsurprising.
static bool gmux_mkdirs(char *path)
{
    for (char *slash = path + 1; (slash = strchr(slash, '/')); slash++) {
        *slash = '\0';
        if (mkdir(path, 0700) < 0 && errno != EEXIST) {
            *slash = '/';
            return false;
        }
        *slash = '/';
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static bool gmux_config_path(char *out, size_t size)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && *base)
        return snprintf(out, size, "%s/gmux/config", base) < (int)size;
    const char *home = getenv("HOME");
    return home && *home
        && snprintf(out, size, "%s/.config/gmux/config", home) < (int)size;
}

static void gmux_ensure_default_config(void)
{
    static const char defaults[] =
        "# gmux configuration\n"
        "# Font settings are used by gmux; themes are used by gmux-server.\n"
        "font = Monaspace Argon Frozen\n"
        "font-size = 24\n"
        "# theme = Catppuccin Frappe\n";
    char path[PATH_MAX];
    if (!gmux_config_path(path, sizeof(path))) return;
    char directory[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s", path)
            >= (int)sizeof(directory)) return;
    char *slash = strrchr(directory, '/');
    if (!slash) return;
    *slash = '\0';
    if (!gmux_mkdirs(directory)) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return;
    const char *cursor = defaults;
    size_t remaining = sizeof(defaults) - 1;
    while (remaining) {
        ssize_t written = write(fd, cursor, remaining);
        if (written > 0) {
            cursor += written;
            remaining -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    close(fd);
}
