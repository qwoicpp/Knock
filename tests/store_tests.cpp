#include "knock/store.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using knock::Json;
using knock::Store;
using namespace std::chrono_literals;

namespace {
void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Json ok(Json result) {
    check(result.value("ok", false), "operation failed: " + result.dump());
    return result;
}

void fails(const Json& result, const std::string& contains = {}) {
    check(!result.value("ok", false), "operation unexpectedly succeeded: " + result.dump());
    if (!contains.empty())
        check(result.value("error", "").find(contains) != std::string::npos,
              "wrong error: " + result.dump());
}

struct Fixture {
    std::filesystem::path directory = std::filesystem::temp_directory_path() / ("knock-test-" + knock::make_id());
    Store store{directory};
    Fixture() { store.load(); }
    ~Fixture() { store.stop(); std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};

Json request(Store& store, const std::string& chat = "AUDIO-1", const std::string& kind = "input", int seconds = 1800, int lease = 300) {
    return ok(store.handle({{"op", "request.create"}, {"chat", chat}, {"message", "Permission to use desktop input for interface tests?"},
                           {"kind", kind}, {"timeout_seconds", seconds}, {"lease_seconds", lease}}))["request"];
}

Json answer(Store& store, const Json& item, const std::string& decision, const std::string& text = {}) {
    return store.handle({{"op", "request.resolve"}, {"id", item["id"]}, {"decision", decision}, {"text", text}}, true);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    check(static_cast<bool>(stream), "test fixture write failed");
}

void fresh_state_and_validation() {
    Fixture f;
    const auto initial = f.store.snapshot();
    check(initial["schema"] == 1 && initial["lease"].is_null(), "wrong fresh schema");
    for (const auto* key : {"requests", "jobs", "tasks", "notes"}) check(initial[key].empty(), "fresh app contains demo data");
    const std::vector<Json> invalid = {
        Json::array(), {{"op", "request.create"}, {"message", "Missing chat"}},
        {{"op", "request.create"}, {"chat", " "}, {"message", "a"}},
        {{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "a"}, {"kind", "other"}},
        {{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "a"}, {"timeout_seconds", 0}},
        {{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "a"}, {"timeout_seconds", 1.5}},
        {{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", "a"}, {"lease_seconds", 3601}},
        {{"op", "request.create"}, {"chat", std::string(65, 'a')}, {"message", "a"}},
        {{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", std::string(16001, 'a')}},
        {{"op", "note.add"}, {"title", std::string("a\0b", 3)}},
        {{"op", "note.add"}, {"title", std::string("\xff", 1)}},
        {{"op", "nonsense"}}, {{"op", "request.result"}, {"id", "../bad"}}
    };
    for (const auto& command : invalid) fails(f.store.handle(command));
    check(f.store.snapshot() == initial, "invalid commands mutated state");
}

void permission_boundaries() {
    Fixture f;
    auto item = request(f.store);
    fails(f.store.handle({{"op", "request.resolve"}, {"id", item["id"]}, {"decision", "approved"}}), "forbidden");
    fails(answer(f.store, item, "custom", " "), "empty_field");
    const auto custom = ok(answer(f.store, item, "custom", "Not now; continue with headless tests."))["request"];
    check(custom["status"] == "custom" && custom["lease_until"] == 0, "custom granted input");
    check(f.store.snapshot()["lease"].is_null(), "custom created a lease");
    fails(answer(f.store, item, "approved"), "already_resolved");
    auto denied = request(f.store, "AUDIO-2");
    check(ok(answer(f.store, denied, "denied"))["request"]["response"] == "No", "wrong denial response");
    check(f.store.snapshot()["lease"].is_null(), "denial created a lease");
    auto question = request(f.store, "DOCS-1", "question");
    ok(answer(f.store, question, "approved"));
    check(f.store.snapshot()["lease"].is_null(), "general question granted input");
}

void exclusive_concurrent_approvals() {
    Fixture f;
    auto a = request(f.store, "AUDIO-1");
    auto b = request(f.store, "UI-1");
    auto first = std::async(std::launch::async, [&] { return answer(f.store, a, "approved"); });
    auto second = std::async(std::launch::async, [&] { return answer(f.store, b, "approved"); });
    auto ra = first.get();
    auto rb = second.get();
    check(ra.value("ok", false) != rb.value("ok", false), "two concurrent input leases granted");
    const auto winner = ra.value("ok", false) ? a : b;
    const auto loser = ra.value("ok", false) ? b : a;
    fails(ra.value("ok", false) ? rb : ra, "input_busy");
    check(f.store.snapshot()["lease"]["id"] == winner["id"], "lease request mismatch");
    ok(f.store.handle({{"op", "request.release"}, {"id", winner["id"]}}));
    ok(f.store.handle({{"op", "request.release"}, {"id", winner["id"]}}));
    ok(answer(f.store, loser, "approved"));
    check(f.store.snapshot()["lease"]["id"] == loser["id"], "released lease was not reusable");
}

void waiting_and_stop() {
    Fixture f;
    auto item = request(f.store);
    auto wait = [&] { return f.store.handle({{"op", "request.wait"}, {"id", item["id"]}, {"wait_seconds", 10}}); };
    auto a = std::async(std::launch::async, wait);
    auto b = std::async(std::launch::async, wait);
    check(a.wait_for(30ms) == std::future_status::timeout, "wait returned while pending");
    ok(answer(f.store, item, "custom", "Run the other tests first."));
    check(a.wait_for(1s) == std::future_status::ready && b.wait_for(1s) == std::future_status::ready, "decision failed to wake all waiters");
    check(ok(a.get())["request"]["status"] == "custom" && ok(b.get())["request"]["status"] == "custom", "wrong waiter decision");
    auto pending = request(f.store);
    auto stopped = std::async(std::launch::async, [&] {
        return f.store.handle({{"op", "request.wait"}, {"id", pending["id"]}, {"wait_seconds", 60}});
    });
    check(stopped.wait_for(30ms) == std::future_status::timeout, "stop waiter returned early");
    f.store.stop();
    check(stopped.wait_for(1s) == std::future_status::ready, "stop did not wake waiter");
    fails(stopped.get(), "store_stopped");
}

void wait_budget_and_expiry() {
    Fixture f;
    auto long_request = request(f.store, "BUILD-1", "input", 60);
    auto budget = ok(f.store.handle({{"op", "request.wait"}, {"id", long_request["id"]}, {"wait_seconds", 1}}));
    check(budget["request"]["status"] == "pending", "wait budget manufactured a decision");
    auto short_request = request(f.store, "BUILD-2", "input", 1);
    auto expired = ok(f.store.handle({{"op", "request.wait"}, {"id", short_request["id"]}, {"wait_seconds", 3}}));
    check(expired["request"]["status"] == "expired", "request deadline did not expire");
    fails(answer(f.store, short_request, "approved"), "already_resolved");
    check(f.store.snapshot()["lease"].is_null(), "expired request granted input");
}

void lease_expiry_and_restart() {
    Fixture f;
    auto a = request(f.store, "SHORT-LEASE", "input", 60, 1);
    ok(answer(f.store, a, "approved"));
    auto timer = request(f.store, "TIMEOUT", "question", 1);
    ok(f.store.handle({{"op", "request.wait"}, {"id", timer["id"]}, {"wait_seconds", 3}}));
    check(f.store.snapshot()["lease"].is_null(), "expired lease remained active");
    check(ok(f.store.handle({{"op", "request.result"}, {"id", a["id"]}}))["request"]["status"] == "released", "expired lease still approved");
    auto b = request(f.store, "RESTART", "input", 60, 300);
    ok(answer(f.store, b, "approved"));
    auto pending = request(f.store, "PENDING");
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    check(restarted.snapshot()["lease"].is_null(), "lease survived restart");
    check(ok(restarted.handle({{"op", "request.result"}, {"id", b["id"]}}))["request"]["status"] == "released", "old Yes revived after restart");
    check(ok(restarted.handle({{"op", "request.result"}, {"id", pending["id"]}}))["request"]["status"] == "pending", "pending request was lost on restart");
}

void task_note_unicode_roundtrip() {
    Fixture f;
    const auto task = ok(f.store.handle({{"op", "task.add"}, {"title", "Build: 音声 🛠"}, {"body", "Line one\nSecond line — café"}, {"operator", "AUDIO-1"}, {"project", "Current"}}))["task"];
    check(task["status"] == "todo", "wrong task default");
    auto edited = ok(f.store.handle({{"op", "task.update"}, {"id", task["id"]}, {"status", "in_progress"}, {"body", "Testing\nUnicode ✓"}}))["task"];
    fails(f.store.handle({{"op", "task.update"}, {"id", task["id"]}, {"title", "must roll back"}, {"status", "bad"}}), "invalid_task_status");
    check(f.store.snapshot()["tasks"][0] == edited, "partially invalid task update persisted");
    auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Ideas"}, {"body", "C:\\Users\\Example\\test\n日本語"}}))["note"];
    ok(f.store.handle({{"op", "note.update"}, {"id", note["id"]}, {"body", "Changed 🧪\n"}}));
    auto before = f.store.snapshot();
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    check(restarted.snapshot() == before, "Unicode content did not survive save/reload");
    ok(restarted.handle({{"op", "note.delete"}, {"id", note["id"]}}));
    ok(restarted.handle({{"op", "task.delete"}, {"id", task["id"]}}));
    check(restarted.snapshot()["tasks"].empty() && restarted.snapshot()["notes"].empty(), "delete failed");
}

void note_tags_roundtrip() {
    Fixture f;
    const auto plain = ok(f.store.handle({{"op", "note.add"}, {"title", "Existing personal note"}}))["note"];
    check(plain["tags"] == Json::array(), "new untagged note omitted its tags array");
    const Json tags = {"Personal", "IDEAS", "日本語", "Café 🧪", "É", "é"};
    auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Notebook"}, {"body", "Private thoughts"},
        {"tags", {" Personal ", "IDEAS", "ideas", "日本語", " Café 🧪 ", "É", "é"}}}))["note"];
    check(note["tags"] == tags, "tags were not trimmed/deduplicated with spelling and order preserved");
    note = ok(f.store.handle({{"op", "note.update"}, {"id", note["id"]}, {"body", "Changed body"}}))["note"];
    check(note["tags"] == tags, "body-only update dropped tags");
    const auto listed = ok(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"summaries", true}}));
    check(listed["items"][0]["tags"] == tags && !listed["items"][0].contains("body"),
          "note summaries lost tags or included the full body");
    note = ok(f.store.handle({{"op", "note.update"}, {"id", note["id"]}, {"tags", {"Reference"}}}))["note"];
    check(note["tags"] == Json({"Reference"}), "tag update merged instead of replacing");
    note = ok(f.store.handle({{"op", "note.update"}, {"id", note["id"]}, {"tags", Json::array()}}))["note"];
    check(note["tags"].empty(), "explicit empty tag array did not clear tags");
    ok(f.store.handle({{"op", "note.update"}, {"id", note["id"]}, {"tags", tags}}));
    const auto before = f.store.snapshot();
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    check(restarted.snapshot() == before, "tags or note content changed across restart");
}

void note_tag_validation_and_limits() {
    Fixture f;
    Json twelve = Json::array();
    for (int index = 0; index < 12; ++index) twelve.push_back("Tag " + std::to_string(index));
    auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Limit tests"}, {"tags", twelve}}))["note"];
    check(note["tags"].size() == 12, "maximum tag count was rejected");
    std::string emoji_limit;
    for (int index = 0; index < 32; ++index) emoji_limit += "🧪";
    check(emoji_limit.size() == 128, "UTF-8 tag fixture has wrong byte length");
    note = ok(f.store.handle({{"op", "note.update"}, {"id", note["id"]},
        {"tags", {std::string(32, 'x'), " " + emoji_limit + " "}}}))["note"];
    check(note["tags"][1] == emoji_limit, "32 Unicode scalar values / 128 bytes was rejected");

    Json thirteen = twelve;
    thirteen.push_back("Tag 12");
    const std::vector<Json> invalid = {
        nullptr, "Personal", 42, Json::object(), Json::array({false}), Json::array({1}),
        Json::array({""}), Json::array({"   "}), Json::array({"bad\ttag"}), Json::array({"bad\ntag"}),
        Json::array({std::string("bad\0tag", 7)}), Json::array({std::string("bad\x7F", 4)}),
        Json::array({"bad\xC2\x85"}), Json::array({"bad\xE2\x80\xA8"}),
        Json::array({std::string("\xff", 1)}), Json::array({std::string("\xc0\xaf", 2)}),
        Json::array({std::string(33, 'x')}), Json::array({emoji_limit + "a"}), thirteen
    };
    const auto before = f.store.snapshot();
    const auto disk = read_file(f.directory / "state.json");
    for (const auto& tags : invalid) {
        fails(f.store.handle({{"op", "note.update"}, {"id", note["id"]}, {"title", "Must roll back"}, {"tags", tags}}));
        fails(f.store.handle({{"op", "note.add"}, {"title", "Must not create"}, {"tags", tags}}));
        check(f.store.snapshot() == before, "invalid tags changed existing notes");
        check(read_file(f.directory / "state.json") == disk, "invalid tags modified the saved file");
    }
}

void note_tag_migration_and_corruption() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Older personal note"},
        {"body", "Unchanged text\n日本語"}}))["note"];
    auto legacy = Json::parse(read_file(f.directory / "state.json"));
    legacy["notes"][0].erase("tags");
    f.store.stop();
    const auto path = f.directory / "state.json";
    write_file(path, legacy.dump());
    {
        Store restarted(f.directory);
        restarted.load();
        const auto migrated = restarted.snapshot();
        check(migrated["notes"][0] == note, "tag migration altered an old note or its timestamps");
        check(Json::parse(read_file(path))["notes"][0]["tags"] == Json::array(),
              "tag migration was not persisted");
    }
    auto corrupt = legacy;
    corrupt["notes"][0]["tags"] = Json::array({"invalid\nlabel"});
    const auto bytes = corrupt.dump();
    write_file(path, bytes);
    Store rejected(f.directory);
    bool threw = false;
    try { rejected.load(); } catch (...) { threw = true; }
    check(threw && read_file(path) == bytes, "invalid stored tags were accepted or overwrote evidence");
}

void note_conversation_migration() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Websites"},
        {"body", "Bookmarks and thoughts\nhttps://example.com/\n日本語 🧪"}, {"tags", {"Personal"}}}))["note"];
    const auto blank = ok(f.store.handle({{"op", "note.add"}, {"title", "Empty notebook"}}))["note"];
    const auto path = f.directory / "state.json";
    auto legacy = Json::parse(read_file(path));
    for (auto& record : legacy["notes"]) record.erase("messages");
    f.store.stop();
    write_file(path, legacy.dump());
    Store restarted(f.directory);
    restarted.load();
    const auto migrated = ok(restarted.handle({{"op", "note.get"}, {"id", note["id"]}}));
    const auto& saved = migrated["note"];
    check(saved["title"] == note["title"] && saved["body"] == note["body"] &&
          saved["tags"] == note["tags"] && saved["created_at"] == note["created_at"] &&
          saved["updated_at"] == note["updated_at"], "conversation migration altered note metadata");
    check(saved["messages"].size() == 1 && saved["messages"][0]["text"] == note["body"] &&
          saved["messages"][0]["images"].empty() &&
          saved["messages"][0]["created_at"] == note["updated_at"], "old text was not preserved as its first message");
    check(ok(restarted.handle({{"op", "note.get"}, {"id", blank["id"]}}))["note"]["messages"].empty(),
          "empty old note acquired a placeholder message");
    restarted.load();
    check(ok(restarted.handle({{"op", "note.get"}, {"id", note["id"]}})) == migrated,
          "migration ran twice or replaced the preserved message ID");
    check(Json::parse(read_file(path))["notes"][0]["messages"] == saved["messages"],
          "conversation migration was not persisted");
    const auto compact = restarted.snapshot()["notes"][0];
    check(!compact.contains("messages") && compact["message_count"] == 1,
          "snapshot contains the full conversation or lost its count");
}

void note_conversation_append_and_restart() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Websites"},
        {"body", "Things to revisit"}, {"tags", {"Personal", "Reference"}}}))["note"];
    const auto id = note["id"];
    check(ok(f.store.handle({{"op", "note.get"}, {"id", id}}))["note"]["messages"].empty(),
          "a new conversation description became a sent message");
    std::vector<Json> events;
    f.store.set_changed([&](const Json& change) { events.push_back(change); });
    const auto first = ok(f.store.handle({{"op", "note.append"}, {"id", id},
        {"text", "https://example.com/\nA useful reference — 日本語 🧪"}}));
    const auto image = ok(f.store.handle({{"op", "note.append"}, {"id", id},
        {"images", {"0123456789abcdef0123456789abcdef.png"}}}));
    check(first["note_id"] == id && first["message_count"] == 1 &&
          image["message_count"] == 2 && image["message"]["text"] == "", "append response is incomplete");
    check(events.size() == 2 && events[0]["type"] == "updated" && events[0]["id"] == id &&
          events[0]["operation"] == "note.append", "append did not identify its note in the event");
    const auto before = ok(f.store.handle({{"op", "note.get"}, {"id", id}}))["note"]["messages"];
    const auto changed = ok(f.store.handle({{"op", "note.update"}, {"id", id},
        {"body", "Changed description"}, {"tags", {"Ideas"}}}));
    check(changed["note"]["message_count"] == 2 && !changed["note"].contains("messages"),
          "note update returned its entire conversation");
    check(ok(f.store.handle({{"op", "note.get"}, {"id", id}}))["note"]["messages"] == before,
          "description/tag update replaced conversation history");
    const auto removed = ok(f.store.handle({{"op", "note.message.remove"}, {"id", id},
        {"message_id", first["message"]["id"]}}));
    check(removed["message_count"] == 1 && events.back()["operation"] == "note.message.remove",
          "message removal did not update its count or event");
    const auto saved = ok(f.store.handle({{"op", "note.get"}, {"id", id}}));
    check(saved["note"]["messages"][0] == image["message"], "removal affected a different message");
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    check(ok(restarted.handle({{"op", "note.get"}, {"id", id}})) == saved,
          "conversation text, image references, or deletion did not survive restart");
}

void note_conversation_limits_and_rollback() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Limits"}}))["note"];
    const auto id = note["id"];
    Json eight = Json::array();
    for (int index = 0; index < 8; ++index) eight.push_back("image-" + std::to_string(index) + ".png");
    ok(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", std::string(65536, 'x')}, {"images", eight}}));
    auto nine = eight;
    nine.push_back("another.png");
    std::vector<Json> commands = {
        {{"op", "note.append"}, {"id", id}},
        {{"op", "note.append"}, {"id", id}, {"text", " \r\n\t"}},
        {{"op", "note.append"}, {"id", id}, {"text", std::string(65537, 'x')}},
        {{"op", "note.append"}, {"id", id}, {"text", std::string("a\0b", 3)}},
        {{"op", "note.append"}, {"id", id}, {"text", std::string("\xff", 1)}},
        {{"op", "note.append"}, {"id", id}, {"text", "ok"}, {"images", nine}},
        {{"op", "note.append"}, {"id", id}, {"text", "ok"}, {"images", "file.png"}},
        {{"op", "note.message.remove"}, {"id", id}, {"message_id", "missing"}}
    };
    for (const Json& name : {Json("../outside.png"), Json("folder/file.png"), Json("folder\\file.png"),
            Json("C:\\outside.png"), Json("/outside.png"), Json("file.jpg"), Json(".png"),
            Json("file.png.exe"), Json("bad name.png"), Json("bad:stream.png"), Json(42)}) {
        commands.push_back({{"op", "note.append"}, {"id", id}, {"text", "ok"}, {"images", Json::array({name})}});
    }
    const auto before = ok(f.store.handle({{"op", "note.get"}, {"id", id}}));
    const auto state_path = f.directory / "state.json";
    const auto bytes = read_file(state_path);
    int events = 0;
    f.store.set_changed([&](const Json&) { ++events; });
    for (const auto& command : commands) fails(f.store.handle(command));
    check(ok(f.store.handle({{"op", "note.get"}, {"id", id}})) == before && read_file(state_path) == bytes,
          "invalid message changed memory or disk");
    const auto backup = f.directory / "saved.json";
    std::filesystem::rename(state_path, backup);
    std::filesystem::create_directory(state_path);
    fails(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", "Must not be committed"}}));
    fails(f.store.handle({{"op", "note.message.remove"}, {"id", id},
        {"message_id", before["note"]["messages"][0]["id"]}}));
    check(ok(f.store.handle({{"op", "note.get"}, {"id", id}})) == before && events == 0,
          "failed message save changed history or emitted a successful event");
    std::filesystem::remove(state_path);
    std::filesystem::rename(backup, state_path);
    ok(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", "Recovered after disk failure"}}));
}

void note_conversation_paging_and_concurrency() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "A long conversation"},
        {"body", std::string(64000, '\x01')}}))["note"];
    const auto id = note["id"];
    for (int index = 0; index < 4; ++index)
        ok(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", std::string(65536, '\x01')}}));
    std::set<std::string> identifiers;
    std::size_t offset = 0;
    for (;;) {
        const auto page = ok(f.store.handle({{"op", "note.get"}, {"id", id}, {"offset", offset}}));
        check(page.dump().size() <= 800 * 1024 && page["total_messages"] == 4 &&
              page["offset"] == offset, "conversation page exceeded its encoded byte budget or lost its offset");
        const auto& messages = page["note"]["messages"];
        check(!messages.empty(), "nonfinal conversation page did not advance");
        for (const auto& message : messages) {
            check(message["text"] == std::string(65536, '\x01'), "conversation paging truncated message text");
            check(identifiers.insert(message["id"].get<std::string>()).second, "message repeated across pages");
        }
        if (page["next_offset"].is_null()) break;
        const auto next = page["next_offset"].get<std::size_t>();
        check(next > offset, "conversation page cursor did not advance");
        offset = next;
    }
    check(identifiers.size() == 4, "conversation paging omitted messages");
    const auto listed = ok(f.store.handle({{"op", "list"}, {"collection", "notes"}}));
    check(!listed["items"][0].contains("messages") && listed["items"][0]["message_count"] == 4,
          "notes list returned conversation history or omitted the count");
    const auto beyond = ok(f.store.handle({{"op", "note.get"}, {"id", id}, {"offset", INT64_MAX}}));
    check(beyond["note"]["messages"].empty() && beyond["next_offset"].is_null(), "past-end page was not empty");
    fails(f.store.handle({{"op", "note.get"}, {"id", id}, {"offset", -1}}), "integer_out_of_range");
    fails(f.store.handle({{"op", "note.get"}, {"id", id}, {"limit", 101}}), "integer_out_of_range");
    fails(f.store.handle({{"op", "note.get"}, {"id", "missing-note"}}), "not_found");

    const auto concurrent = ok(f.store.handle({{"op", "note.add"}, {"title", "Concurrent notes"}}))["note"]["id"];
    std::vector<std::future<void>> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.push_back(std::async(std::launch::async, [&, worker] {
            for (int index = 0; index < 5; ++index)
                ok(f.store.handle({{"op", "note.append"}, {"id", concurrent},
                    {"text", std::to_string(worker) + ":" + std::to_string(index)}}));
        }));
    }
    for (auto& worker : workers) worker.get();
    const auto saved = ok(f.store.handle({{"op", "note.get"}, {"id", concurrent}}));
    check(saved["total_messages"] == 20, "concurrent append lost a message");
    std::set<std::string> texts;
    for (const auto& message : saved["note"]["messages"]) texts.insert(message["text"].get<std::string>());
    check(texts.size() == 20, "concurrent append duplicated a message");
}

void note_conversation_stored_validation() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Stored limits"}}))["note"];
    const auto path = f.directory / "state.json";
    auto base = Json::parse(read_file(path));
    const std::string client_id = "0123456789abcdef0123456789abcdef";
    Json message{{"id", "saved-message"}, {"text", "A saved message"}, {"client_id", client_id},
                 {"images", Json::array()}, {"created_at", note["created_at"]}};
    base["notes"][0]["messages"].push_back(message);
    f.store.stop();
    std::vector<Json> invalid;
    auto bad = base;
    bad["notes"][0]["messages"] = "not an array";
    invalid.push_back(bad);
    bad = base;
    bad["notes"][0]["messages"][0].erase("images");
    invalid.push_back(bad);
    bad = base;
    bad["notes"][0]["messages"][0]["images"] = Json::array({"../outside.png"});
    invalid.push_back(bad);
    bad = base;
    bad["notes"][0]["messages"].push_back(message);
    invalid.push_back(bad);
    bad = base;
    bad["notes"][0]["messages"][0]["client_id"] = "malformed";
    invalid.push_back(bad);
    bad = base;
    auto duplicate_token = message;
    duplicate_token["id"] = "different-message-id";
    bad["notes"][0]["messages"].push_back(duplicate_token);
    invalid.push_back(bad);
    for (const auto& corrupt : invalid) {
        const auto bytes = corrupt.dump();
        write_file(path, bytes);
        Store rejected(f.directory);
        bool threw = false;
        try { rejected.load(); } catch (...) { threw = true; }
        check(threw && read_file(path) == bytes, "invalid stored conversation overwrote evidence or loaded");
    }
    auto full = base;
    auto& messages = full["notes"][0]["messages"];
    message.erase("client_id");
    for (int index = 1; index < 2000; ++index) {
        message["id"] = "saved-" + std::to_string(index);
        messages.push_back(message);
    }
    write_file(path, full.dump());
    Store limited(f.directory);
    limited.load();
    fails(limited.handle({{"op", "note.append"}, {"id", note["id"]}, {"text", "One too many"}}),
          "note_message_limit_reached");
    check(ok(limited.handle({{"op", "note.get"}, {"id", note["id"]}}))["total_messages"] == 2000,
          "message cap changed existing history");
    const auto replay = ok(limited.handle({{"op", "note.append"}, {"id", note["id"]},
        {"text", "A saved message"}, {"client_id", client_id}}));
    check(replay["message_count"] == 2000 && replay["message"]["id"] == "saved-message",
          "message limit rejected a durable replay");
}

void note_message_replay_and_lookup() {
    Fixture f;
    const auto id = ok(f.store.handle({{"op", "note.add"}, {"title", "Saved send"}}))["note"]["id"];
    const std::string client_id = "0123456789abcdef0123456789abcdef";
    const Json command{{"op", "note.append"}, {"id", id}, {"client_id", client_id},
        {"text", "A saved draft\n日本語"}, {"images", Json::array({"image-1.png"})}};
    std::atomic<int> events{0};
    f.store.set_changed([&](const Json&) { ++events; });
    std::vector<std::future<Json>> sends;
    for (int index = 0; index < 8; ++index)
        sends.push_back(std::async(std::launch::async, [&] { return ok(f.store.handle(command)); }));
    const auto first = sends.front().get();
    for (std::size_t index = 1; index < sends.size(); ++index)
        check(sends[index].get() == first, "concurrent send replay returned a different record");
    check(events == 1 && first["message_count"] == 1 && first["message"]["client_id"] == client_id,
          "concurrent replay duplicated a message or event");
    const auto path = f.directory / "state.json";
    const auto saved = read_file(path);
    const Json lookup{{"op", "note.find_message"}, {"id", id}, {"client_id", client_id}};
    check(ok(f.store.handle(lookup))["message"] == first["message"], "lookup did not find saved message");
    check(ok(f.store.handle({{"op", "note.find_message"}, {"id", id},
        {"client_id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}}))["message"].is_null(), "unknown token was not null");
    auto conflict = command;
    conflict["text"] = "Different content";
    fails(f.store.handle(conflict), "message_client_id_conflict");
    conflict = command;
    conflict["images"] = Json::array({"image-2.png"});
    fails(f.store.handle(conflict), "message_client_id_conflict");
    check(read_file(path) == saved && events == 1, "lookup or conflicting replay mutated state");
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    restarted.set_changed([&](const Json&) { ++events; });
    check(ok(restarted.handle(lookup))["message"] == first["message"], "saved token was lost on restart");
    check(ok(restarted.handle(command)) == first && read_file(path) == saved && events == 1,
          "restart replay changed IDs, timestamps, state bytes, or events");
    const auto other_id = ok(restarted.handle({{"op", "note.add"}, {"title", "Other note"}}))["note"]["id"];
    check(ok(restarted.handle({{"op", "note.find_message"}, {"id", other_id}, {"client_id", client_id}}))["message"].is_null(),
          "lookup escaped its note");
    auto other = command;
    other["id"] = other_id;
    check(ok(restarted.handle(other))["message"]["id"] != first["message"]["id"], "tokens were shared across notes");
}

void note_message_token_validation_and_rollback() {
    Fixture f;
    const auto id = ok(f.store.handle({{"op", "note.add"}, {"title", "Draft recovery"}}))["note"]["id"];
    const std::vector<Json> invalid{nullptr, 123, true, "", "abc", std::string(31, 'a'), std::string(33, 'a'),
        std::string(32, 'A'), std::string(32, 'g'), std::string(31, 'a') + '\0'};
    const auto path = f.directory / "state.json";
    const auto before = read_file(path);
    int events = 0;
    f.store.set_changed([&](const Json&) { ++events; });
    for (const auto& token : invalid) {
        fails(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", "Draft"}, {"client_id", token}}), "invalid_client_id");
        fails(f.store.handle({{"op", "note.find_message"}, {"id", id}, {"client_id", token}}), "invalid_client_id");
    }
    fails(f.store.handle({{"op", "note.find_message"}, {"id", id}}), "missing_field: client_id");
    const std::string token = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    fails(f.store.handle({{"op", "note.find_message"}, {"id", "missing-note"}, {"client_id", token}}), "not_found");
    check(read_file(path) == before && events == 0, "invalid tokens changed state or emitted events");
    const auto backup = f.directory / "state-saved.json";
    std::filesystem::rename(path, backup);
    std::filesystem::create_directory(path);
    const Json command{{"op", "note.append"}, {"id", id}, {"text", "Uncommitted draft"}, {"client_id", token}};
    const Json lookup{{"op", "note.find_message"}, {"id", id}, {"client_id", token}};
    fails(f.store.handle(command));
    check(ok(f.store.handle(lookup))["message"].is_null() && events == 0,
          "failed append kept a replay token or emitted an event");
    std::filesystem::remove(path);
    std::filesystem::rename(backup, path);
    check(read_file(path) == before, "failed append altered prior bytes");
    auto changed = command;
    changed["text"] = "Edited draft after failed save";
    const auto sent = ok(f.store.handle(changed));
    check(sent["message_count"] == 1 && events == 1 && ok(f.store.handle(lookup))["message"] == sent["message"],
          "retry after rollback did not commit once");
}

void jobs_and_idempotent_completion() {
    Fixture f;
    std::atomic<int> job_banners{0};
    f.store.set_changed([&](const Json& e) { if (e["type"] == "job") ++job_banners; });
    auto job = ok(f.store.handle({{"op", "job.create"}, {"chat", "BUILD-1"}, {"title", "Release build"}, {"command", "cmake --build build"}, {"cwd", "C:\\src"}, {"log_path", "C:\\logs\\build.log"}}))["job"];
    check(job["worker_pid"] == 0 && job["exit_code"].is_null(), "job defaults incorrect");
    fails(f.store.handle({{"op", "item.dismiss"}, {"kind", "job"}, {"id", job["id"]}}), "item_still_active");
    ok(f.store.handle({{"op", "job.started"}, {"id", job["id"]}, {"worker_pid", 1234}}));
    ok(f.store.handle({{"op", "job.started"}, {"id", job["id"]}, {"worker_pid", 1234}}));
    fails(f.store.handle({{"op", "job.started"}, {"id", job["id"]}, {"worker_pid", 4567}}), "worker_already_started");
    const Json finish = {{"op", "job.finish"}, {"id", job["id"]}, {"exit_code", 1}, {"output_tail", "Compile failed: 日本語"}, {"resume_text", "BUILD-1: inspect the compiler error."}, {"error", "compiler failure"}};
    const auto completed = ok(f.store.handle(finish))["job"];
    check(completed["status"] == "failed", "failed build marked completed");
    check(ok(f.store.handle(finish))["job"] == completed, "identical finish replay not idempotent");
    check(job_banners == 1, "finish replay duplicated a notification");
    auto changed = finish;
    changed["exit_code"] = 0;
    fails(f.store.handle(changed), "already_finished");
    ok(f.store.handle({{"op", "item.dismiss"}, {"kind", "job"}, {"id", job["id"]}}));
    check(f.store.snapshot()["jobs"][0]["archived"] == true, "archive not persisted");
    auto notice = ok(f.store.handle({{"op", "notify"}, {"chat", "AUDIO-4"}, {"message", "Build finished."}, {"resume_text", "Continue AUDIO-4."}}))["job"];
    check(notice["status"] == "notice" && job_banners == 2, "notice did not generate a banner event");
}

void bounded_reads_with_large_history() {
    Fixture f;
    // Escaping expands each 64 KiB body to about 384 KiB on the wire. This
    // catches budgets based on unescaped character counts rather than JSON.
    const std::string body(64000, '\x01');
    std::vector<std::string> note_ids;
    for (int index = 0; index < 4; ++index) {
        const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Note " + std::to_string(index)},
                                             {"body", body}}))["note"];
        note_ids.push_back(note["id"].get<std::string>());
    }
    auto pending = request(f.store, "PENDING");
    auto granted = request(f.store, "LEASE-A");
    ok(answer(f.store, granted, "approved"));
    ok(f.store.handle({{"op", "task.add"}, {"title", "Still to do"}}));
    auto in_progress = ok(f.store.handle({{"op", "task.add"}, {"title", "Already working"}}))["task"];
    ok(f.store.handle({{"op", "task.update"}, {"id", in_progress["id"]}, {"status", "in_progress"}}));
    auto job = ok(f.store.handle({{"op", "job.create"}, {"chat", "BUILD-1"}, {"title", "Finished build"}}))["job"];
    job = ok(f.store.handle({{"op", "job.finish"}, {"id", job["id"]}, {"exit_code", 0},
                            {"output_tail", std::string(16000, 'x')}, {"resume_text", std::string(16000, 'y')}}))["job"];
    ok(f.store.handle({{"op", "job.create"}, {"chat", "BUILD-2"}, {"title", "Running build"}}));

    const auto before = f.store.snapshot();
    check(read_file(f.directory / "state.json").size() > 1024 * 1024,
          "large-history fixture did not exceed the IPC frame limit");
    check(before["notes"][0]["body_truncated"] == true &&
          before["notes"][0]["body"].get<std::string>().size() == 512,
          "snapshot retained a complete large note description");
    int mutations = 0;
    f.store.set_changed([&](const Json&) { ++mutations; });
    const auto status = ok(f.store.handle({{"op", "status"}}));
    check(status["requests_pending"] == 1 && status["jobs_running"] == 1 &&
          status["tasks_todo"] == 1 && status["notes"] == 4, "compact counts were incorrect");
    check(status["lease"]["id"] == granted["id"] && status.dump().size() < 1024,
          "status omitted the lease or returned history");
    check(ok(f.store.handle({{"op", "job.result"}, {"id", job["id"]}}))["job"] == job,
          "specific job result lost full output in a large store");
    fails(f.store.handle({{"op", "job.result"}, {"id", "missing-job"}}), "not_found");

    std::vector<std::string> listed_ids;
    std::size_t offset = 0;
    int pages = 0;
    for (;;) {
        const auto page = ok(f.store.handle({{"op", "list"}, {"collection", "notes"},
                                             {"offset", offset}, {"limit", 100}}));
        check(page["total"] == 4 && !page["items"].empty(), "paged list was empty before completion");
        check(page.dump().size() <= 800 * 1024, "encoded list exceeded the response budget");
        for (const auto& note : page["items"]) {
            check(note["body"] == body, "full list truncated note content");
            listed_ids.push_back(note["id"].get<std::string>());
        }
        ++pages;
        if (page["next_offset"].is_null()) break;
        const auto next = page["next_offset"].get<std::size_t>();
        check(next > offset, "page cursor did not advance");
        offset = next;
    }
    check(pages == 2 && listed_ids.size() == note_ids.size(), "byte-budget paging lost records");
    for (std::size_t index = 0; index < note_ids.size(); ++index)
        check(listed_ids[index] == note_ids[note_ids.size() - 1 - index], "list order was not newest first");

    const auto summaries = ok(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"summaries", true}}));
    check(summaries["items"].size() == 4 && summaries["next_offset"].is_null(), "summary page unexpectedly incomplete");
    for (const auto& summary : summaries["items"])
        check(!summary.contains("body") && summary.contains("title"), "summary included body or omitted title");
    check(summaries.dump().size() < 2048, "summary retained large text");
    for (const auto* collection : {"requests", "jobs", "tasks"}) {
        const auto page = ok(f.store.handle({{"op", "list"}, {"collection", collection}, {"limit", 1}}));
        check(page["items"].size() == 1 && page["next_offset"] == 1, "list limit or cursor ignored");
    }
    const auto beyond = ok(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"offset", INT64_MAX}}));
    check(beyond["items"].empty() && beyond["next_offset"].is_null(), "out-of-range offset did not return an empty final page");
    fails(f.store.handle({{"op", "list"}, {"collection", "missing"}}), "invalid_collection");
    fails(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"offset", -1}}), "integer_out_of_range");
    fails(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"limit", 0}}), "integer_out_of_range");
    fails(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"limit", 101}}), "integer_out_of_range");
    fails(f.store.handle({{"op", "list"}, {"collection", "notes"}, {"summaries", "yes"}}), "invalid_boolean");
    check(mutations == 0 && f.store.snapshot() == before, "read operations mutated the store");
    check(ok(f.store.handle({{"op", "request.result"}, {"id", pending["id"]}}))["request"]["status"] == "pending",
          "history paging changed an unrelated request");
}

void callbacks_are_durable_and_reentrant() {
    Fixture f;
    bool called = false;
    bool durable = false;
    bool snapshot_matches = false;
    f.store.set_changed([&](const Json& notification) {
        called = true;
        const auto disk = Json::parse(read_file(f.directory / "state.json"));
        durable = !disk["requests"].empty() && disk["requests"][0]["id"] == notification["id"];
        snapshot_matches = f.store.snapshot() == disk;
    });
    request(f.store);
    check(called && durable && snapshot_matches, "callback did not observe durable state");
    f.store.set_changed([](const Json&) { throw std::runtime_error("UI gone"); });
    ok(f.store.handle({{"op", "note.add"}, {"title", "still saved"}}));
}

void failed_save_rolls_back() {
    Fixture f;
    auto item = request(f.store);
    const auto before = f.store.snapshot();
    int events = 0;
    f.store.set_changed([&](const Json&) { ++events; });
    const auto state_path = f.directory / "state.json";
    const auto backup = f.directory / "saved.json";
    std::filesystem::rename(state_path, backup);
    std::filesystem::create_directory(state_path);
    fails(f.store.handle({{"op", "task.add"}, {"title", "Must not save"}}));
    fails(answer(f.store, item, "approved"));
    check(f.store.snapshot() == before, "failed disk write left an in-memory mutation or lease");
    check(events == 0, "failed mutation emitted success event");
    for (const auto& entry : std::filesystem::directory_iterator(f.directory))
        check(entry.path().extension() != ".tmp", "failed write left temporary file");
    std::filesystem::remove(state_path);
    std::filesystem::rename(backup, state_path);
    ok(answer(f.store, item, "approved"));
    check(events == 1, "store did not recover after transient disk failure");
}

void corruption_is_preserved() {
    Fixture f;
    request(f.store);
    auto incomplete = f.store.snapshot();
    incomplete["requests"][0].erase("response");
    f.store.stop();
    const auto path = f.directory / "state.json";
    const std::vector<std::string> broken = {
        "{broken", "", "{\"schema\":99}", incomplete.dump(),
        R"({"schema":1,"requests":[],"jobs":[],"tasks":[],"notes":[],"lease":{"id":"missing","chat":"A","until":100}})"
    };
    for (const auto& contents : broken) {
        write_file(path, contents);
        Store restarted(f.directory);
        bool threw = false;
        try { restarted.load(); } catch (...) { threw = true; }
        check(threw, "corrupt file loaded successfully");
        fails(restarted.handle({{"op", "note.add"}, {"title", "must not overwrite evidence"}}), "store_not_loaded");
        check(read_file(path) == contents, "corrupt original was overwritten");
    }
}

void cancellation_and_archiving() {
    Fixture f;
    auto item = request(f.store);
    fails(f.store.handle({{"op", "item.dismiss"}, {"kind", "request"}, {"id", item["id"]}}), "item_still_active");
    ok(f.store.handle({{"op", "request.cancel"}, {"id", item["id"]}}));
    fails(answer(f.store, item, "approved"), "already_resolved");
    const auto result = ok(f.store.handle({{"op", "request.wait"}, {"id", item["id"]}, {"wait_seconds", 1}}));
    check(result["request"]["status"] == "cancelled" && result["request"]["lease_until"] == 0, "cancelled request approved");
    ok(f.store.handle({{"op", "item.dismiss"}, {"kind", "request"}, {"id", item["id"]}}));
    check(f.store.snapshot()["requests"][0]["archived"] == true, "request not archived");
}

void preferences_are_validated_and_gui_owned() {
    Fixture f;
    const auto defaults = f.store.snapshot()["preferences"];
    check(defaults["saved_replies"].size() == 3 && defaults["busy_until"] == 0 &&
          defaults["trash_retention_days"] == 30, "wrong preference defaults");
    const auto busy_until = knock::now_ms() + 900000;
    fails(f.store.handle({{"op", "preferences.update"}, {"busy_until", busy_until}}), "forbidden");
    const auto saved = ok(f.store.handle({{"op", "preferences.update"}, {"busy_until", busy_until},
        {"saved_replies", {"Not now — 日本語", "Run headless tests.\nAsk later."}}}, true))["preferences"];
    check(saved["busy_until"] == busy_until && saved["trash_retention_days"] == 30,
          "partial preferences did not preserve unrelated values");
    const auto pending = request(f.store);
    check(pending["status"] == "pending" && f.store.snapshot()["lease"].is_null(),
          "busy preferences answered or approved an agent");
    Json excessive = Json::array();
    for (int index = 0; index < 13; ++index) excessive.push_back("Reply");
    const std::vector<Json> invalid = {
        {{"busy_until", -1}}, {{"busy_until", 1.5}}, {{"trash_retention_days", 0}},
        {{"trash_retention_days", 366}}, {{"saved_replies", "not an array"}},
        {{"saved_replies", {" "}}}, {{"saved_replies", {std::string(2001, 'x')}}},
        {{"saved_replies", {std::string("\xff", 1)}}}, {{"saved_replies", excessive}}
    };
    for (auto command : invalid) {
        command["op"] = "preferences.update";
        fails(f.store.handle(command, true));
        check(f.store.snapshot()["preferences"] == saved, "invalid preferences partially saved");
    }
    ok(f.store.handle({{"op", "preferences.update"}, {"trash_retention_days", 7}}, true));
    const auto before = f.store.snapshot();
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    check(restarted.snapshot() == before, "preferences did not survive restart");
    const auto cleared = ok(restarted.handle({{"op", "preferences.update"},
        {"saved_replies", Json::array()}, {"busy_until", 0}}, true))["preferences"];
    check(cleared["saved_replies"].empty() && cleared["busy_until"] == 0 &&
          cleared["trash_retention_days"] == 7, "could not disable busy mode and remove saved replies");
}

void note_trash_restore_and_purge() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Websites 日本語"},
        {"body", "Personal bookmarks"}, {"tags", {"Personal", "Reference"}}}))["note"];
    const auto id = note["id"];
    ok(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", "https://example.test"},
        {"images", {"saved-image.png"}}, {"client_id", std::string(32, 'a')}}));
    const auto original = ok(f.store.handle({{"op", "note.get"}, {"id", id}}))["note"];
    const auto deleted = ok(f.store.handle({{"op", "note.delete"}, {"id", id}}))["note"];
    check(deleted["deleted_at"].is_number_integer() && deleted["message_count"] == 1,
          "delete omitted restorable metadata");
    check(f.store.snapshot()["notes"].empty() && f.store.snapshot()["trash"].size() == 1,
          "deleted note remained active or was lost");
    fails(f.store.handle({{"op", "note.get"}, {"id", id}}), "not_found");
    fails(f.store.handle({{"op", "note.append"}, {"id", id}, {"text", "must not revive"}}), "not_found");
    const auto listed = ok(f.store.handle({{"op", "note.trash.list"}, {"limit", 1}}));
    check(listed["items"][0] == deleted && listed["total"] == 1 && listed["next_offset"].is_null(),
          "trash list did not return note metadata");
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    ok(restarted.handle({{"op", "note.restore"}, {"id", id}}));
    ok(restarted.handle({{"op", "note.restore"}, {"id", id}}));
    check(restarted.snapshot()["trash"].empty(), "restored note remained in trash");
    check(ok(restarted.handle({{"op", "note.get"}, {"id", id}}))["note"] == original,
          "restore changed IDs, message content, attachments, tags, or timestamps");
    fails(restarted.handle({{"op", "note.purge"}, {"id", id}}, true), "not_found");
    ok(restarted.handle({{"op", "note.delete"}, {"id", id}}));
    fails(restarted.handle({{"op", "note.purge"}, {"id", id}}), "forbidden");
    ok(restarted.handle({{"op", "note.purge"}, {"id", id}}, true));
    check(restarted.snapshot()["trash"].empty(), "permanent deletion failed");
    fails(restarted.handle({{"op", "note.restore"}, {"id", id}}), "not_found");
}

void trash_retention_migration_and_rollback() {
    Fixture f;
    auto active = ok(f.store.handle({{"op", "note.add"}, {"title", "Keep active"}}))["note"];
    auto old = ok(f.store.handle({{"op", "note.add"}, {"title", "Old deleted note"}}))["note"];
    auto recent = ok(f.store.handle({{"op", "note.add"}, {"title", "Recently deleted"}}))["note"];
    ok(f.store.handle({{"op", "note.delete"}, {"id", old["id"]}}));
    ok(f.store.handle({{"op", "note.delete"}, {"id", recent["id"]}}));
    f.store.stop();
    const auto path = f.directory / "state.json";
    auto disk = Json::parse(read_file(path));
    constexpr std::int64_t day = 86400000;
    disk["trash"][0]["deleted_at"] = knock::now_ms() - 31 * day;
    disk["trash"][1]["deleted_at"] = knock::now_ms() - 2 * day;
    write_file(path, disk.dump());
    Store restarted(f.directory);
    restarted.load();
    check(restarted.snapshot()["trash"].size() == 1 && restarted.snapshot()["notes"][0]["id"] == active["id"],
          "retention did not limit cleanup to expired trash");
    const auto before = restarted.snapshot();
    const auto saved = f.directory / "saved.json";
    std::filesystem::rename(path, saved);
    std::filesystem::create_directory(path);
    fails(restarted.handle({{"op", "preferences.update"}, {"trash_retention_days", 1}}, true));
    fails(restarted.handle({{"op", "note.delete"}, {"id", active["id"]}}));
    fails(restarted.handle({{"op", "note.restore"}, {"id", recent["id"]}}));
    fails(restarted.handle({{"op", "note.purge"}, {"id", recent["id"]}}, true));
    check(restarted.snapshot() == before, "failed save changed preferences, active notes, or trash");
    std::filesystem::remove(path);
    std::filesystem::rename(saved, path);
    ok(restarted.handle({{"op", "preferences.update"}, {"trash_retention_days", 1}}, true));
    check(restarted.snapshot()["trash"].empty(), "shorter retention did not apply transactionally");
    restarted.stop();
    auto legacy = Json::parse(read_file(path));
    legacy.erase("trash");
    legacy.erase("preferences");
    write_file(path, legacy.dump());
    Store migrated(f.directory);
    migrated.load();
    check(migrated.snapshot()["trash"].empty() && migrated.snapshot()["preferences"]["trash_retention_days"] == 30 &&
          migrated.snapshot()["notes"][0]["id"] == active["id"], "old state did not migrate without changing notes");
    migrated.stop();
    auto corrupt = Json::parse(read_file(path));
    corrupt["preferences"]["saved_replies"] = {42};
    const auto contents = corrupt.dump();
    write_file(path, contents);
    Store rejected(f.directory);
    bool threw = false;
    try { rejected.load(); } catch (...) { threw = true; }
    check(threw && read_file(path) == contents, "invalid stored preferences were overwritten");
}

void message_edits_keep_identity_and_append_retries() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Editable notes"}}))["note"];
    const Json append{{"op", "note.append"}, {"id", note["id"]}, {"text", "Original café"},
        {"images", {"first.png"}}, {"client_id", std::string(32, 'b')}};
    const auto original = ok(f.store.handle(append))["message"];
    auto command = Json{{"op", "note.message.edit"}, {"id", note["id"]}, {"message_id", original["id"]},
        {"text", "Corrected 日本語"}, {"expected_revision", 0}};
    auto edited = ok(f.store.handle(command))["message"];
    check(edited["id"] == original["id"] && edited["created_at"] == original["created_at"] &&
          edited["images"] == original["images"] && edited["revision"] == 1 && edited.contains("edited_at"),
          "edit replaced identity, attachment, timestamp, or revision");
    check(!edited.contains("append_payload"), "private append payload leaked in edit response");
    check(ok(f.store.handle(append))["message"] == edited, "original append retry was invalidated by editing");
    auto changed_append = append;
    changed_append["text"] = edited["text"];
    fails(f.store.handle(changed_append), "message_client_id_conflict");
    const auto found = ok(f.store.handle({{"op", "note.message.get"}, {"id", note["id"]}, {"message_id", original["id"]}}));
    check(found["message"] == edited && found["message_index"] == 0 && found["message_count"] == 1,
          "message ID lookup returned wrong content or position");
    check(ok(f.store.handle({{"op", "note.find_message"}, {"id", note["id"]}, {"client_id", std::string(32, 'b')}}))["message"] == edited,
          "client token lookup returned an unedited or private message");
    fails(f.store.handle(command), "message_edit_conflict");
    command["expected_revision"] = 1;
    check(ok(f.store.handle(command))["message"] == edited, "no-op edit changed revision or edit timestamp");
    command["text"] = "";
    command["images"] = {"second.png"};
    edited = ok(f.store.handle(command))["message"];
    check(edited["revision"] == 2 && edited["images"] == Json({"second.png"}), "attachment edit failed");
    check(ok(f.store.handle(append))["message"] == edited, "second edit replaced original append retry payload");
    f.store.stop();
    Store restarted(f.directory);
    restarted.load();
    check(ok(restarted.handle(append))["message"] == edited, "append retry identity was lost after restart");
    check(ok(restarted.handle({{"op", "note.get"}, {"id", note["id"]}}))["note"]["messages"][0] == edited,
          "note history leaked original append contents or changed edited message");
    ok(restarted.handle({{"op", "note.delete"}, {"id", note["id"]}}));
    ok(restarted.handle({{"op", "note.restore"}, {"id", note["id"]}}));
    check(ok(restarted.handle(append))["message"] == edited, "Trash/Restore invalidated append identity");
}

void message_edit_conflicts_validation_and_rollback() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Concurrent edits"}}))["note"];
    auto original = ok(f.store.handle({{"op", "note.append"}, {"id", note["id"]}, {"text", "Original"}}))["message"];
    const Json base{{"op", "note.message.edit"}, {"id", note["id"]}, {"message_id", original["id"]},
        {"text", "New text"}, {"expected_revision", 0}};
    const std::vector<Json> invalid = {
        {{"text", " "}}, {{"text", std::string(65537, 'a')}}, {{"text", std::string("\xff", 1)}},
        {{"expected_revision", -1}}, {{"expected_revision", 0.5}}, {{"images", {"../no.png"}}}
    };
    for (const auto& changes : invalid) {
        auto command = base;
        command.update(changes);
        fails(f.store.handle(command));
    }
    auto missing = base;
    missing.erase("expected_revision");
    fails(f.store.handle(missing), "missing_field");
    missing = base;
    missing.erase("text");
    fails(f.store.handle(missing), "missing_field");
    const auto state_path = f.directory / "state.json";
    const auto backup = f.directory / "saved.json";
    std::filesystem::rename(state_path, backup);
    std::filesystem::create_directory(state_path);
    fails(f.store.handle(base));
    check(ok(f.store.handle({{"op", "note.message.get"}, {"id", note["id"]}, {"message_id", original["id"]}}))["message"] == original,
          "failed edit changed the in-memory message");
    std::filesystem::remove(state_path);
    std::filesystem::rename(backup, state_path);
    auto alternate = base;
    alternate["text"] = "Another edit";
    auto first = std::async(std::launch::async, [&] { return f.store.handle(base); });
    auto second = std::async(std::launch::async, [&] { return f.store.handle(alternate); });
    const auto a = first.get(), b = second.get();
    check(a.value("ok", false) != b.value("ok", false), "concurrent edits both overwrote the same revision");
    fails(a.value("ok", false) ? b : a, "message_edit_conflict");
    ok(f.store.handle({{"op", "note.message.remove"}, {"id", note["id"]}, {"message_id", original["id"]}}));
    fails(f.store.handle(base), "message_not_found");
    fails(f.store.handle({{"op", "note.message.get"}, {"id", note["id"]}, {"message_id", original["id"]}}), "message_not_found");
}

void note_search_unicode_paging_and_locations() {
    Fixture f;
    const auto first = ok(f.store.handle({{"op", "note.add"}, {"title", "Websites"}}))["note"];
    const auto second = ok(f.store.handle({{"op", "note.add"}, {"title", "Research"}}))["note"];
    const auto removed = ok(f.store.handle({{"op", "note.add"}, {"title", "Deleted"}}))["note"];
    const auto append = [&](const Json& note, const std::string& text) {
        return ok(f.store.handle({{"op", "note.append"}, {"id", note["id"]}, {"text", text}}))["message"];
    };
    const auto early = append(first, "Keep this before the search result.");
    const auto accented = append(first, "Café — ΩΜΈΓΑ — ЖУРНАЛ — 日本語 🧪");
    const auto repeated = append(first, "café café CAFÉ");
    const auto elsewhere = append(second, "Another CAFÉ link");
    append(removed, "CAFÉ must not appear in active search");
    ok(f.store.handle({{"op", "note.delete"}, {"id", removed["id"]}}));
    const auto before = f.store.snapshot();
    const auto disk_before = read_file(f.directory / "state.json");
    std::set<std::string> found_ids;
    std::size_t offset = 0;
    for (;;) {
        const auto result = ok(f.store.handle({{"op", "note.search"}, {"query", "CAFÉ"}, {"offset", offset}, {"limit", 1}}));
        check(result["total"] == 3 && result["items"].size() == 1, "search paging counted repeats or included Trash");
        const auto& item = result["items"][0];
        check(!item.contains("messages") && !item.contains("images") && item["excerpt"].is_string(),
              "search returned an unbounded note instead of an excerpt");
        check(found_ids.insert(item["message_id"].get<std::string>()).second, "search paging repeated a result");
        if (result["next_offset"].is_null()) break;
        offset = result["next_offset"].get<std::size_t>();
    }
    check(found_ids == std::set<std::string>{accented["id"], repeated["id"], elsewhere["id"]},
          "search lost a Unicode message");
    for (const auto* query : {"café", "ωμέγα", "журнал", "日本語", "🧪"}) {
        const auto result = ok(f.store.handle({{"op", "note.search"}, {"id", first["id"]}, {"query", query}}));
        check(!result["items"].empty() && result["items"][0]["message_id"] == accented["id"] &&
              result["items"][0]["message_index"] == 1, "Unicode search or message location is wrong");
    }
    const auto local = ok(f.store.handle({{"op", "note.search"}, {"id", first["id"]}, {"query", "café"}}));
    check(local["total"] == 2 && local["items"][0]["message_id"] == accented["id"] &&
          local["items"][1]["message_id"] == repeated["id"], "within-note search changed chronological order");
    const auto end = ok(f.store.handle({{"op", "note.search"}, {"query", "café"}, {"offset", 999}}));
    check(end["total"] == 3 && end["items"].empty() && end["next_offset"].is_null(), "past-end search page is wrong");
    check(f.store.snapshot() == before && read_file(f.directory / "state.json") == disk_before,
          "search mutated or rewrote persistent data");
    ok(f.store.handle({{"op", "note.message.remove"}, {"id", first["id"]}, {"message_id", early["id"]}}));
    const auto relocated = ok(f.store.handle({{"op", "note.message.get"}, {"id", first["id"]}, {"message_id", accented["id"]}}));
    check(relocated["message_index"] == 0, "ID lookup used a stale search offset after deletion");
    fails(f.store.handle({{"op", "note.search"}, {"id", removed["id"]}, {"query", "café"}}), "not_found");
}

void note_search_bounds_and_current_content() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Long entries"}}))["note"];
    std::string prefix;
    for (int index = 0; index < 1000; ++index) prefix += "日本語🧪";
    const auto body = prefix + "needle\nCAFÉ\t" + prefix;
    const auto message = ok(f.store.handle({{"op", "note.append"}, {"id", note["id"]}, {"text", body},
        {"client_id", std::string(32, 'c')}}))["message"];
    const auto result = ok(f.store.handle({{"op", "note.search"}, {"query", "NEEDLE"}}));
    const auto excerpt = result["items"][0]["excerpt"].get<std::string>();
    check(excerpt.size() <= 774 && excerpt.find("needle") != std::string::npos &&
          excerpt.starts_with("…") && excerpt.ends_with("…") && excerpt.find('\n') == std::string::npos,
          "long Unicode excerpt is malformed or does not surround its match");
    (void)Json(excerpt).dump();
    const std::vector<Json> invalid = {
        {{"query", ""}}, {{"query", " \t"}}, {{"query", std::string(513, 'x')}},
        {{"query", std::string("\xff", 1)}}, {{"query", 42}}, {{"query", "needle"}, {"offset", -1}},
        {{"query", "needle"}, {"limit", 0}}, {{"query", "needle"}, {"limit", 101}}
    };
    for (auto command : invalid) { command["op"] = "note.search"; fails(f.store.handle(command)); }
    ok(f.store.handle({{"op", "note.message.edit"}, {"id", note["id"]}, {"message_id", message["id"]},
        {"expected_revision", 0}, {"text", "Replacement"}}));
    check(ok(f.store.handle({{"op", "note.search"}, {"query", "needle"}}))["total"] == 0,
          "search exposed private original-append contents after editing");
    const auto fresh = ok(f.store.handle({{"op", "note.search"}, {"query", "replacement"}}));
    check(fresh["total"] == 1 && fresh["items"][0]["message_id"] == message["id"], "edit did not update searchable content");
    ok(f.store.handle({{"op", "note.append"}, {"id", note["id"]}, {"images", {"needle.png"}}}));
    check(ok(f.store.handle({{"op", "note.search"}, {"query", "needle"}}))["total"] == 0,
          "text search treated an attachment filename as a message");
}

void note_search_locations_are_trusted_and_complete() {
    Fixture f;
    const auto note = ok(f.store.handle({{"op", "note.add"}, {"title", "Find in a full conversation"}}))["note"];
    f.store.stop();
    const auto path = f.directory / "state.json";
    auto state = Json::parse(read_file(path));
    auto& messages = state["notes"][0]["messages"];
    for (int index = 0; index < 2000; ++index) {
        auto id = "entry-" + std::to_string(index);
        id.append(128 - id.size(), 'x');
        messages.push_back({{"id", id}, {"text", "CAFÉ 日本語🧪"}, {"images", Json::array()},
            {"created_at", index}, {"revision", 0}});
    }
    write_file(path, state.dump());
    Store restarted(f.directory);
    restarted.load();
    const auto before = read_file(path);
    const Json command{{"op", "note.search_locations"}, {"id", note["id"]}, {"query", "café 日本語🧪"}};
    fails(restarted.handle(command), "forbidden");
    const auto results = ok(restarted.handle(command, true));
    check(results["total"] == 2000 && results["items"].size() == 2000 && !results.contains("next_offset"),
          "in-note search did not return every matching message in one result");
    check(results.dump().size() < 800 * 1024, "longest message IDs exceeded the response budget");
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto& item = results["items"][index];
        check(item.size() == 2 && item["message_id"] == messages[index]["id"] && item["message_index"] == index,
              "locations contained content or changed chronological positions");
    }
    check(read_file(path) == before, "location search rewrote state");
    auto empty = command;
    empty["query"] = "no matches";
    const auto none = ok(restarted.handle(empty, true));
    check(none["total"] == 0 && none["items"].empty(), "unmatched location query returned entries");
    empty["query"] = " ";
    fails(restarted.handle(empty, true), "empty_field");
    ok(restarted.handle({{"op", "note.delete"}, {"id", note["id"]}}));
    fails(restarted.handle(command, true), "not_found");
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"fresh state and invalid input", fresh_state_and_validation},
        {"GUI-only decisions and custom responses", permission_boundaries},
        {"concurrent input lease exclusivity", exclusive_concurrent_approvals},
        {"condition-variable wait and shutdown", waiting_and_stop},
        {"wait budget versus request expiry", wait_budget_and_expiry},
        {"lease expiry and restart revocation", lease_expiry_and_restart},
        {"tasks, notes, Unicode persistence", task_note_unicode_roundtrip},
        {"note tag normalization and persistence", note_tags_roundtrip},
        {"note tag validation and limits", note_tag_validation_and_limits},
        {"legacy note migration and invalid tag preservation", note_tag_migration_and_corruption},
        {"legacy note conversation migration", note_conversation_migration},
        {"note conversation append and restart", note_conversation_append_and_restart},
        {"note conversation limits and rollback", note_conversation_limits_and_rollback},
        {"note conversation paging and concurrent append", note_conversation_paging_and_concurrency},
        {"stored conversation validation and count limit", note_conversation_stored_validation},
        {"note message durable replay and lookup", note_message_replay_and_lookup},
        {"note message token validation and rollback", note_message_token_validation_and_rollback},
        {"job lifecycle and completion replay", jobs_and_idempotent_completion},
        {"bounded reads with large history", bounded_reads_with_large_history},
        {"durable reentrant callbacks", callbacks_are_durable_and_reentrant},
        {"disk failure rollback", failed_save_rolls_back},
        {"corrupt state preservation", corruption_is_preserved},
        {"cancellation and archiving", cancellation_and_archiving},
        {"GUI preferences validation and persistence", preferences_are_validated_and_gui_owned},
        {"note trash restore and permanent deletion", note_trash_restore_and_purge},
        {"trash retention, migration, and save rollback", trash_retention_migration_and_rollback},
        {"message edits preserve append identity", message_edits_keep_identity_and_append_retries},
        {"message edit concurrency, validation, and rollback", message_edit_conflicts_validation_and_rollback},
        {"note search Unicode, paging, and stable locations", note_search_unicode_paging_and_locations},
        {"note search bounds and edited content", note_search_bounds_and_current_content},
        {"complete GUI-only in-note search locations", note_search_locations_are_trusted_and_complete}
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures ? 1 : 0;
}
