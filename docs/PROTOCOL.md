# Local protocol, version 1

All text is UTF-8. The local account-scoped named pipe carries one JSON command and one JSON response per connection, framed as a little-endian uint32 byte length then that many bytes. Maximum frame 1 MiB. Responses have `ok: true` or `ok: false, error: string`. No network listener.

Store timestamps are integer Unix milliseconds. IDs are opaque printable strings. Snapshot shape: `{schema:1, requests:[], jobs:[], tasks:[], notes:[], lease:null|{id,chat,until}}`. Arrays are chronological; UI displays newest first. Empty fresh install stays empty.

Operations use an `op` string:

- `snapshot`: `{ok:true,state:<snapshot>}`. This is the GUI's local overview. Notes omit `messages`, include `message_count`, and preview `body` at at most 512 UTF-8 bytes without splitting a character; `body_truncated:true` marks a shortened description. Other collections retain their records. This overview can still exceed the 1 MiB IPC frame limit with sufficiently many records; CLI clients use the compact and paginated operations below. Opening a conversation uses `note.get`.
- `status`: `{ok:true,requests_pending,jobs_running,tasks_todo,notes,lease}`. Counts are integers; `tasks_todo` counts only `todo`, and `lease` is null or the current lease object. This response never includes history arrays.
- `list`: collection (`requests`, `jobs`, `tasks`, or `notes`), offset (nonnegative, default 0), limit (1..100, default 50), summaries (boolean, default false). Returns `{ok:true,items:[],total,next_offset:null|integer}`. Items are newest first by insertion; `total` includes archived history. Follow `next_offset` until null. Each serialized response is at most 800 KiB, so a page can contain fewer than `limit` items. Paging does not freeze the collection: newly inserted or deleted items can shift later offsets. A single full record larger than the page budget returns `list_item_exceeds_page_budget: use summaries=true` instead of an empty non-advancing page. Summary records include applicable fields from: id, chat, title, kind, status, operator, project, tags, created_at, updated_at, finished_at, resolved_at, expires_at, lease_seconds, lease_until, released_at, release_reason, exit_code, worker_pid, archived. They omit large text bodies, responses, command text, output, and paths.
- `request.create`: chat (required, max 64 bytes), message (required, max 16000), kind (`input` default or `question`), timeout_seconds (1..86400, default 1800), lease_seconds (1..3600, default 300). Returns `{ok:true,request:<record>}`. Record fields: id, chat, message, kind, created_at, expires_at, status (`pending`), response (`""`), lease_seconds, lease_until (0).
- `request.wait`: id, wait_seconds (1..86400, default 86400). Blocks on condition variable until resolved, stopped, or wait budget expires. Returns `{ok:true,request:<record>}`; an elapsed wait budget alone returns pending and must not manufacture a decision. Request's own expiry becomes `expired`.
- `request.result`: id. Returns `{ok:true,request:<record>}`.
- `request.resolve`: id, decision (`approved`, `denied`, `custom`), text (required for custom). GUI ONLY via trusted_ui=true. Exactly once. Input approval requires an empty lease; otherwise error `input_busy`. Sets status, response, resolved_at. Input approval creates lease {id,chat,until}; record lease_until is set. Other statuses never grant input.
- `request.release`: id. Ends matching input lease and changes approved record to `released`. Available through CLI and GUI.
- `request.cancel`: id. Cancels only pending requests.
- `job.create`: chat, title (max 160), command (display text only), cwd, log_path, worker_pid optional. Returns `{ok:true,job:<record>}`. Record: id, chat, title, command, cwd, log_path, created_at, status (`running`), exit_code:null, output_tail:"", resume_text:"".
- `job.result`: id. Returns `{ok:true,job:<record>}` for that job without returning the rest of history.
- `job.started`: id, worker_pid (1..4294967295). Records the detached worker PID for a running job; `job.create` defaults worker_pid to 0. Repeating the same PID is idempotent; a different nonzero PID is rejected.
- `job.finish`: id, exit_code (-2147483648..4294967295), output_tail (max 16000), resume_text (max 16000), optional error (max 16000). Sets finished_at and status `completed` for 0 or `failed` otherwise. An identical replay returns the existing durable result without a second event or banner; a differing repeated result is rejected. Missing and empty error values compare equally. This supports crash recovery from completion sidecars after a reply is lost.
- `notify`: chat, message, resume_text optional. Creates a completed job-like notification with title=message, command="", exit_code=0, output_tail="", status=`notice` and finished_at=created_at.
- `task.add`: title, body optional, operator optional (default `ME`), project optional. Returns `{ok:true,task:<record>}`. Record id,title,body,operator,project,status (`todo`),created_at,updated_at.
- `task.update`: id, any of title,body,operator,project,status (`todo`,`in_progress`,`done`). Returns task.
- `task.delete`: id. Returns ok.
- `note.add`: title, body optional (the conversation description), tags optional (default `[]`). The new conversation starts with no messages. Returns `{ok:true,note:<metadata>}`. Metadata fields are id,title,body,tags,created_at,updated_at,message_count. The response keeps the full description but omits history.
- `note.update`: id, any of title,body,tags. Providing tags replaces the note's tag array; `[]` clears it. Omitting tags preserves them. Returns note metadata with the full description and `message_count`; it never replaces messages.
- `note.get`: id, offset (nonnegative, default 0), limit (1..100, default 100). Returns `{ok:true,note:<metadata plus messages[]>,offset,total_messages,next_offset:null|integer}`. Messages are chronological, oldest first. The response stays within 800 KiB of encoded JSON, so it may contain fewer than `limit` messages. Follow `next_offset` until null. An offset past the end returns an empty final page. The full description is included. A UI may start at `max(0,message_count-100)` to load the newest part of a long conversation. Paging does not freeze history; deletion between reads can shift offsets.
- `note.append`: id, text optional (max 65536 UTF-8 bytes), images optional (default `[]`), client_id optional (exactly 32 lowercase hexadecimal characters). At least non-whitespace text or one image is required. Appends one message `{id,text,images,created_at,client_id?}`, updates the note timestamp, and returns `{ok:true,note_id,message,message_count}`. It does not return the whole conversation. Repeating a client_id within the same note with exactly the same text and image array returns the existing message without changing its timestamps, writing state, or emitting a note event. Reusing it with different content returns `message_client_id_conflict`. A replay still succeeds at the note's message-count limit. Messages are plain personal notes, never shell commands or AI prompts.
- `note.find_message`: id (note ID), client_id (required, exactly 32 lowercase hexadecimal characters). Returns `{ok:true,message:<record or null>}` without changing the note. A missing note returns `not_found`; a missing token within an existing note returns null. Clients can persist the token with a draft before sending and use this lookup after restart to distinguish a saved message from an unsent draft.
- `note.message.remove`: id (note ID), message_id. Removes that message and updates the note timestamp. Returns `{ok:true,note_id,message_count}`. This operation only changes the saved reference; it does not delete a potentially shared image file.
- `note.delete`: id. Returns ok.
- `item.dismiss`: kind (`request` or `job`), id. Marks archived=true; pending requests and running jobs cannot be dismissed.

Store::handle catches validation/storage errors and returns JSON errors. Mutations are persisted atomically before a successful reply. Failed saves must leave state unchanged. load() preserves corrupt evidence and fails rather than overwriting it. Pending requests survive restart; input leases are revoked on restart (`released`). Completion history stays in the app even if Windows suppresses a banner.

Task and note titles are required (max 160 UTF-8 bytes); bodies allow 64000 bytes. Task operator allows 64 bytes and project 160. Job command display text allows 64000 bytes; cwd and log_path each allow 32768. All strings reject embedded NUL and invalid UTF-8. Each collection allows 10000 records; the state file allows 64 MiB. Exceeding a limit fails without committing a partial update. Stored records are validated on load; malformed state remains untouched. `tick()` and `snapshot()` may throw on a storage failure so the GUI can surface the error.

Note tags are an ordered array of at most 12 input strings. Surrounding ASCII spaces are trimmed. Each resulting tag must contain 1–32 Unicode scalar values and at most 128 UTF-8 bytes; C0/C1 control characters and Unicode line/paragraph separators are rejected. Duplicates are removed using locale-independent ASCII case folding, preserving the first spelling and input order; non-ASCII letters are compared exactly. Existing schema-1 notes without a tags field load as `tags: []` and are saved without changing their title, body, ID, or timestamps. Invalid stored tags cause load to fail with the original file preserved.

Each note permits at most 2000 messages. A message permits at most eight image
references. Each reference is a PNG basename: 1–128 ASCII letters, digits,
hyphens, or underscores, followed by `.png`. Paths, separators, drive prefixes,
alternate streams, and other extensions are rejected. The UI imports image
bytes into `note-images` inside the data directory before appending a reference;
the store does not interpret paths or execute content. Images are separate
files, not base64 payloads in the state or IPC response. Message IDs are unique
within their note, and stored messages are validated on load. Stored client IDs,
when present, must also be valid and unique within that note. Older messages
without a client ID remain valid. Removing a message also removes its replay
token; clients must not reuse a consumed token for new content.

Existing schema-1 notes without a `messages` field migrate once. A nonempty old
body becomes the first message, with the note's existing `updated_at` timestamp.
The body is also retained as the description, and note IDs, titles, tags, and
timestamps stay unchanged. Empty old notes acquire an empty array. The migrated
array is saved atomically, preventing duplicate migration on restart.

Both full and summary `list` results for notes omit message history and include
`message_count`; full results preserve the complete description, while summaries
omit it. Use `note.get` for messages.

Store callback runs outside the mutex after a durable mutation. Event `{type:"request"|"job"|"updated",id,chat,title,message}`. request means newly created request, job means finished job or notify; updated means all other changes. Note mutations also include `operation` with the exact operation name (`note.add`, `note.update`, `note.append`, `note.message.remove`, or `note.delete`); `id` is always the note ID. UI posts events across threads and shows OS banners only for request/job.

GUI and CLI share no API keys, no external accounts, no background AI. A cooperating agent must check the result and valid lease before using input and release input after testing. Only the specific agent host can resume its AI automatically; otherwise resume_text must be copied into the named chat manually.
