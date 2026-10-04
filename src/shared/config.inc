// Only the server loads configuration. Management commands never create files.
static struct {
    char font[128];
    int font_size;
    char theme[256];
    unsigned font_line, theme_line;
    char path[PATH_MAX];
    char error[4096];
    unsigned revision;
} gmux_config = { .font = "Monaspace Argon Frozen, monospace", .font_size = 24 };

static void config_error(const char *path, unsigned line, const char *key,
                         const char *value, const char *reason)
{
    size_t used = strlen(gmux_config.error);
    snprintf(gmux_config.error + used, sizeof(gmux_config.error) - used,
             "%s:%u\n%s = %s\n%s\n\n", path, line, key, value, reason);
}

static char *trim_ascii(char *value)
{
    while (*value == ' ' || *value == '\t') value++;
    char *end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t'
                           || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return value;
}

static bool gmux_config_dir(char *out, size_t size)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        return snprintf(out, size, "%s/gmux", xdg) < (int)size;
    const char *home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }
    return home && *home
        && snprintf(out, size, "%s/.config/gmux", home) < (int)size;
}

static bool gmux_mkdirs(char *path)
{
    for (char *slash = path + 1; (slash = strchr(slash, '/')); slash++) {
        *slash = '\0';
        int result = mkdir(path, 0700);
        *slash = '/';
        if (result < 0 && errno != EEXIST) return false;
    }
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

static void gmux_load_config(void)
{
    char directory[PATH_MAX], path[PATH_MAX];
    if (!gmux_config_dir(directory, sizeof(directory))
        || snprintf(path, sizeof(path), "%s/config", directory)
            >= (int)sizeof(path)) return;
    FILE *file = fopen(path, "r");
    snprintf(gmux_config.path, sizeof(gmux_config.path), "%s", path);
    gmux_config.error[0] = '\0';
    gmux_config.revision++;
    if (!file && errno == ENOENT && gmux_mkdirs(directory)) {
        // Exclusive creation preserves existing files, including another
        // gmux process's configuration if both start for the first time.
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            file = fdopen(fd, "w");
            if (!file) {
                close(fd);
                unlink(path);
                return;
            }
            bool written = fprintf(file,
                "# gmux configuration: key = value\n"
                "# Font installed on the client (comma-separated fallbacks).\n"
                "font = %s\n"
                "# Font size in points, from 6 to 96.\n"
                "font-size = %d\n"
                "# Server: empty uses libghostty's default colors.\n"
                "# Use a theme name or absolute path; e.g. Catppuccin Frappe.\n"
                "theme =\n", gmux_config.font, gmux_config.font_size) >= 0;
            if (fclose(file) != 0) written = false;
            if (!written) {
                perror("write gmux config");
                unlink(path);
            }
            if (written) gmux_load_config();
            return;
        }
        file = fopen(path, "r");
    }
    if (!file) {
        perror(path);
        config_error(path, 0, "config", path, strerror(errno));
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    // Removing a setting on reload restores its default rather than retaining
    // the previous file's value.
    strcpy(gmux_config.font, "Monaspace Argon Frozen, monospace");
    gmux_config.font_size = 24;
    gmux_config.theme[0] = '\0';
    gmux_config.font_line = gmux_config.theme_line = 0;
    unsigned line_number = 0;
    while (getline(&line, &capacity, file) >= 0) {
        line_number++;
        char *key = trim_ascii(line);
        if (!*key || *key == '#') continue;
        char *value = strchr(key, '=');
        if (!value) {
            config_error(path, line_number, "config", key, "Expected key = value");
            continue;
        }
        *value++ = '\0';
        key = trim_ascii(key);
        value = trim_ascii(value);
        if (strcmp(key, "font") == 0 && *value
            && strlen(value) < sizeof(gmux_config.font)) {
            strcpy(gmux_config.font, value);
            gmux_config.font_line = line_number;
        } else if (strcmp(key, "font-size") == 0) {
            char *end;
            long size = strtol(value, &end, 10);
            if (end != value && *end == '\0' && size >= 6 && size <= 96) {
                gmux_config.font_size = (int)size;
            } else config_error(path, line_number, key, value,
                                "Expected an integer from 6 to 96");
        } else if (strcmp(key, "theme") == 0
                   && strlen(value) < sizeof(gmux_config.theme)) {
            strcpy(gmux_config.theme, value);
            gmux_config.theme_line = line_number;
        } else config_error(path, line_number, key, value,
                            "Unknown option, empty font, or value too long");
    }
    free(line);
    if (ferror(file)) config_error(path, line_number, "config", path, "Cannot read config file");
    fclose(file);
}
