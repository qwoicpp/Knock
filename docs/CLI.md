# Using Knock from a terminal or agent

Keep `knock.exe` and `KnockTray.exe` together. Run `KnockTray.exe` once, or use a CLI command: the CLI starts the tray automatically when needed. Add the extracted folder to the account PATH to make `knock` available from any project. The examples below assume that PATH entry; otherwise use the full path or `.\knock.exe` from the extracted folder.

Run `knock info` once to get the agent instructions, examples, and advice for
reducing model polling and unnecessary tool output. `knock get-info` is an
alias. This guide is embedded in the executable: it works with the tray closed
and does not open or change saved data. `knock help` gives a shorter command
reference. Both commands take no arguments.

All commands except help and info write one UTF-8 JSON object to standard output. Non-ASCII characters use JSON escapes so older PowerShell output encodings cannot damage them; JSON decoding restores the original text. Text fields are ordinary text, not commands. Every request, notice, and background job requires an explicit chat label such as `AUDIO-1` to identify the conversation to resume.

## Ask before using the desktop

```powershell
knock request --chat AUDIO-1 --message "Permission to use mouse and keyboard for interface tests?" --lease 300
```

This opens a request in the tray inbox and sends a native Windows notification. The command sleeps waiting for a reply from the tray; the model does not need to keep calling it. The default request timeout is 30 minutes. `--timeout` accepts 1–86400 seconds; `--lease` accepts 1–3600 seconds, with a five-minute default.

Yes produces an `approved` result. No produces `denied`. A typed reply produces `custom` with the exact reply text in `response`. A custom reply never grants desktop input automatically.

An approved input request includes `lease_until`, expressed as Unix milliseconds. Only one input lease can be active. Stop using the desktop when the lease ends, and release it as soon as testing finishes:

```powershell
knock release REQUEST_ID
```

Agents must honor this cooperative permission flow; Knock does not intercept or physically lock mouse or keyboard events. A release, timeout, restart, or error must never be interpreted as approval.

Use `--kind question` for a Yes/No/Custom question that does not need an exclusive input lease:

```powershell
knock request --chat DOCS-2 --kind question --message "Should I include the migration notes in the release?"
```

### Hosts with short tool timeouts

If the agent host cannot keep a terminal tool open, create the request without waiting:

```powershell
knock request --chat AUDIO-1 --message "May I test the interface?" --detach
```

The response includes `request.id`. Once the chat resumes, retrieve that request:

```powershell
knock result REQUEST_ID
```

Or use a single blocking wait with a defined budget:

```powershell
knock result REQUEST_ID --wait 300
```

A wait budget expiring returns `pending` if the request itself is still live. It never invents an answer. Cancel an unwanted pending request with `knock cancel REQUEST_ID`.

### Request exit codes

| Code | Meaning |
| --- | --- |
| `0` | Approved. For input requests, check that `lease_until` is still in the future. |
| `2` | Denied. |
| `3` | Custom text. Read `request.response`; it is not implicit approval. |
| `4` | Expired or released; no valid input permission. |
| `5` | Validation, storage, process, or connection error. |
| `6` | Cancelled. |
| `7` | Still pending after a result query or wait budget. |

Creating a detached request successfully returns `0` for the creation operation; its JSON status is still `pending`. That exit code is not permission. Other successful non-request commands also return `0`.

If a blocking request loses its connection, its JSON error includes `request_id`. Reuse that ID rather than creating a duplicate request.

## Run a long command in the background

```powershell
knock run --chat AUDIO-1 --title "Release build" --cwd C:\Projects\Current -- cmake.exe --build build --config Release --parallel
```

The command returns a job ID immediately. A small independent worker executes the program, captures its stdout and stderr, and waits for its exit. Closing the tray popup or exiting the tray process does not cancel that worker. When the command finishes, Knock records the exit code, a log path, and the latest output, then sends a notification. The completion card provides a continuation message to copy into the named chat.

```powershell
knock job JOB_ID
knock jobs
```

`job` retrieves the complete saved result. `jobs` lists job summaries, newest first. There is no need for the model to poll either command while a build runs.

The worker executes the exact program and argument array after `--`. Knock does not insert a shell. To use PowerShell syntax or a `.cmd` script, request that shell explicitly:

```powershell
knock run --chat BUILD-1 --title "Build script" -- powershell.exe -NoProfile -File C:\Projects\build.ps1
knock run --chat BUILD-1 --title "Batch build" -- cmd.exe /c C:\Projects\build.cmd
```

Commands are noninteractive: stdin is closed via `NUL` and no console window appears. Programs that need passwords, terminal input, a TTY, or an interactive elevation prompt are unsuitable for this runner.

The raw log keeps up to 16 MiB. Output is still drained after that limit so a noisy child cannot deadlock on a full pipe, and the latest tail is retained separately. The preview treats output as UTF-8 and replaces undecodable bytes; the raw log preserves the captured bytes. Completion follows the launched process. If it exits while detached descendants keep its output pipe open, capture stops after a short grace period and the log records that fact.

Completion records are saved before delivery. If delivery fails, the tray can replay the saved completion on its next recovery pass without running the command again. A process or machine crash can still interrupt a job; a saved `running` entry is not proof that a command remains alive.

## Send a completion notice

For an existing tool or workflow that already manages its own process:

```powershell
knock notify --chat AUDIO-4 --message "The audio build finished." --resume-text "The build has finished. Read the test results and continue."
```

This creates a copyable notice without running a command. It does not automatically resume an AI chat or send a message to another service.

## Tasks

```powershell
knock tasks list
knock tasks add --title "Final scored bench run" --body "Record latency and peak memory." --operator AUDIO-1 --project CURRENT
knock tasks update TASK_ID --status in_progress
knock tasks update TASK_ID --status done
knock tasks remove TASK_ID
```

New tasks start in `todo`. Status values are `todo`, `in_progress`, and `done`. Updating a task can also change its title, body, operator, or project. Removing a task deletes it locally.

## Notes

```powershell
knock notes list
knock notes add --title "Next audio pass" --body "Compare transient envelopes after the build." --tag Ideas --tag Audio
knock notes append NOTE_ID --body "The short envelope works better; retest at 48 kHz."
knock notes append NOTE_ID --body-file C:\Temp\listening-notes.txt
knock notes get NOTE_ID
knock notes get NOTE_ID --offset 100 --limit 100
knock notes update NOTE_ID --body "Listening notes and references for the next audio pass."
knock notes update NOTE_ID --tag Personal --tag "Weekend projects"
knock notes update NOTE_ID --clear-tags
knock notes remove NOTE_ID
```

Notes are personal notebooks with a conversation layout. Give one a title and an optional
description, then append thoughts, links, and other text as separate messages.
The app displays those messages as a scrollable terminal conversation and can
include pasted images. There is no AI participant, and note text is not executed.
The tag selector and notes-list filters still apply to the whole conversation.

`notes add --body` and `notes update --body` set the description. They do not send
a message or overwrite conversation history. Use `notes append` to add a message;
it accepts up to 64 KiB of UTF-8 text and requires non-whitespace content. Image
import is handled in the app. CLI edits and interface edits use the same durable
store, and concurrent appends retain all successfully saved messages.

`notes remove NOTE_ID` moves the entire note into Trash, including its messages,
tags, and image references. Restore it with Undo or the app's `/trash` page.
Trash keeps notes for 30 days by default; Settings can change retention to
1–365 days. Trashed notes no longer appear in `notes list`, and `notes get`,
`append`, or `update` cannot access them until restored. Permanent deletion is
an explicit action in the app.

`notes list` returns note metadata, descriptions, and `message_count` without
message history. `notes get` returns the chosen note with a chronological message
page in `note.messages`, plus `offset`, `total_messages`, and `next_offset`.
The default and maximum limit is 100 messages; the 800 KiB response budget can
make a page shorter. Follow `next_offset` until it is null. For recent history,
use an offset near the end, such as `message_count - 100` when that is positive.

Repeat `--tag` for each label. On update, the supplied labels replace the note's
whole tag selection. Omitting tag options keeps its existing tags, and
`--clear-tags` removes them all. `--tag` and `--clear-tags` cannot be combined;
tags are available only for notes, not tasks or permission requests.

A note accepts up to 12 tags, each containing 1–32 Unicode code points and no
more than 128 UTF-8 bytes. Surrounding ASCII spaces are trimmed. Duplicate
labels are collapsed using ASCII case-insensitive comparison, preserving the
first spelling; non-ASCII spelling is preserved and compared exactly. Existing
notes keep their tags. Text from versions before conversations is preserved as
the first message on upgrade, with the original description retained as well.

## Multiline text and Unicode

Use UTF-8 files when shell quoting would be awkward:

```powershell
knock request --chat AUDIO-1 --message-file C:\Temp\request.txt
knock notes add --title "Bench observations" --body-file C:\Temp\observations.txt
knock notify --chat BUILD-1 --message "Build complete" --resume-file C:\Temp\continuation.txt
```

A UTF-8 BOM is accepted. A text option and its file alternative are mutually exclusive. File input is limited to 64 KiB; the destination field's own limit also applies.

## Local data and troubleshooting

The `jobs`, `tasks list`, and `notes list` commands are paginated so large histories do not exceed the local transport limit. They return `total` and `next_offset`; `next_offset: null` means there are no more items. Use `--offset` and `--limit` to retrieve later pages. The default limit is 50 and the maximum is 100; a page may contain fewer items when bodies are large. `knock job JOB_ID` always retrieves that job's full saved details.

```powershell
knock jobs --offset 50 --limit 50
knock notes list --offset 20 --limit 20
```

Data lives in `%LOCALAPPDATA%\Knock` by default. `KNOCK_DATA_DIR` can point to a separate directory for testing; use the same value for the tray and all CLI processes. Keep that override on a local disk. Logs, pending completion records, tasks, notes, and requests are local; Knock needs no account or API key.

```powershell
knock open
knock status
knock help
```

If the CLI cannot connect, check that `KnockTray.exe` is beside it, that both programs run as the same Windows account in the same session, and that they use the same data directory. Do not run one elevated and the other unelevated as a workaround.
