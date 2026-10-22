"""Exercise the real Windows executables under an isolated Wine prefix.

Usage: WINEPREFIX=/tmp/knock-test-wine DISPLAY=:98 python3 tests/wine_smoke.py build-windows
This is a developer check, not a runtime dependency. runner_fixture.exe must be
built first. Native Windows UI/notification acceptance remains a separate check.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time


build = Path(sys.argv[1]).resolve()
wine = os.environ.get("WINE", "/usr/lib/wine/wine64")
temporary = Path(tempfile.mkdtemp(prefix="knock-cli-smoke-"))
env = os.environ.copy()
env["WINEDEBUG"] = "-all"
env["KNOCK_DATA_DIR"] = "Z:" + str(temporary).replace("/", "\\")
assert env.get("WINEPREFIX"), "Use an isolated WINEPREFIX"


def windows(path: Path) -> str:
    return "Z:" + str(path.resolve()).replace("/", "\\")


def cli(*arguments: str, code: int = 0, timeout: float = 30):
    # Wine keeps its Unix diagnostic stderr in newly started GUI processes,
    # independently of their explicit Win32 NUL standard handles. Capture that
    # diagnostic stream to a file, while keeping stdout piped to verify prompt
    # EOF for the actual CLI result consumed by agent tools.
    with tempfile.TemporaryFile() as diagnostics:
        result = subprocess.run([wine, str(build / "knock.exe"), *arguments], env=env,
                                stdout=subprocess.PIPE, stderr=diagnostics, timeout=timeout)
        diagnostics.seek(0)
        errors = diagnostics.read().decode(errors="replace")
    output = result.stdout.decode("utf-8")
    assert result.returncode == code, (arguments, result.returncode, output, errors)
    parsed = json.loads(output)
    assert parsed.get("ok") == (code != 5), parsed
    return parsed


def wait_job(identifier: str, timeout: float = 20):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        job = cli("job", identifier)["job"]
        if job["status"] != "running":
            return job
        time.sleep(0.15)
    raise AssertionError(f"Job {identifier} never finished")


def passed(name: str):
    print("PASS", name, flush=True)


fixture = windows(build / "runner_fixture.exe")
cli("request", "--message", "Missing chat", code=5)
cli("request", "--chat", "TEST", "--message", "x", "--timeout", "-1", code=5)
cli("unknown-command", code=5)
cli("notes", "add", "--title", "First", "--title", "Duplicate", code=5)
cli("notes", "add", "--title", "Invalid", "--clear-tags", code=5)
cli("notes", "add", "--title", "Invalid", "--tag", code=5)
cli("tasks", "add", "--title", "Invalid", "--tag", "Ideas", code=5)
cli("notes", "get", code=5)
cli("notes", "append", "missing-note", code=5)
cli("notes", "get", "missing-note", "--limit", "101", code=5)
passed("invalid CLI arguments fail before creating requests")

state = cli("status")
assert all(state[key] == 0 for key in ("requests_pending", "jobs_running", "tasks_todo", "notes")), state
passed("auto-start and empty persistent state")

note = cli("notes", "add", "--title", "Café ☕", "--body", "Line one\nSecond line: 日本語",
           "--tag", " Ideas ", "--tag", "ideas", "--tag", "日本語", "--tag", "Personal projects")["note"]
assert note["tags"] == ["Ideas", "日本語", "Personal projects"], note
cli("notes", "update", note["id"], "--body", "Updated – résumé")
saved = cli("notes", "list")["notes"][0]
assert saved["body"] == "Updated – résumé" and saved["tags"] == note["tags"], saved
task = cli("tasks", "add", "--title", "Check output", "--operator", "AUDIO-1", "--project", "CURRENT")["task"]
cli("tasks", "update", task["id"], "--status", "in_progress")
assert cli("tasks", "list")["tasks"][0]["status"] == "in_progress"
passed("Unicode notes, task metadata, and updates")

cleared = cli("notes", "update", note["id"], "--clear-tags")["note"]
assert cleared["tags"] == [], cleared
replacement = cli("notes", "update", note["id"], "--tag", "Personal", "--tag", "日本語")["note"]
assert replacement["tags"] == ["Personal", "日本語"], replacement
cli("notes", "update", note["id"], "--clear-tags", "--tag", "Invalid", code=5)
cli("notes", "update", note["id"], "--clear-tags", "--clear-tags", code=5)
cli("notes", "update", note["id"], "--tag", "x" * 33, code=5)
assert cli("notes", "list")["notes"][0]["tags"] == ["Personal", "日本語"]
passed("repeated note tags, normalization, preservation, clearing, and invalid-option rollback")

conversation = cli("notes", "get", note["id"])
assert conversation["note"]["messages"] == [] and conversation["total_messages"] == 0, conversation
assert conversation["note"]["body"] == "Updated – résumé", conversation
first_message = cli("notes", "append", note["id"], "--body", "https://example.com/\nA useful reference — café ☕")["message"]
message_file = temporary / "message.txt"
message_file.write_text("Second message\n日本語 🧪", encoding="utf-8-sig")
second_message = cli("notes", "append", note["id"], "--body-file", windows(message_file))["message"]
assert second_message["text"] == "Second message\n日本語 🧪", second_message
first_page = cli("notes", "get", note["id"], "--limit", "1")
last_page = cli("notes", "get", note["id"], "--offset", "1", "--limit", "1")
assert first_page["offset"] == 0 and first_page["total_messages"] == 2 and first_page["next_offset"] == 1, first_page
assert first_page["note"]["messages"] == [first_message], first_page
assert last_page["offset"] == 1 and last_page["next_offset"] is None and last_page["note"]["messages"] == [second_message], last_page
metadata = cli("notes", "update", note["id"], "--body", "Updated – résumé")["note"]
assert metadata["message_count"] == 2 and "messages" not in metadata, metadata
metadata = cli("notes", "list")["notes"][0]
assert metadata["message_count"] == 2 and "messages" not in metadata, metadata
cli("notes", "append", note["id"], "--body", " \t\n", code=5)
cli("notes", "append", note["id"], "--body", "text", "--body-file", windows(message_file), code=5)
cli("notes", "append", note["id"], "--body", "text", "--tag", "Ideas", code=5)
cli("notes", "get", note["id"], "--offset", "-1", code=5)
assert cli("notes", "get", note["id"])["note"]["messages"] == [first_message, second_message]
passed("personal conversations, Unicode append, message paging, metadata, and failed-write preservation")

expired = cli("request", "--chat", "WAIT-1", "--kind", "question", "--message", "Expiry test", "--timeout", "1", code=4)
assert expired["request"]["status"] == "expired"
pending = cli("request", "--chat", "CANCEL-1", "--message", "Do not approve", "--detach")["request"]
assert pending["status"] == "pending"
assert cli("result", pending["id"], code=7)["request"]["status"] == "pending"
cli("cancel", pending["id"])
assert cli("result", pending["id"], code=6)["request"]["status"] == "cancelled"
passed("blocking expiry, detached status, and cancellation")

literal = ["space value", "", 'embedded"quote', "C:\\trailing space\\", "$() & | > < ;", "日本語 ☕"]
echo = cli("run", "--chat", "ARGS-1", "--title", "Argument round trip", "--", fixture, "echo", *literal)
job = wait_job(echo["job"]["id"])
assert job["exit_code"] == 0, job
assert json.loads(job["output_tail"].splitlines()[0]) == literal, job
assert "stderr-captured" in job["output_tail"], job
assert "ARGS-1" in job["resume_text"] and job["id"] in job["resume_text"], job
passed("exact argv, empty arguments, Unicode, shell metacharacters, and stderr capture")

shell = cli("run", "--chat", "SHELL-1", "--title", "Explicit cmd syntax", "--", "cmd.exe", "/d", "/s", "/c", "echo shell-syntax-ok")
job = wait_job(shell["job"]["id"])
assert job["exit_code"] == 0 and "shell-syntax-ok" in job["output_tail"], job
batch = temporary / "build script.cmd"
batch.write_bytes(b"@echo off\r\necho batch-path-ok\r\nexit /b 7\r\n")
shell = cli("run", "--chat", "SHELL-2", "--title", "Batch path with spaces", "--", "cmd.exe", "/d", "/c", windows(batch))
job = wait_job(shell["job"]["id"])
assert job["exit_code"] == 7 and "batch-path-ok" in job["output_tail"], job
passed("explicit cmd syntax and batch-file paths with spaces")

flood = cli("run", "--chat", "NOISE-1", "--title", "Noisy failing build", "--", fixture, "flood")
job = wait_job(flood["job"]["id"])
assert job["exit_code"] == 23 and job["status"] == "failed", job
assert "TAIL_SENTINEL" in job["output_tail"] and "capped at 16 MiB" in job["output_tail"], job
log = Path(job["log_path"][2:].replace("\\", "/"))
assert 16 * 1024 * 1024 <= log.stat().st_size < 16 * 1024 * 1024 + 1024
passed("nonzero exit code, 17 MiB output drained, bounded log, final tail retained")

missing = cli("run", "--chat", "ERROR-1", "--title", "Missing program", "--", "knock-this-does-not-exist.exe")
job = wait_job(missing["job"]["id"])
assert job["status"] == "failed" and job["exit_code"] == -1 and job.get("error"), job
passed("launch failure becomes a completed failure record")

long = cli("run", "--chat", "SURVIVE-1", "--title", "Independent worker", "--", fixture, "wait")
subprocess.run([wine, "taskkill.exe", "/F", "/IM", "KnockTray.exe"], env=env,
               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10, check=True)
time.sleep(3)
job = wait_job(long["job"]["id"])
assert job["status"] == "completed" and "finished-after-tray-stop" in job["output_tail"], job
saved = cli("notes", "list")["notes"][0]
assert saved["body"] == "Updated – résumé" and saved["tags"] == ["Personal", "日本語"], saved
assert saved["message_count"] == 2 and cli("notes", "get", note["id"])["note"]["messages"] == [first_message, second_message]
assert cli("tasks", "list")["tasks"][0]["status"] == "in_progress"
passed("worker survives tray termination; automatic restart and note/task persistence")

cli("notes", "remove", note["id"])
cli("tasks", "remove", task["id"])
assert not cli("notes", "list")["notes"]
assert not cli("tasks", "list")["tasks"]
passed("task and note deletion")
first = cli("notes", "add", "--title", "First page test")["note"]
second = cli("notes", "add", "--title", "Second page test")["note"]
page = cli("notes", "list", "--limit", "1")
assert page["total"] == 2 and page["next_offset"] == 1 and page["notes"][0]["id"] == second["id"], page
last = cli("notes", "list", "--offset", "1", "--limit", "1")
assert last["next_offset"] is None and last["notes"][0]["id"] == first["id"], last
cli("notes", "remove", first["id"])
cli("notes", "remove", second["id"])
passed("bounded list pagination and newest-first ordering")
print(json.dumps({"ok": True, "data_directory": str(temporary)}), flush=True)
