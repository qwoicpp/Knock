# Working on Knock

Knock is a small native Windows tray inbox for agent requests, background command results, tasks, and notes. Keep the interface black, square, compact, and monospace, with restrained pixel artwork. Do not add a web runtime, decorative dashboard, large branding, gradients, or an in-app AI.

Use readable C++20 and Win32. Keep GUI, storage, IPC, and command execution separate. Make meaningful local commits as coherent work lands. Keep HANDOFF.md and docs/FEATURES.md current. Never report a Windows desktop test when only a cross-build or Wine test ran.

Only the GUI may approve or deny requests. Custom text, expiry, disconnection, shutdown, and errors must never be treated as approval. Input permissions are cooperative time-limited leases, not an OS security boundary. Do not pass agent text to a shell implicitly.

Verify persistence, concurrent decisions, Unicode, argument quoting, and request timeout behavior. Inspect real rendered UI for clipping, hover bounds, scroll behavior, and DPI handling. Keep unneeded tests and dependencies out.
