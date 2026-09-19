# Headless terminal research notebook

Initial survey: 2026-09-18. This is an exploratory learning project with no
deadline. Findings below are prior art; design proposals are not commitments.

## Prior art

- **WezTerm** is the closest architectural comparison found in this first pass.
  Its native GUI can attach to a remote mux daemon. Its protocol includes pane
  render changes, line retrieval, cursor state, dimensions, sequence numbers,
  input, and scrollback search. Study its choices rather than assume this split
  is new. [Multiplexing](https://wezterm.org/multiplexing.html),
  [protocol source](https://github.com/wezterm/wezterm/blob/main/codec/src/lib.rs).
- **Mosh** synchronizes terminal screen state instead of transporting every
  output byte. Its client still displays through an outer terminal. The paper
  distinguishes current-screen synchronization from preservation of scrollback.
  [Project](https://mosh.org/), [paper](https://mosh.org/mosh-paper.pdf).
- **iTerm2 with tmux control mode** gives persistent remote panes a native UI.
  Control mode carries pane management plus application output, including escape
  sequences; it is not simply a stream of final cells.
  [iTerm2 integration](https://iterm2.com/documentation-tmux-integration.html),
  [control protocol](https://github.com/tmux/tmux/wiki/Control-Mode).
- **Neovim external UIs** are an adjacent design, not a terminal multiplexer.
  Their protocol offers grids, line updates, highlight definitions, cursor
  positions, and redraw completion, with optional structured UI elements.
  [UI protocol](https://neovim.io/doc/user/api-ui-events/).
- **Zellij** remains the user's primary reference for multiplexer behavior.
  The local checkout's web client creates an xterm.js terminal and passes network
  output to `term.write(data)`. A graphical/browser client does not by itself
  eliminate a second terminal parser. See `../zellij/zellij-client/assets/terminal.js`,
  `../zellij/zellij-client/assets/websockets.js`, and
  `../zellij/zellij-utils/src/ipc.rs`.
- **DomTerm** offers another split: the server manages processes while the
  browser implements terminal emulation and a rich document-oriented view.
  [Architecture](https://domterm.org/Architecture-notes.html),
  [process model](https://domterm.org/Processes-and-security.html).

This survey does not establish adoption rates or historical reasons for them.
The broad idea has been implemented. Potential tradeoffs worth investigating
include installation on both ends, protocol compatibility, text layout, latency,
multiple clients, and compatibility with existing terminal-only workflows.

## Where could we split the system?

| Network representation | Client responsibility | Questions to explore |
| --- | --- | --- |
| Application bytes | Full terminal parsing and rendering | Persistence without removing client emulation |
| Cells/lines plus styles and widths | Maintain a display cache; shape and draw text | Unicode width authority, history, resynchronization |
| Positioned glyphs/drawing commands | Execute rendering commands | Font identity, DPI, text selection and accessibility |
| Pixels or video | Decode/display; transmit input | Fidelity versus bandwidth, scaling, text semantics |
| Structured terminal objects plus assets | Native presentation of grids, images, panes, metadata | Scope, versioning, and what applications actually expose |

A hybrid worth exploring is server-authoritative cells and history with local
font shaping and native pane UI, plus image resources sent separately. This is a
hypothesis, not a selected protocol. Do not serialize libghostty's raw C structs:
define a representation independent of pointers, ABI layout, and library version.

## State, history, and events are different

- The current screen is replaceable state. Intermediate progress-bar frames may
  be skipped when a client falls behind, provided a newer complete state arrives.
- Scrollback is retained history, subject to an explicit retention policy. A
  latest-screen snapshot alone cannot reconstruct lines that already scrolled off.
- Input and actions need ordering and reconnect semantics. Retrying input must
  not execute a keystroke twice. Notifications and clipboard requests are events,
  not merely pixels that can always be discarded when a newer frame arrives.

Candidate server-to-client messages: pane lifecycle/layout; versioned screen
snapshots and deltas; cursor; history chunks; style definitions; image resources
and placements; title and explicit metadata; events and input acknowledgments.

Candidate client-to-server messages: logical keys with modifiers; committed text
and paste; mouse position/buttons; grid/pixel dimensions; focus; history requests;
pane/session actions; resynchronization and capability negotiation.

Native UI freedom does not reveal application intent automatically. A terminal
grid cannot reliably identify commands, tables, file links, or editor buffers
without explicit application/shell integration or fallible heuristics.

## Learning experiments to choose together

1. Trace a shell printing `hello` and a cursor-moving update through Zellij,
   then compare with WezTerm's render-state messages and Neovim's grid events.
2. Describe one screen snapshot in readable JSON before choosing a binary format.
3. Compare snapshots, changed rows, and changed cell runs for a progress bar,
   scrolling output, and a fullscreen editor. Measure actual encoded bytes.
4. Explore disconnect/reconnect and slow clients: what is state, what is history,
   and which actions require an acknowledgment?
5. Attach two clients with different fonts and sizes. One PTY has one active
   grid size; decide resize ownership separately from each client's viewport.
6. Revisit keyboard mapping through native Wayland/XKB and distinguish physical
   keys, logical keys, composed text, shortcuts, and terminal-mode encoding.
