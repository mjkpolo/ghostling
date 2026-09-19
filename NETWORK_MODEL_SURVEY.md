# What should cross the terminal boundary?

This report compares existing implementations and applies their lessons to a
two-process Ghostling prototype. It is based on local source checkouts, not only
project descriptions. The revisions inspected on 2026-09-19 are:

| Project | Local checkout | Revision |
| --- | --- | --- |
| Ghostling + vendored Ghostty | `.` and `build/_deps/ghostty-src` | Ghostling checkout plus vendored `f64f4aca2` |
| Zellij | `../zellij` | `474ea0cef620c83d6ec05a7c28c7f80c4f63bbbb` |
| WezTerm | `../wezterm` | `b09b56c29c1e367e598b60ca266e2cc9038751e0` |
| Mosh | `../mosh` | `decd9b705eb81626f694335b8d5940538beb06da` |
| tmux | `../tmux` | `e880cf63e0a9fe095d7c5d313761520fb1a8653c` |
| DomTerm | `../DomTerm` | `3df85cc4a509baaf11f97c0ea7c7c45f8ee7617e` |

## Comparison

| System | Authoritative terminal interpretation | What crosses the relevant boundary | Receiving side | History/images | Lesson for Ghostling |
| --- | --- | --- | --- | --- | --- |
| Traditional SSH | Client terminal | PTY/application byte stream | Parses VT and renders | Client scrollback; extensions depend on the client | Simple and universal, but leaves terminal interpretation on the desktop. |
| Zellij native client and web client | Zellij server has its own terminal model for panes | Server emits a `Render { content: String }`; the web client passes received data to `term.write(data)` | Another terminal parser renders the stream | Multiplexer owns pane state; browser uses xterm.js | A GUI does not remove the second terminal boundary if it consumes terminal bytes. See [IPC message](../zellij/zellij-utils/src/ipc.rs#L251) and [web client write](../zellij/zellij-client/assets/websockets.js#L147). |
| tmux control mode / iTerm2 | tmux parses application output into pane grids | Control messages plus `%output`, whose payload is application output escaped for the control stream | Integrated terminal interprets pane output | tmux can provide captured history; images remain constrained by the terminal stream | Native pane management can coexist with a byte-stream data plane; this does not meet our “interpret once” goal. See [control output queue](../tmux/control.c#L818). |
| DomTerm | Browser-side DomTerm terminal | PTY output is proxied over WebSocket/pipe, mixed with DomTerm control sequences | Browser terminal implementation parses and builds its richer DOM | Browser model supports rich content; server manages persistent processes | Rich UI and persistence do not require server-side terminal interpretation. Its split is nearly the inverse of ours. See [PTY/WebSocket model](../DomTerm/lws-term/server.h#L225). |
| Mosh | Server and client both maintain compatible terminal/framebuffer state | A state-sync diff whose payload contains resize/ack instructions and terminal output synthesized to transform the old framebuffer into the new one | Mosh client applies the synthesized terminal bytes, then its display emits updates to the user's outer terminal | Focuses on latest screen state; classic Mosh does not provide general scrollback synchronization | The crucial lesson is replaceable state synchronization and skipping obsolete intermediate states. It is not a structured cell protocol on the wire. See [`Complete::diff_from`](../mosh/src/statesync/completeterminal.cc#L71). |
| WezTerm remote mux | Remote pane/terminal | Structured RPCs: render metadata, dirty stable-row ranges, serialized lines/cells, hyperlink tables, image-cell references, and separate image retrieval | Maintains an LRU cache of structured `Line` objects and renders natively | Stable row indices support history; image cells reference content hashes and image data is fetched separately | Closest existing model. It demonstrates that state metadata, line hydration, resources, and input can be separate protocol operations. See [render response](../wezterm/codec/src/lib.rs#L915), [serialized lines](../wezterm/codec/src/lib.rs#L975), and [client cache](../wezterm/wezterm-client/src/pane/renderable.rs#L31). |
| Proposed Ghostling prototype | Server-side libghostty | Initially: complete structured screen snapshots, cursor/colors/title, image resources and placements; structured key/mouse/paste/resize in reverse | Raylib stores plain client render state and draws it | Server remains authoritative; history paging can follow after the first viewport works | Gets the architectural benefit with less machinery than WezTerm. Full snapshots are intentionally acceptable for the first experiment. |

## Important correction about Mosh

Mosh is often described as sending “screen state,” which is directionally useful
but easy to overstate. Its server compares two framebuffer states, then asks its
`Display` to create terminal output that transforms the old framebuffer into the
new one. That byte string is carried in a Protobuf `hostbytes` instruction and
parsed on the client. Mosh's innovation is synchronization semantics—diffing from
a receiver state, discarding stale intermediate states, acknowledgments, and
roaming—not a rich cell-object wire format.

## What current WezTerm actually does

WezTerm's mux protocol is more nuanced than “push every changed cell.” A per-pane
server tracker remembers dimensions, cursor, title, working directory, mouse
mode, and a terminal sequence number. It computes stable row ranges changed since
the previous sequence. Changed viewport rows and the cursor row are included as
“bonus lines”; other dirty ranges can be requested by the client later. See
[`PerPane::compute_changes`](../wezterm/wezterm-mux-server-impl/src/sessionhandler.rs#L52).

The client keeps an LRU cache whose entries can be current, fetching, stale, or
current-but-being-refetched. It marks dirty ranges stale and requests needed
lines. This matters because scrollback may be much larger than the visible
viewport. See [`LineEntry`](../wezterm/wezterm-client/src/pane/renderable.rs#L31).

`SerializedLines` carries terminal `Line` objects, deduplicated hyperlink data,
and image-cell metadata. Image cells carry source texture coordinates, z-index,
padding, Kitty IDs, and a content hash; actual image data is retrieved separately.
That separation—small placement/reference updates versus larger immutable
resources—is especially relevant to Ghostling.

WezTerm also sends structured key and mouse events. Input serials correlate a key
with subsequent render state and support its optional predictive echo. Its codec
uses typed PDUs, variable-size bincode serialization, optional zstd compression,
and explicit codec versions. Those are mature-system choices, not requirements
for our first Unix-socket prototype.

## The real seam in Ghostling

The cleanest seam is the beginning of
[`render_terminal`](main.c#L800). Today, this function reads libghostty's
`GhosttyRenderState` and Kitty graphics handles and immediately issues Raylib draw
calls. The server should perform the reads; the client should perform the draws.
The protocol object placed between them is the extracted value data.

This is cleaner than serializing libghostty's opaque handles or raw structs.
Those handles are borrowed and invalidated by terminal mutation. Protocol values
must own their bytes and remain valid after the server resumes reading the PTY.

### Natural server/client ownership

| Existing Ghostling code | Destination | Reason |
| --- | --- | --- |
| `pty_spawn`, `pty_read`, `pty_write` | Server | The process that owns the child and terminal parser owns its byte stream. |
| `GhosttyTerminal`, render state, row/cell iterators | Server | They are the authoritative terminal state and expose borrowed views. |
| effect callbacks and PNG decode | Server | Terminal replies go to the PTY; Kitty payload decoding populates authoritative image storage. |
| key/mouse/focus encoders | Server | Encoding depends on modes selected by the application. |
| Raylib window and font loading | Client | They depend on the local display, DPI, fonts, and GPU. |
| Raylib key/mouse collection | Client | They depend on the local compositor/input stack. Events remain structured until the server encodes them. |
| text/cursor/image draw calls | Client | Drawing consumes owned protocol state, never libghostty handles. |
| scrollbar hit testing | Client, with a scroll request sent to server | Hit testing is graphical; authoritative viewport movement changes terminal state. |

## Structured state available from libghostty

### Viewport cells and cursor

The render API exposes dimensions, dirty status, default colors and palette,
cursor visibility/style/position, row dirty flags and selection, and per-cell
grapheme clusters, resolved colors, and `GhosttyStyle`. See
[`GhosttyRenderStateData`](build/_deps/ghostty-src/include/ghostty/vt/render.h#L138)
and [`GhosttyRenderStateRowCellsData`](build/_deps/ghostty-src/include/ghostty/vt/render.h#L632).

For the first protocol, mirror each exposed libghostty value directly: UTF-8
grapheme bytes, `GhosttyStyle` fields, resolved foreground/background results,
selection, cursor values, and colors. This is a serialization view of
libghostty's API, not a second terminal model. Raw memory still cannot be shipped:
some values contain padding or pointers, and opaque handles/iterators only name
server-process memory.

### Scrollback

The terminal API exposes scrollback row counts and maximums, and the lower-level
screen/grid APIs provide tracked grid references and row/cell inspection. The
current render state describes the selected viewport, which is enough for the
first milestone. A real history protocol should introduce stable line identities
or ranges rather than repeatedly sending the entire retained grid. WezTerm's
stable row indices are a valuable model. History is postponed, not discarded.

### Hyperlinks

The lower-level grid API exposes a cell hyperlink ID and resolves its URI through
[`ghostty_grid_ref_hyperlink_uri`](build/_deps/ghostty-src/include/ghostty/vt/grid_ref.h#L187).
The high-level render cell iterator currently does not directly return hyperlink
URIs. Hyperlinks therefore require correlating rendered rows/cells with a tracked
grid view or extending the vendored API. This should be a later experiment, not a
reason to block the first viewport.

### Kitty graphics

Ghostling currently iterates libghostty placements, resolves viewport position,
grid size, source rectangle, offsets and z-layer, then looks up RGBA image data
and uploads a new GPU texture every frame. See
[`render_kitty_images`](main.c#L664).

The protocol needs two distinct object types:

```text
ImageResource { image_id, generation, width, height, format, byte_length, rgba }
ImagePlacement { image_id, placement_id, viewport_col, viewport_row,
                 columns, rows, source_rect, x_offset, y_offset, z }
```

The storage-wide and per-image generations exposed by libghostty let the server
send resources only when content changes. Placements can change when scrolling
even if image bytes do not. The client should cache GPU textures by resource
identity/generation and delete them when told or when a replacement snapshot no
longer references them. This is simpler and faster than Ghostling's current
per-frame upload behavior.

## Input flow and ownership

Current keyboard flow is:

```text
Raylib key/text queues
  -> Ghostling maps Raylib key + modifiers into GhosttyKeyEvent
  -> ghostty_key_encoder_setopt_from_terminal()
  -> ghostty_key_encoder_encode()
  -> PTY write
```

Mouse input similarly synchronizes the encoder with terminal tracking modes and
pixel/cell geometry before encoding. See [`handle_input`](main.c#L451) and
[`handle_mouse`](main.c#L325).

Keeping encoders server-side remains the simplest correct design. Application
cursor mode, Kitty keyboard flags, mouse tracking format, focus reporting, and
bracketed paste are authoritative terminal modes. Sending already encoded bytes
would force the client to mirror those modes, recreating the split we are trying
to avoid.

The client should send logical events containing key identity, action, modifiers,
consumed modifiers, unshifted codepoint, and committed UTF-8 text. Mouse messages
need action/button/modifiers and pixel position; the latest resize message gives
the server screen, cell, and padding geometry needed by the mouse encoder. Paste
should be a separate message so the server can apply terminal paste mode.

## Smallest reasonable first wire protocol

The payload should be a field-for-field serialization of libghostty query
results. MessagePack is a reasonable experiment because it supplies framing for
arrays/maps, byte strings, integers, and booleans. It is not a drop-in C struct
serializer: every field must still be packed and unpacked explicitly, and the
installed machine currently has the runtime library but not development headers.
If adopted, vendor it inside `~/codex_projects` with the other dependencies.

Without MessagePack, use a byte-order-defined, length-prefixed stream over one
Unix domain socket:

```text
Header { u32 payload_length_le; u16 message_type_le; u16 flags_le; }
payload bytes...
```

Both programs come from one build, so the prototype does not need negotiation or
a generic model. Whether using MessagePack or manual encoding, never `write()` C
struct memory directly: padding, enum size, pointers, `size_t`, and host byte
order make that invalid even over a local socket.

Client-to-server messages:

- `RESIZE`: rows/columns, window pixels, cell pixels, and padding.
- `KEY`: logical key/action/modifiers plus optional UTF-8 committed text.
- `MOUSE`: action/button/modifiers and local pixel coordinates.
- `PASTE`: UTF-8 bytes.
- `FOCUS` and `SCROLL`: explicit events.

Server-to-client messages:

- `SCREEN_SNAPSHOT`: sequence, grid size, default colors, cursor, title, then
  rows of variable-length cells.
- `IMAGE_RESOURCE`: immutable/replacement RGBA payload keyed by ID + generation.
- `IMAGE_PLACEMENTS`: complete current visible placement list for a sequence.
- `CHILD_EXITED` and a small error/status message.

For the first working version, send a complete viewport snapshot after each
render-state change. At 80x24 this is small enough to validate ownership and
correctness. Dirty rows can be the first optimization after measurement. A full
snapshot also makes dropped client state and image placement replacement easy to
reason about.

## Minimal source split

The suggested `client.c`, `server.c`, and `protocol.h` division fits the current
single-file project. Add `protocol.c` only if framing helpers become substantial.
Avoid model/backend interfaces: the wire structs are the process boundary.

An implementation should first move code without changing behavior, then replace
direct calls across the seam:

1. Move PTY, terminal construction/effects, parsing, and encoders to `server.c`.
2. Move Raylib setup, fonts, event collection, and drawing to `client.c`.
3. Make a client-owned `ScreenSnapshot` accepted by a renderer that no longer
   receives `GhosttyTerminal` or `GhosttyRenderState`.
4. Serialize that snapshot over a socket.
5. Add image resource caching and placement snapshots.

## Revised size estimate

After inspecting the real code, 600–1,000 new lines remains plausible only for a
very narrow demo with loose error handling. A maintainable first prototype with
mandatory images is more likely **850–1,300 genuinely new lines**, while roughly
700–1,000 existing lines move or are adapted.

| Area | New LOC estimate |
| --- | ---: |
| Framing, partial I/O, message dispatch | 100–160 |
| Wire value definitions and encode/decode helpers | 140–220 |
| Server poll/socket lifecycle | 100–160 |
| Screen extraction and serialization | 150–230 |
| Client snapshot ownership and decoding | 130–200 |
| Structured input messages and server encoding | 90–140 |
| Image resources, placement serialization, texture cache | 140–220 |
| **Likely total** | **850–1,330** |

This estimate excludes reconnection, multiple panes, protocol negotiation,
history paging/search, and tests beyond focused framing/state checks. Full
snapshots may keep the first version near the lower end. Dirty ranges and stable
history identities move it toward WezTerm's substantially richer architecture.

## Where the WezTerm analogy holds—and breaks

It holds because both designs put terminal emulation and PTY ownership on the
server; send structured keyboard/mouse operations toward that authority; keep a
client-side renderable cache; transmit lines/cells, cursor and metadata; and treat
image bytes as resources referenced by cell/placement state.

It breaks because WezTerm already has a mature multi-pane mux object model,
stable scrollback row identities, asynchronous pull/hydration, predictive echo,
reconnection, codec evolution, optional compression, TLS/SSH domains, and its own
terminal `Line` types shared across client and server. Ghostling uses libghostty's
C API, whose render views are borrowed and viewport-oriented. We must copy values
into our own wire representation, and we can begin with push snapshots because we
have one pane, one local client, and one Unix socket.

The most useful lesson to borrow is the separation of **change metadata**,
**line/state content**, and **large image resources**. The machinery surrounding
that separation should be earned by experiments rather than copied wholesale.
