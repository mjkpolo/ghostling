#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <pwd.h>
#include <limits.h>

#include "../src/shared/config.inc"

int main(void)
{
    char temporary[] = "/tmp/gmux-config-test-XXXXXX";
    char *directory = mkdtemp(temporary);
    assert(directory);
    int result = setenv("XDG_CONFIG_HOME", directory, 1);
    assert(result == 0);
    result = setenv("HOME", directory, 1);
    assert(result == 0);
    gmux_load_config();
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/gmux/config", directory);
    struct stat info;
    result = stat(path, &info);
    assert(result == 0 && (info.st_mode & 0777) == 0600);
    assert(gmux_config.font_size == 24 && !*gmux_config.theme);

    const char custom[] =
        "# comment\n font = Test Font, monospace \nfont-size = 32\n"
        "theme = Test Theme\nunknown = ignored\n";
    FILE *file = fopen(path, "w");
    assert(file);
    size_t written = fwrite(custom, 1, sizeof(custom) - 1, file);
    assert(written == sizeof(custom) - 1);
    result = fclose(file);
    assert(result == 0);
    gmux_load_config();
    assert(strcmp(gmux_config.font, "Test Font, monospace") == 0);
    assert(gmux_config.font_size == 32);
    assert(strcmp(gmux_config.theme, "Test Theme") == 0);
    file = fopen(path, "r");
    assert(file);
    char actual[sizeof(custom)];
    size_t read = fread(actual, 1, sizeof(actual), file);
    assert(read == sizeof(custom) - 1);
    assert(memcmp(actual, custom, read) == 0);
    fclose(file);

    // Relative XDG paths are ignored, consistently on client and server.
    result = setenv("XDG_CONFIG_HOME", "relative-path", 1);
    assert(result == 0);
    char resolved[PATH_MAX], expected[PATH_MAX];
    bool found = gmux_config_dir(resolved, sizeof(resolved));
    snprintf(expected, sizeof(expected), "%s/.config/gmux", directory);
    assert(found && strcmp(resolved, expected) == 0);
    unlink(path);
    snprintf(path, sizeof(path), "%s/gmux", directory);
    rmdir(path);
    rmdir(directory);
    puts("config: defaults, shared parsing, existing-file preservation passed");
    return 0;
}
