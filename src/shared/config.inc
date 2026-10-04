// Both binaries use the same file and defaults. Parse once when launching a
// terminal, never as a side effect of --check, --kill, or printing usage.
static struct {
    char font[128];
    int font_size;
    char theme[256];
} gmux_config = { .font = "Monaspace Argon Frozen, monospace", .font_size = 24 };

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
    if (xdg && *xdg == '/')
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
                "# Client: installed font family (comma-separated fallbacks).\n"
                "font = %s\n"
                "# Client: font size in points, from 6 to 96.\n"
                "font-size = %d\n"
                "# Server: empty uses libghostty's default colors.\n"
                "# Use a theme name or absolute path; e.g. Catppuccin Frappe.\n"
                "theme =\n", gmux_config.font, gmux_config.font_size) >= 0;
            if (fclose(file) != 0) written = false;
            if (!written) {
                perror("write gmux config");
                unlink(path);
            }
            return;
        }
        file = fopen(path, "r");
    }
    if (!file) {
        perror(path);
        return;
    }
    char *line = NULL;
    size_t capacity = 0;
    while (getline(&line, &capacity, file) >= 0) {
        char *key = trim_ascii(line);
        if (!*key || *key == '#') continue;
        char *value = strchr(key, '=');
        if (!value) continue;
        *value++ = '\0';
        key = trim_ascii(key);
        value = trim_ascii(value);
        if (strcmp(key, "font") == 0 && *value
            && strlen(value) < sizeof(gmux_config.font)) {
            strcpy(gmux_config.font, value);
        } else if (strcmp(key, "font-size") == 0) {
            char *end;
            long size = strtol(value, &end, 10);
            if (end != value && *end == '\0' && size >= 6 && size <= 96)
                gmux_config.font_size = (int)size;
        } else if (strcmp(key, "theme") == 0
                   && strlen(value) < sizeof(gmux_config.theme)) {
            strcpy(gmux_config.theme, value);
        }
    }
    free(line);
    fclose(file);
}
