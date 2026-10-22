# Validation

The earlier sections retain the original platform and application checks.
Version-specific additions follow; the latest results are in **Version 0.3**.

## Windows platform and local IPC

The Windows platform and IPC sources were compiled with MinGW-w64 GCC 13,
C++20, `-Wall -Wextra -Wpedantic`. Both compiled without warnings.

The focused `platform_tests.exe` ran successfully under Wine 9.0 on Linux. It
exercised the compiled Windows APIs and the real Store and named-pipe server:

- Valid UTF-8/UTF-16 round trips, including non-Latin text and an emoji; rejection
  of malformed UTF-8 and an unpaired UTF-16 surrogate.
- Windows argument quoting round trips for empty arguments, spaces, tabs,
  embedded quotes, trailing backslashes, and Unicode; embedded null rejection.
- Per-account/session identity and the `KNOCK_DATA_DIR` override.
- Real framed request and response traffic, including 100 consecutive snapshots
  without a lost response and Unicode note content.
- Rejection of `request.resolve` over IPC, while a direct trusted UI call
  resolves a blocked request waiter and its resulting lease can be released.
- Rejection of oversized frames at both the client and server.
- Shutdown with a pending request waiter and a client stalled before writing its
  frame. Shutdown completed in less than three seconds, and produced no approval.

The server uses a DACL allowing the current Windows account and SYSTEM, rejects remote pipe clients,
limits frames to 1 MiB, permits at most 64 simultaneous connections and 32 request
waiters, and cancels overlapped reads/writes on shutdown. Those configuration
limits were inspected in source; the runtime test does not simulate 64 hostile
clients or a second Windows account.

`platform_tests` is registered with CTest only on Windows. Close KnockTray first;
the test skips if the application's window already exists. Test data uses a
separate temporary directory. On Linux, a cross-built test needs Wine in a
separate prefix from a running copy of the application.

Wine validation does not establish Windows 11 shell notification behavior,
notification clicks, taskbar visibility, multi-monitor placement, or Windows
high-DPI behavior. Those require testing on a real Windows desktop.

## CLI and background commands

The built `knock.exe`, `KnockTray.exe`, and `runner_fixture.exe` passed
`tests/wine_smoke.py` under a separate Wine 9.0 prefix and virtual display. These
checks exercised the actual Windows binaries, local pipe, persistent store,
worker processes, and redirected process output:

- Missing chat IDs, unknown commands, and invalid numeric options failed before
  creating a request.
- The CLI automatically started an empty tray instance and returned its result
  through a piped stdout stream.
- Unicode note titles and multiline bodies survived creation and editing; task
  operator, project, and status fields were preserved.
- A blocking one-second request expired without approval. A detached request
  returned pending, then cancelled, with the documented exit codes.
- A detached command received spaces, an empty argument, an embedded quote,
  trailing backslashes, non-Latin text, an emoji, and shell metacharacters as
  literal arguments. Both stdout and stderr were captured.
- Explicit `cmd.exe /d /s /c` syntax executed successfully. A `.cmd` file whose
  path contained spaces ran successfully and retained its deliberate exit code
  7. Plain switches remain unquoted; arguments needing CRT escaping keep it.
- A failing command produced 17 MiB of output. Its saved log stopped at 16 MiB
  plus the truncation notice, the worker kept draining output, the final output
  marker remained in the tail, and exit code 23 was retained.
- A nonexistent executable became a completed failure record with exit code -1
  and an explanation, rather than leaving a running job behind.
- A worker remained alive after its tray process was forcibly terminated. It
  completed the command, automatically restarted the tray, and delivered its
  result. Earlier note edits and task state survived that restart.
- Task and note deletion was reflected by subsequent reads.
- List pagination returned the expected newest-first records, total count,
  continuation offset, and final null offset.

During these checks, the GUI and detached worker launch paths were changed to
redirect their Win32 standard handles to `NUL` with an explicit handle allow-list.
This prevents the background process from holding the agent's result stream
open. Wine separately retains its Unix diagnostic stderr descriptor in GUI
children; the test captures that diagnostic stream in a temporary file while
keeping CLI stdout piped and checking that it closes promptly.

`tests/cli_smoke.ps1` provides corresponding checks for a real Windows desktop.
It is included but has not been executed on Windows in this environment. Close
Knock before running it; it uses a temporary data directory and shuts down its
test tray instance afterward. Example from the source folder:

```powershell
powershell.exe -NoProfile -File tests\cli_smoke.ps1 -BuildDirectory build\Release
```

The runner test does not establish compatibility with every shell's command-line
grammar, interactive/elevated programs, or detached process trees. Normal
programs receive a directly constructed Windows argument array; explicitly
invoked shells interpret their own syntax.


## Original native GUI control integration

The built `ui_tests.exe` passed against the actual `KnockTray.exe` and `knock.exe`
under an isolated Wine prefix and Xvfb display. The test creates a unique data
directory, refuses to run if an existing `Knock.Tray.v1` window is present, and
checks the spawned process ID before controlling or closing its window. It uses
native button/edit messages and IPC; it generates no global mouse or keyboard
input.

Verified behavior:

- The popup has `WS_EX_TOOLWINDOW`, lacks `WS_EX_APPWINDOW`, and has no caption,
  system menu, or minimize box. This verifies window flags, not Windows 11's
  actual taskbar rendering.
- `WM_CLOSE` hides the popup while its process and window remain alive;
  reopening shows the same window.
- Clicking the real No button records a denial and creates no input lease.
- The real custom-answer editor and Send button preserve Unicode, CRLF line
  breaks, and a typed character exactly; the resulting decision remains custom.
- Clicking Yes grants the correct request. Another input request's Yes button
  becomes disabled, and Release makes it usable again. The second request can
  then be approved and released normally.
- Incoming state changes retain the same three tab handles and their positions.
- Clicking the real Copy button publishes the exact continuation text as
  `CF_UNICODETEXT`, including CRLF, Japanese, an em dash, and a checkmark.
- An incoming request while editing a note preserves the edit handles, text,
  and keyboard focus. The draft is then saved through the actual Save button.
- Deleting the open note through the real CLI exits the stale detail view;
  creating and editing another note still works.
- Task detail controls save operator, project, body, and both In Progress and
  Done status changes.
- Normal end-session messages shut down the test instance. Relaunching with the
  same data directory preserves notes, tasks, and decision history, and opening
  the saved note shows its exact text.

Multiline `WM_SETTEXT` does not itself generate the same change notification as
keyboard input. The test follows it with a native edit/backspace cycle inside the
specific control, so dirty-state/autosave behavior is exercised without global
input injection.

The final run used isolated copies of the completed Windows executables and
assets, exited with code 0, shut its tray instance down normally, and removed
its unique test data.

The CMake `ui` test is Windows-only, runs serially, skips with code 77 if Knock is
already open, and has a 60-second timeout. Its executable is built beside the
app and CLI. This result does not replace the real Windows 11 notification,
tray, multi-monitor, and DPI acceptance checks above.

## Original core state and failure handling

The original core validation, through version 0.2, passed all 16 groups in
`store_tests` natively on Linux. They cover invalid
input, GUI-only decisions, custom replies, concurrent lease exclusivity, blocked
waits and shutdown, wait budgets versus request deadlines, lease expiry and
restart revocation, Unicode task/note persistence, idempotent completion replay,
bounded reads with large histories, durable callbacks, disk-write rollback,
corrupt-state preservation, and cancellation/archiving.

The large-history case uses a stored history over 1.5 MiB and verifies compact
status, individual job reads, and paginated results below the 800 KiB response
budget, including escaped text. The IPC frame limit remains 1 MiB.

## Rendered interface inspection

The actual Windows executable was displayed under Wine/Openbox in an isolated
1280 × 900 Xvfb desktop. Screenshots of the populated inbox, task list, notes
list, and note editor were inspected against the PNG design references. The
window rendered at 420 × 510 logical pixels with the three visible tabs, square
outlined buttons, monospace text, and bundled pixel artwork. A native white
editor scrollbar discovered during inspection was replaced with a dark custom
scroll indicator. Escape and clicking the virtual tray icon hid/reopened the
window. This is visual evidence of the implemented UI under Wine, not a
Windows 11 desktop screenshot.

## Remaining real-Windows acceptance checks

On a Windows 11 desktop, confirm the following with the included CLI:

1. Launch the app, pin its tray icon if desired, and confirm there is no taskbar
   button when the popup is open. Click away, press Escape, and reopen it.
2. Send a request and a completion notice. Confirm Windows displays each native
   notification and clicking it opens Knock. Notification settings may suppress
   banners; the in-app inbox remains the saved record.
3. Repeat with 125–200% display scaling and a second monitor, checking placement
   within the work area. Restart Explorer and confirm the tray icon returns.
4. Confirm the bundled GIFs animate correctly on the target desktop and pause
   while the popup is hidden or Task List / Notes is selected.
5. Check the configurable shortcut, clipboard image paste, and tab transitions
   on the target Windows desktop, including after changing display scaling.

No real Windows 11 machine, notification center, or physical multi-monitor
setup was available during this build. Those cases are explicitly unverified.


## Version 0.2: personal-note data and original GIFs

Three additional core groups cover note-tag normalization and persistence, exact
limits and invalid-input rollback, and migration of existing schema-1 note
records. Existing titles, bodies, IDs, and timestamps remain unchanged. Tag
updates preserve order, and an omitted tags field preserves the saved selection.

The new Windows/GDI+ sprite test passed under Wine using the three actual
bundled GIFs. It verifies decoding, visible-pixel changes across frames,
fixed alpha-union crop bounds, original 100/200 ms frame durations, complete
loops, pause/resume, delayed-timer recovery, idempotent resume, and fallback to
PNG when a replacement GIF is invalid. The files are bundled byte-for-byte as
uploaded. The integrated renderer pauses when Notifications is not visible.

The version-0.2 CLI smoke run passed all 12 groups against copied Windows
executables. The new group exercises repeated tags, normalization, omission
preservation, explicit clearing, invalid combinations, and Unicode. Tags also
survive the existing tray-restart scenario.

The native GUI suite passed its new tag checks: Personal/Ideas selection,
Unicode tag creation, query control/text/focus preservation during an incoming
request, tag filtering, Untagged and All notes, and checked tags after restart.
A regression also changes tags through the real CLI while a note body is dirty;
the body and focus survive, and saving the text preserves the external tags.

The version-0.2 popup was inspected under Wine/Openbox at 420 × 510 pixels. It
shows the new individual terminal tabs and icons, one-pixel scroll thumbs,
card tag labels, personal-note editor, and tag selector. Multiple captures of
the running Notifications view confirmed that the actual character pixels
change with the animation timer. Bundled GIF files passed byte-for-byte hash
comparison; no reference animation was rewritten. These are Wine results, not
real Windows 11 shell validation.

## Version 0.3: conversation notes, shortcuts, and repainting

The completed build compiled without warnings. The current automated results
are:

| Area | Environment | Result |
| --- | --- | --- |
| Core storage and permission rules | Native Linux | 23 groups passed |
| Native GUI interactions | Windows binaries under Wine | 15 groups passed |
| CLI and background commands | Windows binaries under Wine | 13 groups passed |
| Clipboard image helper | Windows/GDI+ under Wine | 5 groups passed |
| Global shortcut and settings dialog | Windows APIs under Wine | 6 groups passed |

The conversation checks cover text/image messages, bounded history reads,
legacy-body migration, concurrent append, metadata and tag preservation,
draft survival, and restart. A client_id is saved before a send. Concurrent or
restarted identical sends return the same durable message without a duplicate
event or timestamp change. Different content with a consumed token is rejected;
failed writes leave neither a message nor a reserved token. Stored malformed
or duplicate tokens are rejected without overwriting their original state.
Read-only lookup supports recovery when a send committed before the draft
could be cleared.

The image helper checks cover supported clipboard data, PNG normalization,
stored-image loading, bounds, and invalid input. Images are saved separately
from state/IPC. Removing a message reference deliberately does not delete its
file; these checks do not claim automatic cleanup of unreferenced images.

The shortcut checks exercise validation, actual registration conflicts,
preservation of unrelated settings, rollback on failed persistence, restart,
disable/teardown, and native key capture through the settings dialog. A new
binding is reserved before the existing one is released, and only a successful
durable save completes the replacement.

Separate direct keyboard checks in the isolated desktop used Ctrl+V,
Shift+Enter, and Enter to commit one PNG attachment with the exact intended
newline. The registered Ctrl+Alt+K shortcut hid and reopened the same popup.
These checks supplement the native control-message tests.

White-flash fixes initialize child controls against a dark background, handle
button background erasure, and avoid painting an intermediate layout. The
rendered app was sampled across 27 tab changes, producing 81 screenshot frames.
The largest reported bright-pixel fraction was 0.0001354 (about 0.01354%); no
full-white frame was observed. This is a bounded capture under Wine, not proof
that every possible transition frame on Windows has been checked.

The version-0.3 screenshots show the actual running inbox, task list, notes
list, conversation, metadata editor, tag picker, and shortcut dialog. Test
records are not included in the portable application. Real Windows 11 shell
notification delivery/clicks, taskbar behavior, Explorer restart, multiple
monitors, and high-DPI placement remain unverified desktop acceptance checks.
