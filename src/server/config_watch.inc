#ifdef GMUX_SERVER
// Watch the directory: editors commonly save by renaming a replacement file.
// Watching just the original inode would silently stop after the first save.
static int server_config_watch(void)
{
    char directory[PATH_MAX];
    if (!gmux_config_dir(directory, sizeof(directory))) return -1;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    if (inotify_add_watch(fd, directory, IN_CLOSE_WRITE | IN_MOVED_TO) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool server_config_changed(int fd)
{
    union {
        struct inotify_event alignment;
        char bytes[4096];
    } buffer;
    bool changed = false;
    ssize_t count;
    while ((count = read(fd, buffer.bytes, sizeof(buffer.bytes))) > 0) {
        for (size_t offset = 0; offset < (size_t)count;) {
            const struct inotify_event *event =
                (const struct inotify_event *)(buffer.bytes + offset);
            changed |= (event->mask & IN_Q_OVERFLOW)
                || (event->len && strcmp(event->name, "config") == 0);
            offset += sizeof(*event) + event->len;
        }
    }
    return changed;
}
#endif
