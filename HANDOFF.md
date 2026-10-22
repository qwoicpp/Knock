# Knock development notes

## Scope

Knock is a compact Windows tray app with a terminal-style interface. Native Win32 popup, no taskbar button or window controls; request inbox with Yes/No/Custom; background command completion and copyable continuation; personal conversation notes with tags and images; a task list and terminal CLI with explicit chat labels.

## Implementation

Version 0.5.0 is a portable Windows x64 application. Keep `KnockTray.exe`,
`knock.exe`, and the `assets` directory together. There is no web runtime,
server account, or network listener.

- C++20, Win32/GDI/GDI+, per-account/session named pipe, durable JSON store.
- UI reference: default 420 × 510 logical pixels, #090909 background, #151515
  tabs, #474747 borders, light monospace text, bundled pixel art. Settings
  allow widths of 360–800 and heights of 360–1000 logical pixels, clamped to
  the monitor work area when shown. The popup uses tool-window flags and has
  no window controls. Opening from the tray hides on deactivation; opening by
  hotkey keeps it topmost and open across focus changes until toggled or hidden.
  Escape backs out of the current view before hiding the popup.
- Requests require a chat label. Yes, No, and custom replies are persisted.
  Only trusted UI calls may resolve requests. A custom reply never grants input.
- Custom replies have up to 12 saved presets, managed in Settings → More.
  Choosing a preset only fills the editor; SEND submits the reply.
- A persisted busy deadline suppresses banners for 15/30/60 minutes without
  resolving requests or extending expiry. The app clears the deadline and sends
  one inbox summary when the timer ends. Right-click or the tray menu controls
  the timer; its status is shown above the popup content and in the tray tooltip.
- The inbox filters by exact chat label from a card or CHATS selector. Pending
  requests sort first. Notification clicks clear the filter before opening the
  relevant request/result.
- Input approvals are exclusive, time-limited cooperative leases. Release,
  timeout, and restart revoke permission. Blocking CLI waits use a condition
  variable, so the model does not need to poll.
- Detached workers run explicit argument arrays, capture output, retain exit
  codes, and persist completion records before notifying the tray. They survive
  tray exit. Completion messages can be copied into the relevant AI chat.
- Notes are personal conversations: Enter sends composer text and pasted images;
  Shift+Enter inserts a newline. EDIT opens title/description metadata; TAGS
  opens the tag selector. Descriptions remain separate from message history.
  Notes retain tag/Untagged filters. Old note bodies migrate once to a first
  message without losing text.
- In the Notes list, `/find-note [query]` opens live, case-insensitive title
  search ranked by exact match, prefix, then substring. `/delete-note [query]`
  opens a dedicated click-to-delete page with Back. Slash-prefix suggestions
  and results support arrow keys and Enter; unknown commands never create a
  note. Commands are not interpreted inside note conversations.
- `/search-notes [query]` searches message text with Unicode case-insensitive
  comparison on Windows, bounded excerpts and result pages. Opening a result
  resolves its message ID again before scrolling and highlighting it. Ctrl+F
  searches the current conversation, including its older history.
- Entry editing uses a required expected revision, preserving message ID,
  creation time and position. Stale edits fail without overwriting current text.
  The original append payload remains private to storage so a retry of the
  original client_id stays idempotent after editing. A separate edit draft
  preserves the normal composer draft and attachments through hide/restart.
- Note deletion moves the complete record into Trash. Undo lasts 10 seconds;
  `/trash` restores records or offers an explicit permanent-delete action.
  Retention defaults to 30 days and is configurable from 1 to 365 in Settings →
  More. Expiry and preference changes are transactional; request approval rules
  are unaffected. Imported images are retained even after purging a note.
- Clearing a title leaves the editor blank through background saves and
  incoming updates. Explicit save/navigation requires a nonblank title and
  keeps focus on the draft when validation fails. Hiding preserves the current
  editor without restoring the previous title; unsaved metadata is not a
  restart-persistent draft.
- Composer drafts survive view changes, hiding, incoming updates, and restart.
  A persisted client_id makes sending and crash recovery idempotent. External
  deletion preserves unsaved work rather than silently dropping it.
- Clipboard images are normalized to PNG under `note-images`, with bounded
  input/decoded dimensions and thumbnails. Removing attachments or deleting
  messages/notes leaves the imported files on disk intentionally; there is no
  automatic attachment garbage collection.
- Ctrl+Alt+K toggles the popup by default. Right-click anywhere in the popup
  for Settings, where CHANGE opens shortcut capture with a disable option.
  The page also has window-size APPLY/RESET and Start with Windows. Shortcut
  and size changes save atomically to the shared settings file while preserving
  unrelated preferences; registration conflicts and failed writes preserve the
  previous working shortcut and size. Invalid settings report an error instead
  of silently replacing the file. The tray menu also opens Settings.
- Inbox/Trash preferences live with transactional state; hotkey/window settings
  remain in settings.json. Only trusted UI calls can change inbox preferences or
  permanently purge Trash. Settings → More contains the additional preferences.
- `knock info` and `knock get-info` print the agent guide without opening the
  tray or reading saved data. CMake embeds docs/AGENT_INSTRUCTIONS.md as the one
  source for the CLI text. `help` and `info` produce plain text; other commands
  keep their JSON contract. The guide explains single waits, detached jobs,
  retaining IDs and bounded output; it makes no automatic CLI-resume claim.
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
| Global shortcut and capture dialog | `src/hotkey_win.cpp`, `include/knock/hotkey.hpp` |
| Popup dimensions and shared settings persistence | `src/window_settings.cpp`, `include/knock/window_settings.hpp` |
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

Current verification results and the previous release's baseline are recorded
in docs/VALIDATION.md. Keep native Linux checks, Windows executables exercised
under Wine, and actual Windows desktop checks distinct. A successful Wine run
does not establish how the Windows shell will behave.

Real Windows 11 notification delivery/click behavior, actual taskbar rendering,
Start-menu/focus behavior for the pinned popup, multi-monitor placement, and
high-DPI behavior remain desktop acceptance checks. Do not turn Wine results
into a claim that those Windows shell cases were tested.
`tests/cli_smoke.ps1` is available for native Windows verification.

Future work should follow desktop feedback and stay within the existing local
tray-app scope. AI providers, automatic chat resumption, cloud services, and
interface expansion are outside that scope.
