#ifdef GMUX_SERVER
// ---------------------------------------------------------------------------
// System callbacks (process-global, set once at startup)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Effects callbacks
// ---------------------------------------------------------------------------

// Context passed through the terminal's userdata pointer to all effect
// callbacks so they can reach the pty fd (and anything else they need)
// without global state.
typedef struct {
    Pty *pty;
    int cell_width;
    int cell_height;
    uint16_t cols;
    uint16_t rows;
    bool title_changed;
    char title[256];
    bool clipboard_changed;
    GhosttyClipboardLocation clipboard_location;
    uint8_t *clipboard_data;
    size_t clipboard_len;
    GhosttyColorScheme color_scheme;
} EffectsContext;

// write_pty effect — the terminal calls this whenever a VT sequence
// requires a response back to the application (device status reports,
// mode queries, device attributes, etc.).  Without this, programs like
// vim and tmux that probe terminal capabilities would hang.
static void effect_write_pty(GhosttyTerminal terminal, void *userdata,
                             const uint8_t *data, size_t len)
{
    (void)terminal;
    EffectsContext *ctx = (EffectsContext *)userdata;
    pty_write(ctx->pty, (const char *)data, len);
}

// size effect — responds to XTWINOPS size queries (CSI 14/16/18 t)
// so programs can discover the terminal geometry in cells and pixels.
static bool effect_size(GhosttyTerminal terminal, void *userdata,
                        GhosttySizeReportSize *out_size)
{
    (void)terminal;
    EffectsContext *ctx = (EffectsContext *)userdata;
    out_size->rows = ctx->rows;
    out_size->columns = ctx->cols;
    out_size->cell_width = (uint32_t)ctx->cell_width;
    out_size->cell_height = (uint32_t)ctx->cell_height;
    return true;
}

// device_attributes effect — responds to DA1/DA2/DA3 queries so
// terminal applications can identify the terminal's capabilities.
// We report VT220-level conformance with a modest feature set.
static bool effect_device_attributes(GhosttyTerminal terminal, void *userdata,
                                     GhosttyDeviceAttributes *out_attrs)
{
    (void)terminal;
    (void)userdata;

    // DA1: VT220-level with a few common features.
    out_attrs->primary.conformance_level = GHOSTTY_DA_CONFORMANCE_VT220;
    out_attrs->primary.features[0] = GHOSTTY_DA_FEATURE_COLUMNS_132;
    out_attrs->primary.features[1] = GHOSTTY_DA_FEATURE_SELECTIVE_ERASE;
    out_attrs->primary.features[2] = GHOSTTY_DA_FEATURE_ANSI_COLOR;
    out_attrs->primary.num_features = 3;

    // DA2: VT220-type, version 1, no ROM cartridge.
    out_attrs->secondary.device_type = GHOSTTY_DA_DEVICE_TYPE_VT220;
    out_attrs->secondary.firmware_version = 1;
    out_attrs->secondary.rom_cartridge = 0;

    // DA3: arbitrary unit id.
    out_attrs->tertiary.unit_id = 0;

    return true;
}

// xtversion effect — responds to CSI > q with our application name.
static GhosttyString effect_xtversion(GhosttyTerminal terminal, void *userdata)
{
    (void)terminal;
    (void)userdata;
    return (GhosttyString){ .ptr = (const uint8_t *)"gmux", .len = 4 };
}

// Record OSC 0 / OSC 2 title changes for the next server snapshot.
static void effect_title_changed(GhosttyTerminal terminal, void *userdata)
{
    EffectsContext *ctx = userdata;
    GhosttyString title = {0};
    if (ghostty_terminal_get(terminal, GHOSTTY_TERMINAL_DATA_TITLE, &title) != GHOSTTY_SUCCESS)
        return;

    size_t len = title.len < sizeof(ctx->title) - 1
        ? title.len : sizeof(ctx->title) - 1;
    memcpy(ctx->title, title.ptr, len);
    ctx->title[len] = '\0';
    ctx->title_changed = true;
}

static GhosttyClipboardWriteResult effect_clipboard_write(
    GhosttyTerminal terminal, void *userdata,
    const GhosttyClipboardWrite *write)
{
    (void)terminal;
    EffectsContext *ctx = userdata;
    const GhosttyString *text = NULL;
    for (size_t i = 0; i < write->contents_len; i++) {
        GhosttyString mime = write->contents[i].mime;
        if (mime.len >= 10
            && memcmp(mime.ptr, "text/plain", 10) == 0) {
            text = &write->contents[i].data;
            break;
        }
    }
    if (write->contents_len && !text)
        return GHOSTTY_CLIPBOARD_WRITE_RESULT_UNSUPPORTED;

    size_t len = text ? text->len : 0;
    if (len > MAX_WIRE_MESSAGE - sizeof(SnapshotClipboard))
        return GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY;
    uint8_t *copy = malloc(len + 1);
    if (!copy) return GHOSTTY_CLIPBOARD_WRITE_RESULT_BUSY;
    if (len) memcpy(copy, text->ptr, len);
    copy[len] = '\0';
    free(ctx->clipboard_data);
    ctx->clipboard_data = copy;
    ctx->clipboard_len = len;
    ctx->clipboard_location = write->location;
    ctx->clipboard_changed = true;
    return GHOSTTY_CLIPBOARD_WRITE_RESULT_SUCCESS;
}

// Report the server-owned terminal theme, not the client's desktop theme.
static bool effect_color_scheme(GhosttyTerminal terminal, void *userdata,
                                GhosttyColorScheme *out_scheme)
{
    (void)terminal;
    EffectsContext *ctx = userdata;
    *out_scheme = ctx->color_scheme;
    return true;
}

// ---------------------------------------------------------------------------
#endif
