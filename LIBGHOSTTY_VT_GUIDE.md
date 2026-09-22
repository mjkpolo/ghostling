# libghostty-vt guide for Ghostling

This guide connects the upstream C API documentation shipped in
`build/_deps/ghostty-src/include/ghostty/vt/` to the calls traced in `main.c`.
The vendored headers are the authoritative API documentation; this file adds
Ghostling-specific sequencing and optimization notes.

## The three layers

```text
PTY bytes / user input
        |
        v
GhosttyTerminal             authoritative terminal + VT parser
        |
        | ghostty_render_state_update
        v
GhosttyRenderState           renderer-facing snapshot/cache
        |
        | row/cell/color/Kitty queries
        v
Raylib renderer
```

`GhosttyTerminal` is mutated by calls such as `ghostty_terminal_vt_write`,
`ghostty_terminal_resize`, and scrolling/input operations. `GhosttyRenderState`
is a rendering snapshot built from that terminal.

## Terminal mutation

### `ghostty_terminal_vt_write`

**Purpose:** Feed bytes produced by the shell/application into the VT parser.
Those bytes can change visible cells, cursor state, scrollback, colors, title,
Kitty graphics, and terminal modes such as mouse tracking or Kitty keyboard
mode.

**Important behavior:** Terminal callbacks may run synchronously during this
call. The header documents that callers must not recursively call
`ghostty_terminal_vt_write` on the same terminal from a callback.

**Ghostling use:** PTY output is read and passed here. This is the natural
server-side mutation boundary in a future client/server design.

**Why it matters / without it:** This turns an arbitrary byte stream into terminal meaning. Without it, `echo hi` would only be raw bytes: Ghostling would not know which characters occupy which cells, where the cursor moved, whether the screen was cleared, or whether an escape sequence enabled mouse or Kitty keyboard mode. The project would no longer contain a terminal emulator; the client would have to parse VT itself.

## Render-state synchronization

### `ghostty_render_state_update`

**Purpose:** Synchronize the renderer-facing snapshot from the authoritative
terminal. It consumes terminal dirty information and updates the render-state
cache.

**Current Ghostling use:** Called once per Raylib frame. This is why the current
implementation still enters libghostty while idle.

**Future event-driven use:** Call after PTY bytes, resize, scroll, or other
operations that may mutate the terminal, then inspect the resulting dirty
state and send/render an update only when needed.

**Why it matters / without it:** The authoritative terminal and renderer-facing snapshot are separate. Without this call, the terminal could correctly parse new output while the renderer continued to see an old screen forever. Calling it too often wastes work; failing to call it after mutation produces stale output.

### `ghostty_render_state_get`

**Purpose:** Query data owned by the render snapshot. Ghostling uses it for the
dirty state and other renderer data.

The dirty values mean:

- `false`: the previously rendered state is still valid.
- `partial`: some rows changed.
- `full`: global state or dimensions changed; redraw all rows.

The dirty query is cheap, but in the current frame-driven design it is still a
poll once per frame because `ghostty_render_state_update` is also called once
per frame.

**Why it matters / without it:** Ghostling would have no supported way to know whether its cached texture remains valid. It would need to redraw and traverse every cell every frame, or risk missing changes. In the network design, this is also the decision point for whether the server sends an update.

### `ghostty_render_state_colors_get`

**Purpose:** Read the current background/foreground color state from the render
snapshot.

**Cacheability:** Yes. Cache until dirty is non-false, or until a resize/theme
change requires a refresh.

**Why it matters / without it:** A hard-coded background makes terminal-controlled color changes incorrect and can leave gaps around glyphs or images in the wrong color.

### Row and cell iterators

The following calls expose the render snapshot's visible rows and cells:

- `ghostty_render_state_row_iterator_next`
- `ghostty_render_state_row_get`
- `ghostty_render_state_row_cells_next`
- `ghostty_render_state_row_cells_get`
- `ghostty_render_state_row_set`

**Purpose:** Traverse and read the terminal grid for drawing. These account for
the largest call volume in the traces.

**Cacheability:** Yes. Do not traverse rows/cells on a clean render state. For
`partial`, a future optimization can redraw only affected rows; the current
Ghostling implementation redraws the whole cached texture for any non-false
dirty value.

**Why they matter / without them:** These calls supply the actual grid: codepoints, styles, colors, widths, and row metadata. Without them the renderer has no text model. Replaying PTY bytes is not equivalent because cursor motion, erasure, wrapping, and overwriting have already transformed that stream into terminal state.

### `ghostty_terminal_get`

**Purpose:** Read terminal-owned data such as scrollbar geometry or other
metadata selected by a `GHOSTTY_TERMINAL_DATA_*` tag.

**Cacheability:** Usually yes, depending on the data tag. Scrollbar geometry
should be refreshed after scroll/resize/output changes, not every idle frame.

**Why it matters / without it:** Some state is terminal-owned rather than part of the grid. Without the scrollbar query, the UI cannot represent scrollback length or viewport position; stale values make dragging jump or target the wrong history row.

## Kitty graphics

The Kitty-related calls expose image state and placements rather than asking
the application to replay the original escape sequence:

- `ghostty_kitty_graphics_get`
- `ghostty_kitty_graphics_placement_iterator_set`
- `ghostty_kitty_graphics_placement_next`
- `ghostty_kitty_graphics_placement_get`
- `ghostty_kitty_graphics_placement_grid_size`
- `ghostty_kitty_graphics_placement_source_rect`
- `ghostty_kitty_graphics_placement_viewport_pos`
- `ghostty_kitty_graphics_image`
- `ghostty_kitty_graphics_image_get`

**Purpose:** Find visible image placements, their geometry/source rectangles,
and the image payload/metadata needed by the renderer.

**Cacheability:** Yes on clean frames. Kitty placements are part of the
rendered terminal state. Refresh them after image commands, scrolling,
resizing, or other dirty state—not once per display frame.

**Why they matter / without them:** Image pixels alone are insufficient. Ghostling needs the visible image, placement, crop rectangle, and terminal-cell anchor. Without these interfaces images disappear, render in the wrong place, or fail to move with scrolling. These are also the structured image objects a future server must send to its client.

## Keyboard encoding

### `ghostty_key_encoder_setopt_from_terminal`

**Purpose:** Copy terminal-mode-dependent keyboard options into the reusable
key encoder. These options include application cursor/keypad modes,
modify-other-keys, and Kitty keyboard flags.

**What it does not do:** It does not send bytes to the PTY and does not encode a
key by itself.

**Cacheability:** Cache until terminal modes may have changed. Refresh after
PTY output that could contain mode-setting sequences, or immediately before an
actual key event if using a simpler design.

**Why it matters / without it:** The same physical key requires different bytes under application cursor/keypad mode, modify-other-keys, or Kitty keyboard mode. Without synchronization, keys may work at a shell prompt but break in editors and multiplexers. Refreshing every frame is wasteful; leaving it stale across a mode change is incorrect.

### `ghostty_key_encoder_encode`

**Purpose:** Convert one structured key event into the correct terminal byte
sequence using the encoder's current options.

**When to call:** Only for an actual key event.

**Why it matters / without it:** Raylib supplies a structured key event while a PTY accepts bytes. This performs the protocol-aware conversion. Without it, Ghostling must reimplement legacy key sequences, application modes, modify-other-keys, and Kitty keyboard encoding.

## Mouse encoding

### `ghostty_mouse_encoder_setopt_from_terminal`

**Purpose:** Copy terminal mouse tracking/reporting mode and format into the
reusable mouse encoder.

**Cacheability:** Cache until terminal mode changes. It is not a render-frame
operation.

**Why it matters / without it:** Applications select whether they want mouse reports and which wire format they expect. Without synchronization, Ghostling can send mouse bytes to an ordinary shell, send the wrong format, or fail to report input after tracking is enabled.

### `ghostty_mouse_encoder_setopt`

**Purpose:** Set encoder configuration such as screen/cell geometry, button
state, and motion deduplication.

**Cacheability:** Set geometry on resize; set button/motion state when handling
mouse input. These values are independent of visual dirty state.

**Why it matters / without it:** Mouse protocols report cell or pixel coordinates. Current window, padding, and cell geometry are required to translate Raylib coordinates. Stale geometry makes clicks land in the wrong cell; button and motion settings distinguish clicks, drags, and redundant motion.

### `ghostty_mouse_event_set_mods` and `ghostty_mouse_event_set_position`

**Purpose:** Populate the current structured mouse event with modifiers and
pointer coordinates.

**When to call:** Only when there is a mouse event to encode. Pointer position
and modifiers may change while the terminal screen remains clean.

**Why they matter / without them:** Position selects the target cell and modifiers preserve combinations such as Shift-click and Ctrl-click. They originate in the local GUI and cannot be inferred from terminal dirty state.

### `ghostty_mouse_encoder_encode`

**Purpose:** Convert one structured mouse event into the appropriate VT mouse
sequence, or produce no output when tracking is disabled.

**When to call:** Only for an actual mouse event.

**Why it matters / without it:** This converts structured GUI input to the terminal mouse protocol expected by editors, TUIs, and multiplexers. Without it those applications cannot receive clicks, drags, wheel events, or motion; implementing it locally would duplicate several protocols and their edge cases.

## The current Ghostling issue in one picture

```text
Current frame loop:
    every frame:
        key_encoder_setopt_from_terminal      (input config churn)
        mouse_encoder_setopt_from_terminal    (input config churn)
        mouse_encoder_setopt                   (input config churn)
        mouse_event_set_position               (input object churn)
        mouse_event_set_mods                   (input object churn)
        render_state_update                    (dirty polling)
        render_state_get(DIRTY)                (dirty polling)

        if dirty:
            colors
            rows/cells
            Kitty placements/images
```

A better event-driven split is:

```text
PTY output:
    terminal_vt_write
    mark input options potentially stale
    render_state_update
    if dirty:
        serialize/render updated state

Key event:
    refresh key options if stale
    populate and encode one key event

Mouse event:
    refresh mouse options if stale
    update geometry/state if needed
    populate and encode one mouse event
```

## Header locations

The full upstream comments are in:

- `build/_deps/ghostty-src/include/ghostty/vt/terminal.h`
- `build/_deps/ghostty-src/include/ghostty/vt/render.h`
- `build/_deps/ghostty-src/include/ghostty/vt/key.h`
- `build/_deps/ghostty-src/include/ghostty/vt/key/encoder.h`
- `build/_deps/ghostty-src/include/ghostty/vt/mouse.h`
- `build/_deps/ghostty-src/include/ghostty/vt/mouse/encoder.h`
- `build/_deps/ghostty-src/include/ghostty/vt/kitty_graphics.h`
