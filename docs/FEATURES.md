# Feature tracker — version 0.3.0

- [x] Windows tray lifecycle and popup with no taskbar icon flags
- [x] Reference-matching Notifications, Task List, Notes
- [x] Permission request Yes / No / Custom
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
- [x] Text composer: Enter sends, Shift+Enter inserts a newline
- [x] Clipboard image paste, bounded PNG storage, and inline thumbnails
- [x] Composer drafts across view changes, hiding, and restart
- [x] Idempotent note sending and committed-draft recovery with client IDs
- [x] Existing-note migration to a first message without losing original text
- [x] Configurable global shortcut, default Ctrl+Alt+K, with disable option
- [x] Atomic shortcut settings and registration-conflict rollback
- [x] Dark control initialization and layout repaint fix for tab-switch flash
- [x] Bottom command-labelled buttons removed; Enter creates tasks and notes
- [x] Build, automated correctness checks, UI inspection, documented limitations
- [x] Portable release and complete source package

## Desktop acceptance still required

- [ ] Real Windows 11 notification display and click-to-open
- [ ] Real Windows taskbar/tray behavior, including Explorer restart
- [ ] Multiple monitors, alternate taskbar placement, and 125–200% display scaling
- [x] Bundled animated GIF files decoded and exercised under Wine/GDI+

Implementation is complete for these paths; the unchecked items need the target
desktop. Wine results are documented separately in
VALIDATION.md.

Imported image files are retained when an attachment, message, or note is
removed. Automatic cleanup of unreferenced image files is not implemented.
