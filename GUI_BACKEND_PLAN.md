# GUI and Keyboard Backend Plan

Research revisions:

- Ghostty: `76895d97b74ff6b24c2b1543bcd69ccc18048a4d`
- Kitty: `3d06c27a76526306ec003c1fbdce38718c844238`

## Conclusion

Use a small native GUI shell per operating system:

- Linux: GTK4/GDK, preferring its Wayland backend and retaining its X11
  fallback.
- macOS: AppKit.
- Shared C code: socket protocol, render snapshot cache, terminal drawing, and
  a platform-neutral `GmuxKeyEvent`.

This is the same boundary Ghostty uses. It provides the complete event data
needed by libghostty's key encoder and delegates keyboard layouts, modifier
remaps, dead keys, compose sequences, and input methods to the native platform.

Keep the key encoder on `gmux-server`. The client should send normalized key
events; the server combines them with the authoritative terminal modes and
encodes the appropriate legacy or Kitty keyboard sequence.

## Why Raylib is the current limitation

The current client polls a fixed list of Raylib key constants and separately
drains `GetCharPressed()`. It then guesses that Shift was consumed and computes
the unshifted codepoint from a hard-coded US layout.

Raylib exposes press, repeat, release, and a Unicode character queue, but it
does not expose one coherent event containing:

- the physical key;
- the logical/remapped key;
- modifiers and consumed modifiers;
- committed or composing UTF-8 text;
- the unshifted layout-dependent codepoint;
- IME state.

That separation is why character text can become associated with the wrong key,
non-US layouts cannot be represented faithfully, and compositor/XKB remaps can
be lost. Native Wayland alone fixes only part of this.

Escape exits because Raylib makes Escape its default exit key. Calling
`SetExitKey(KEY_NULL)` immediately after `InitWindow` fixes that independently
of the backend migration.

## The event contract

Ghostty's internal `KeyEvent` is the right model for the client/server boundary:

```c
typedef struct {
    GhosttyKey key;                    // physical key after eligible OS remaps
    GhosttyKeyAction action;           // press, repeat, release
    GhosttyMods mods;
    GhosttyMods consumed_mods;
    uint32_t unshifted_codepoint;
    bool composing;
    uint32_t text_len;
    // followed by text_len bytes of UTF-8
} GmuxKeyEvent;
```

`InputKey` already contains every field except `composing`, and currently uses
a fixed 64-byte text buffer. Move the text to variable-length MessagePack data
when IME support is added so a committed composition is never truncated.

For Caps Lock remapped to Control, the Linux adapter must honor the logical XKB
keyval. Ghostty explicitly remaps eligible physical keys from GDK's keyval and
turns modifier keyvals it cannot represent into `unidentified`; it does not
blindly report the original physical Caps Lock key.

## What Ghostty does

### Linux

Ghostty receives `keyval`, hardware `keycode`, and modifier state from
`GtkEventControllerKey`. Its GTK adapter then:

1. passes every press and release through `GtkIMContext`;
2. obtains the layout-specific unshifted codepoint from GDK's keymap;
3. maps the hardware keycode to a physical W3C-style key;
4. applies logical XKB remapping, including modifier remaps;
5. reads the consumed modifiers from `GdkKeyEvent`;
6. sends press/repeat/release, UTF-8, composing state, and all key metadata to
   the Ghostty core.

GTK chooses Wayland or X11 at runtime. This gives gmux native Wayland under Sway
without deleting X11 compatibility.

Relevant source:

- `../ghostty/src/apprt/gtk/class/surface.zig`, `Surface.keyEvent`
- `../ghostty/src/apprt/gtk/key.zig`, `keyvalUnicodeUnshifted`, `eventMods`, and
  `remapKey`
- `../ghostty/src/input/key.zig`, `KeyEvent`

### macOS

Ghostty uses an `NSView` and handles `keyDown`, `keyUp`, and `flagsChanged`.
`interpretKeyEvents` supplies dead-key and IME behavior. It translates the
physical key code, modifier sides, generated text, repeat state, and unshifted
form before calling the same core key path.

Relevant source:

- `../ghostty/macos/Sources/Ghostty/Surface View/SurfaceView_AppKit.swift`
- `../ghostty/macos/Sources/Ghostty/NSEvent+Extension.swift`

## What Kitty does

Kitty uses its own substantially extended GLFW fork. It builds separate Cocoa,
Wayland, and X11 modules and selects one at runtime. Its `GLFWkeyevent` adds the
data missing from upstream GLFW: shifted and alternate keys, platform-native
key identity, UTF-8 text, IME state, physical native keycode, and synthesized
event tracking.

On Linux, both its Wayland and X11 paths use shared libxkbcommon code. That code
reads the active keymap, compose state, modifiers, UTF-8 output, default and
shifted symbols, and preserves press/repeat/release. Kitty also maintains its
own IBus and Wayland text-input integration.

Relevant source:

- `../kitty/kitty/glfw-wrapper.h`, `GLFWkeyevent`
- `../kitty/glfw/xkb_glfw.c`, `glfw_xkb_handle_key_event`
- `../kitty/kitty/key_encoding.c`, `encode_glfw_key_event`
- `../kitty/kitty/main.py`, `init_glfw`
- `../kitty/setup.py`, `compile_glfw`

## Backend choices

| Choice | Advantages | Costs and gaps | Verdict |
|---|---|---|---|
| Raylib with native Wayland | Smallest immediate change; likely fixes the Sway XKB remap; preserves the renderer | Still loses consumed modifiers, coherent text/key association, and robust IME data; does not provide full Kitty keyboard input | Useful diagnostic only |
| Upstream GLFW | C, compact, supports Cocoa, Wayland, and X11 | Ghostty itself documents that GLFW cannot populate the full key event; IME and consumed-modifier handling remain incomplete | Do not choose |
| Kitty's GLFW fork | One C API across macOS/Linux; proven by Kitty; richest cross-platform event object | Large private fork; Kitty-specific build and dynamic module system; ongoing merges from Kitty; integrating it underneath Raylib would require patching Raylib or replacing Raylib's window core | Strong prototype alternative |
| SDL3 | One maintained cross-platform API; physical scancode, layout keycode, raw code, modifiers, text editing/input, Cocoa/Wayland/X11 | Does not directly provide GDK-style consumed modifiers; text input can suppress key events on some platforms; neither studied terminal uses it for this path, so protocol completeness needs a spike | Worth a later experiment, not the correctness baseline |
| GTK4 on Linux + AppKit on macOS | Best layout, remap, IME, compose, and accessibility behavior; follows Ghostty's proven boundary; native Wayland and macOS behavior | Two small platform adapters; GTK runtime/package dependency on Linux; current Raylib drawing must be separated from Raylib's window ownership | Recommended |

## Implementation sequence

## Raylib to Ghostty-style GTK mapping

The first Linux migration keeps the existing render snapshot and replaces each
Raylib responsibility directly. It does not introduce a general window or
renderer abstraction.

| Current Raylib behavior | GTK/Ghostty-style replacement | Ownership |
|---|---|---|
| `InitWindow`, resize flags, `WindowShouldClose` | `GtkApplication`, `GtkApplicationWindow`, normal close signal | client |
| Raylib's GLFW/X11 event polling | GTK event controllers through GDK's native Wayland or X11 backend | client |
| Scan every known key each frame | `GtkEventControllerKey` press/release callbacks | client |
| Infer modifiers using `IsKeyDown` | GDK modifier state plus consumed modifiers from the actual key event | client |
| `GetCharPressed` | GTK input-method commit/preedit path | client |
| `GetMouse*` polling | motion, click, and scroll event controllers | client |
| `IsWindowFocused` polling | focus property notification | client |
| `SetWindowTitle` | `gtk_window_set_title` | client |
| `LoadRenderTexture` frame cache | GTK drawing-area backing/cache; queue a draw only when state or size changes | client |
| `DrawRectangle` | Cairo rectangle/fill | client |
| `DrawTextEx` with embedded Monaspace | Pango/Cairo with the bundled Regular and Italic faces registered for the application | client |
| `LoadTextureFromImage` and `DrawTexturePro` | Cairo image surface initially; a GTK-hosted GPU renderer can replace it after correctness | client |
| `BeginDrawing`/`EndDrawing` at 60 Hz | GTK main loop sleeps until a GUI or socket event occurs | client |

This follows Ghostty's important boundary rather than copying its Zig classes:
GDK produces a complete native event, the client serializes that event, and the
server asks libghostty to encode it using the terminal application's current
modes.

### Kitty keyboard additions

- Add `composing` to `InputKey` and MessagePack.
- Populate key identity, action, modifiers, consumed modifiers, unshifted
  codepoint, and committed UTF-8 from the GTK event/IME path.
- Preserve press, repeat, and release over the socket.
- Do not add a user configuration toggle. The application in the PTY selects
  Kitty keyboard flags; `ghostty_key_encoder_setopt_from_terminal` reads them
  immediately before encoding each event on the server.
- Add tests for Ctrl+I versus Tab, Escape, repeat/release reporting, Caps-as-Ctrl
  under Sway, shifted punctuation, compose, and IME input.

### OSC 52 additions

- Register libghostty's clipboard-write effect on the server.
- Copy the callback's borrowed location/MIME/data into a MessagePack
  `CLIPBOARD_WRITE` server-to-client record before returning.
- Apply it to `GdkClipboard` in the GTK client. Normal selection copy remains a
  purely client-side operation.
- Defer remote clipboard reads until there is an explicit prompt and an
  asynchronous design; never block libghostty's synchronous callback on a
  socket round trip.

### Build and compatibility

- Build Linux artifacts inside the pinned `rockylinux/rockylinux:9.4` image so
  binaries target glibc 2.34.
- Link vendored MessagePack and libghostty into the binaries.
- Keep the headless server dependent only on the glibc/libm family.
- Record `ldd`, ELF interpreter, and GLIBC symbol requirements in the CI
  artifact. GTK is distributed as normal shared client dependencies because
  Rocky does not ship a practical static GTK stack.

### 1. Fix Escape and add an input inspector

- Disable Raylib's Escape exit key.
- Add an opt-in client log showing each outgoing key event.
- Use it to capture Caps-as-Control, press/repeat/release, Shift punctuation,
  non-US layout input, dead keys, and an IME commit.

This creates a repeatable acceptance test before changing the backend.

### 2. Make the wire event complete

- Add `composing` to the key message.
- Encode UTF-8 as variable-length MessagePack data.
- Keep `GhosttyKey`, action, modifiers, consumed modifiers, and unshifted
  codepoint field-for-field.
- Continue calling `ghostty_key_encoder_setopt_from_terminal` immediately before
  an actual key event on the server.

### 3. Separate window/input from drawing

Extract the GUI-owned operations currently mixed into `run_client`:

- create/destroy window;
- poll/wait for events;
- resize and scale notification;
- title and focus;
- keyboard, mouse, paste, and close requests;
- create/present the render surface.

This should be a concrete `gmux_window` API with only the operations gmux uses,
not a general backend framework.

### 4. Implement Linux first with GTK4/GDK

- Use `GtkApplicationWindow`, `GtkEventControllerKey`, `GtkIMContext`, and a
  GTK drawing surface.
- Port Ghostty's key construction logic narrowly: hardware-key mapping,
  `remapKey`, consumed modifiers, unshifted codepoint, and IME handling.
- Let GDK select Wayland by default under Sway, with X11 fallback.
- Confirm the window is a native Wayland surface and Caps-as-Control arrives as
  Control rather than Caps Lock.

### 5. Implement AppKit

- Use a small Objective-C `.m` frontend so the shared application remains C.
- Handle `keyDown:`, `keyUp:`, `flagsChanged:`, and `NSTextInputClient`/
  `interpretKeyEvents:`.
- Translate those events into the same `GmuxKeyEvent` sent by Linux.
- Package the client as a normal `.app`; the server remains a plain Unix binary.

### 6. Replace the remaining Raylib coupling

The input migration and renderer migration meet at window ownership. Choose one
shared rendering layer after the keyboard path works:

- OpenGL with a small shared renderer, hosted by GTK and AppKit; or
- a small portable graphics layer with GTK/AppKit-native surfaces.

Do not retain Raylib solely for event polling. Its event API is the information
bottleneck this migration is intended to remove.

## Clipboard and OSC 52

Follow Ghostty's ownership split: libghostty on the server parses clipboard
escape sequences, while the GUI client owns the real desktop clipboard.

OSC 52 is application-driven clipboard access. It is separate from selecting
rendered terminal text and pressing the platform copy shortcut; that selection
and copy operation should remain entirely client-side.

### First version: remote application writes the local clipboard

Ghostling's pinned Ghostty revision already exposes
`GHOSTTY_TERMINAL_OPT_CLIPBOARD_WRITE`. When a remote application emits OSC 52,
libghostty validates and decodes it, then synchronously calls the registered
`GhosttyTerminalClipboardWriteFn`. Its strings and content arrays are borrowed
only for that callback, so the server must copy them into the outgoing message
before returning.

The wire path is deliberately small:

```text
remote application -> PTY -> libghostty OSC 52 parser
                   -> CLIPBOARD_WRITE(location, MIME contents)
                   -> client -> GDK clipboard / NSPasteboard
```

The client uses `GdkClipboard` on Linux and `NSPasteboard.general` on macOS.
Support the standard clipboard first, then GDK's primary selection on Linux.
The default policy should match current Ghostty: allow clipboard writes.

### Later: remote application reads the local clipboard

An OSC 52 query lets the remote program read the desktop clipboard, which can
exfiltrate local data. Match Ghostty and Kitty by asking the user before reads;
never silently grant them by default.

Current Ghostty exposes `GHOSTTY_TERMINAL_OPT_CLIPBOARD_READ`, but Ghostling's
pinned revision does not. More importantly, the current C callback contract
requires a reply before the callback returns and warns callbacks not to block.
A socket round trip cannot be inserted there cleanly. Do not fake this with a
blocking read inside the VT callback. When read support becomes a goal, either
upgrade/backport libghostty with an asynchronous embedding contract or add a
small deferred-query mechanism deliberately.

Therefore the staged policy is:

1. Implement OSC 52 writes, which covers programs copying text through the
   remote terminal into the local clipboard.
2. Implement normal selection copy locally in the client.
3. Add OSC 52 reads only with an explicit permission prompt and a reviewed
   asynchronous design.

This clipboard split is another reason to prefer Ghostty's GTK/AppKit design:
both native GUI backends already provide the clipboard and permission UI at the
same boundary that owns keyboard input and the window.

## Acceptance tests

- Escape reaches the terminal and does not close gmux.
- Sway `caps:ctrl_modifier` works in shells and applications.
- Press, repeat, and release work with Kitty's `report_event_types` flag.
- Shifted punctuation reports the correct base and shifted form.
- A non-US layout reports the right physical key, text, and unshifted codepoint.
- Dead-key composition and at least one IME work without duplicated text.
- macOS Option/Command/Control and left/right modifiers are distinguishable.
- Legacy applications still receive traditional sequences when Kitty keyboard
  flags are disabled.
- The exact same client events work after reconnecting to a remote server.
- An OSC 52 write from the remote shell reaches the local system clipboard.
- Selecting rendered text and copying works without contacting the server.
- An OSC 52 read cannot expose the local clipboard without confirmation.
