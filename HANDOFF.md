# Knock development notes

## Scope

Knock is a compact Windows tray app with a terminal-style interface. Native Win32 popup, no taskbar button or window controls; request inbox with Yes/No/Custom; background command completion and copyable continuation; personal conversation notes with tags and images; a task list and terminal CLI with explicit chat labels.

## Implementation

Version 0.3.0 is a portable Windows x64 application. Keep `KnockTray.exe`,
`knock.exe`, and the `assets` directory together. There is no web runtime,
server account, or network listener.

- C++20, Win32/GDI/GDI+, per-account/session named pipe, durable JSON store.
- UI reference: 420 x 510 logical pixels, #090909 background, #151515 tabs,
  #474747 borders, light monospace text, bundled pixel art. The popup uses
  tool-window flags, hides when deactivated, and has no window controls.
- Requests require a chat label. Yes, No, and custom replies are persisted.
  Only trusted UI calls may resolve requests. A custom reply never grants input.
- Input approvals are exclusive, time-limited cooperative leases. Release,
  timeout, and restart revoke permission. Blocking CLI waits use a condition
  variable, so the model does not need to poll.
- Detached workers run explicit argument arrays, capture output, retain exit
  codes, and persist completion records before notifying the tray. They survive
  tray exit. Completion messages can be copied into the relevant AI chat.
- Notes are personal conversations: Enter sends composer text and pasted images;
  Shift+Enter inserts a newline. EDIT opens title/description metadata; TAGS
  opens the tag selector. Descriptions remain separate from message history.
  Notes retain tag/Untagged
  filters. Old note bodies migrate once to a first message without losing text.
- Composer drafts survive view changes, hiding, incoming updates, and restart.
  A persisted client_id makes sending and crash recovery idempotent. External
  deletion preserves unsaved work rather than silently dropping it.
- Clipboard images are normalized to PNG under `note-images`, with bounded
  input/decoded dimensions and thumbnails. Removing attachments or deleting
  messages/notes leaves the imported files on disk intentionally; there is no
  automatic attachment garbage collection.
- Ctrl+Alt+K toggles the popup. The tray Shortcut dialog can change or disable it.
  Settings save atomically; registration conflicts and failed writes preserve
  the previous working shortcut.
- Terminal-style tabs have icons, separate tab surfaces,
  and an active tab connected to the content. Scroll thumbs are one logical
  pixel wide, with wider invisible drag targets.
- All three original GIFs are bundled unchanged. The sprite renderer removes
  transparent canvas padding, preserves frame timing, and pauses outside the
  visible Notifications tab. Optional local asset overrides remain supported.
- Dark child-control initialization, background erasure, and batched layout
  painting address the white tab-switch flash. The bottom command-labelled
  buttons are removed; Enter creates a task or opens a new note conversation.

## Source map

| Area | Files |
| --- | --- |
| Persistent state and permission rules | `src/store.cpp`, `include/knock/store.hpp` |
| Windows encoding, paths, process arguments | `src/platform_win.cpp` |
| Framed local pipe and access control | `src/ipc_win.cpp` |
| CLI and detached command execution | `src/cli.cpp`, `src/runner_win.cpp` |
| Tray lifecycle and rendering | `src/tray_main.cpp`, `src/ui.cpp` |
| GIF decoding, crop, and timing | `src/sprite_win.cpp`, `include/knock/sprite.hpp` |
| Clipboard images and thumbnail rendering | `src/note_images_win.cpp`, `include/knock/note_images.hpp` |
| Global shortcut and settings dialog | `src/hotkey_win.cpp`, `include/knock/hotkey.hpp` |
| Correctness and integration checks | `tests/` |

See README.md for build commands, docs/CLI.md for command usage, and
docs/AGENT_INSTRUCTIONS.md for the text to give cooperating agents. In the
portable release, CLI.md and AGENT_INSTRUCTIONS.md are at the top level.

## Contracts

See docs/PROTOCOL.md. Only direct UI calls can resolve permission requests. No
generic AI auto-resume claim. Conversation text is personal content, never an
executable command or an AI request.

Note sends persist a 32-character lowercase-hex client_id with the draft before
appending. Identical replay returns the saved message; differing content with
the same token fails. `note.find_message` lets recovery recognize a committed
send. Keep that boundary intact when changing composer or draft handling.

## Verification and remaining desktop checks

The version-0.3 build is warning-free. All 23 core groups passed natively on
Linux. The Windows binaries passed 15 GUI, 13 CLI, five image-helper, and six
shortcut groups under Wine. Direct keyboard checks exercised image paste,
multiline sending, and the global shortcut. Sampling 81 frames across 27 tab
changes found no full-white frame. These are Wine observations, not a guarantee
about every Windows display frame. Details and earlier platform/GIF checks are
recorded in docs/VALIDATION.md.

Real Windows 11 notification delivery/click behavior, actual taskbar rendering,
multi-monitor placement, and high-DPI behavior remain desktop acceptance checks.
Do not turn Wine results into a claim that those Windows shell cases were tested.
`tests/cli_smoke.ps1` is available for native Windows verification.

Future work should follow desktop feedback and stay within the existing local
tray-app scope. AI providers, automatic chat resumption, cloud services, and
interface expansion are outside that scope.
