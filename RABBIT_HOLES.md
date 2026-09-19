# Rabbit holes

This is a parking lot for questions we want to explore without losing the
current thread. The project is for learning and fun, so entries are invitations,
not a backlog or deadline.

## What crosses the process or network boundary

- Raw PTY bytes versus terminal cells versus drawing commands versus pixels.
- Snapshots, row deltas, cell-run deltas, and state synchronization.
- Separating replaceable screen state from durable scrollback and ordered events.
- First-class images: resource identity, payload transfer, placement, updates,
  deletion, caching, and memory limits.
- Hyperlinks, title, working directory, palette, cursor, selection, and modes.
- Whether the server or client owns Unicode width, shaping, ligatures, fallback
  fonts, and pixel geometry.
- Multiple clients with different sizes: PTY grid authority versus local viewport.

## Wire encoding and transport

- Readable prototype encodings versus compact binary encodings.
- MessagePack, CBOR, Protocol Buffers, FlatBuffers, Cap'n Proto, bincode, and a
  hand-written length-prefixed format.
- Schema evolution and version negotiation, once the prototype teaches us what
  actually changes.
- Unix domain sockets first; SSH tunneling, TLS, QUIC, and Mosh-like UDP later.
- Backpressure, partial reads/writes, framing, cancellation, and slow clients.
- Compression: whole messages, cell runs, dictionaries, image payloads, and when
  compression costs more CPU than it saves.

## State, history, and recovery

- Scrollback ownership, stable line identifiers, retention, paging, and search.
- Reconnect snapshots and resuming from a known sequence number.
- Idempotent input and acknowledgments so reconnect never duplicates a key.
- What may be skipped when a client falls behind and what must be delivered.
- Alternate screen behavior and whether its contents belong in history.

## Input

- Physical keys, logical keys, composed text, IME/preedit, and paste are distinct.
- XKB, native Wayland, XWayland, and why Caps Lock remapping currently fails.
- Keeping libghostty key/mouse encoding server-side because application modes
  affect the bytes written to the PTY.
- Local shortcuts versus events forwarded to the remote application.
- Mouse cell coordinates versus pixels, high DPI, focus, and scroll ownership.

## Rendering and richer terminal objects

- Text shaping and ligatures across terminal cell boundaries.
- Kitty graphics, Unicode placeholders, animation, and z-order.
- Sixel and iTerm image protocols relative to libghostty's exposed state.
- Accessibility, selection, copying, search, and semantic annotations.
- Shell integration as explicit semantics rather than screen scraping.

## Multiplexer behavior

- Zellij's session, pane, layout, plugin, and collaboration model.
- WezTerm's remote mux cache, dirty ranges, image retrieval, and predictive echo.
- Size policy when several attached clients disagree.
- Session lifecycle, detach/attach, process supervision, and server upgrades.

## Measurement experiments

- Record representative workloads: prompt editing, progress bars, `cat`, `top`,
  Neovim, scrolling, and images.
- Compare full snapshots, dirty rows, and cell runs by bytes, CPU, latency, and
  implementation complexity.
- Inject fragmentation, latency, packet loss, disconnection, and slow readers.

