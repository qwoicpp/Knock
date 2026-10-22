# Feature tracker — version 0.6.0

- [x] Windows tray lifecycle and popup with no taskbar icon flags
- [x] Reference-matching Notifications, Task List, Notes
- [x] Permission request Yes / No / Custom
- [x] Saved custom-reply presets with add/edit/delete in Settings; selection never sends or approves
- [x] Manual At PC / Away / Busy status, optional message, persistent across restarts; unset until first selection
- [x] Compact status strip and dedicated editor accessible from popup/tray menus
- [x] Read-only `knock presence`, presence in status/request responses, GUI-only updates
- [x] Ordinary requests return pending immediately on Away/Busy; existing waits wake without granting permission
- [x] Explicit `result --wait` retains its chosen budget regardless of presence
- [x] Quiet notification timer for 15/30/60 minutes, persisted across restarts, with one inbox summary when it ends
- [x] Chat-label and selector inbox filtering, with pending requests first
- [x] Blocking CLI results, timeouts, cancellation, exclusive input lease
- [x] Background command runner, captured output, completion notice and Copy
- [x] Persistent tasks and personal conversation notes
- [x] Account-scoped named pipe with explicit access control
- [x] Bundled pixel assets and optional GIF loading
- [x] Original animated GIFs with preserved frame timing and transparent bounds
- [x] Terminal-style tabs with icons and an active tab joined to the content
- [x] Thin one-logical-pixel scroll thumbs with usable drag targets
- [x] Personal-note tag selector, custom tags, and tag/Untagged filtering
- [x] Separate note title/description editing and conversation history
- [x] Cleared title stays blank through background saves; save/navigation requires a title
- [x] Notes-list slash-command suggestions with arrow-key and Enter selection
- [x] `/find-note [query]` with live results ranked by note title
- [x] `/delete-note [query]` with a separate click-to-delete list and Back
- [x] `/search-notes [query]` with bounded excerpts, paging and exact-entry jumps
- [x] Ctrl+F within a conversation, previous/next matching entries and temporary highlighting
- [x] Edit individual messages, with revision conflicts and separate recoverable edit/composer drafts
- [x] Up Arrow in an empty composer edits the latest entry; edited entries retain their position
- [x] Note deletion moves to Trash; quick Undo, /trash Restore and explicit Delete forever
- [x] Configurable 1–365 day Trash retention, default 30 days; transactional cleanup
- [x] Unknown slash commands report an error instead of creating notes
- [x] Text composer: Enter sends, Shift+Enter inserts a newline
- [x] Clipboard image paste, bounded PNG storage, and inline thumbnails
- [x] Composer drafts across view changes, hiding, and restart
- [x] Idempotent note sending and committed-draft recovery with client IDs
- [x] Existing-note migration to a first message without losing original text
- [x] Configurable global shortcut, default Ctrl+Alt+K, with disable option
- [x] Atomic shortcut settings and registration-conflict rollback
- [x] Right-click anywhere in the popup for Settings and Hide Knock
- [x] Settings page with shortcut capture/disable and Start with Windows
- [x] Configurable popup size: 360–800 × 360–1000 logical pixels; reset to 420 × 510
- [x] Atomic window-size settings preserve shortcut and unrelated preferences
- [x] Settings → More for saved replies, quiet timers and Trash retention
- [x] Hotkey opens a persistent topmost popup; ordinary tray opening hides on click-away
- [x] Dark control initialization and layout repaint fix for tab-switch flash
- [x] Bottom command-labelled buttons removed; Enter creates tasks and notes
- [x] Build, automated correctness checks, UI inspection, documented limitations
- [x] Portable release and complete source package
- [x] Standalone `knock info` / `get-info` guide for CLI agents, including token-efficient workflows

## Desktop acceptance still required

- [ ] Real Windows 11 notification display and click-to-open
- [ ] Real Windows taskbar/tray behavior, including Explorer restart
- [ ] Real Windows Start-menu and focus changes while the hotkey-opened popup stays on top
- [ ] Multiple monitors, alternate taskbar placement, and 125–200% display scaling
- [x] Bundled animated GIF files decoded and exercised under Wine/GDI+

Implementation is complete for these paths; the unchecked items need the target
desktop. Wine results are documented separately in
VALIDATION.md.

Imported image files are retained when an attachment, message, or note is
removed, including after Trash expiry or permanent note deletion. Automatic
cleanup of unreferenced image files is not implemented.
