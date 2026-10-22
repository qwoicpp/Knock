#include "knock/platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using knock::Json;
using namespace std::chrono_literals;

namespace {
constexpr wchar_t tray_class[] = L"Knock.Tray.v1";

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

template<class Predicate>
void eventually(Predicate predicate, const std::string& description,
                std::chrono::milliseconds limit = 5000ms) {
    const auto end = std::chrono::steady_clock::now() + limit;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < end);
    throw std::runtime_error("Timed out: " + description);
}

LRESULT message(HWND window, UINT id, WPARAM wparam = 0, LPARAM lparam = 0) {
    require(window && IsWindow(window), "Target control no longer exists");
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    const auto delivered = SendMessageTimeoutW(window, id, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                               3000, &result);
    const auto error = GetLastError();
    require(delivered != 0, "Native control message " + std::to_string(id) +
                           " failed or timed out; Win32 error " + std::to_string(error));
    return static_cast<LRESULT>(result);
}

std::wstring window_text(HWND window) {
    const auto length = static_cast<std::size_t>(message(window, WM_GETTEXTLENGTH));
    std::wstring text(length + 1, L'\0');
    const auto written = message(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(written));
    return text;
}

void set_text(HWND window, const std::wstring& value) {
    require(message(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str())) != 0,
            "WM_SETTEXT failed");
    if (GetWindowLongPtrW(window, GWL_STYLE) & ES_MULTILINE) {
        // Windows deliberately omits EN_CHANGE for multiline WM_SETTEXT.
        // A native edit/backspace cycle exercises the same notifications as
        // typing, without sending keyboard input outside this test control.
        message(window, EM_SETSEL, value.size(), static_cast<LPARAM>(value.size()));
        message(window, WM_CHAR, L' ');
        message(window, WM_CHAR, VK_BACK);
    }
}

struct ChildQuery {
    const wchar_t* class_name;
    std::wstring caption;
    int control_id = 0;
    std::vector<HWND> matches;
};

BOOL CALLBACK collect_controls(HWND window, LPARAM context) {
    auto& query = *reinterpret_cast<ChildQuery*>(context);
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    if (_wcsicmp(name, query.class_name) != 0) return TRUE;
    if (query.control_id && GetDlgCtrlID(window) != query.control_id) return TRUE;
    if (!query.caption.empty()) {
        wchar_t text[512]{};
        GetWindowTextW(window, text, 512);
        if (query.caption != text) return TRUE;
    }
    query.matches.push_back(window);
    return TRUE;
}

std::vector<HWND> buttons(HWND root, const wchar_t* caption) {
    ChildQuery query{L"BUTTON", caption, 0, {}};
    EnumChildWindows(root, collect_controls, reinterpret_cast<LPARAM>(&query));
    return query.matches;
}

HWND edit(HWND root, int id) {
    ChildQuery query{L"EDIT", {}, id, {}};
    EnumChildWindows(root, collect_controls, reinterpret_cast<LPARAM>(&query));
    return query.matches.empty() ? nullptr : query.matches.front();
}

HWND button(HWND root, const wchar_t* caption) {
    auto matches = buttons(root, caption);
    require(matches.size() == 1, "Expected exactly one button: " + knock::narrow(caption) +
                                "; found " + std::to_string(matches.size()));
    return matches.front();
}

void click(HWND control) {
    require(IsWindowEnabled(control), "Cannot click a disabled control");
    message(control, BM_CLICK);
}

void enter(HWND control) {
    require(PostMessageW(control, WM_KEYDOWN, VK_RETURN, 0) != 0,
            "Could not post Enter to the chosen test control");
}

void require_no_run_buttons(HWND root) {
    for (HWND control : buttons(root, L""))
        require(!window_text(control).starts_with(L"run /"), "A command-labelled add button remains in the interface");
}

Json send(const Json& command) {
    auto response = knock::send_command(command, false);
    require(response.value("ok", false), "IPC operation failed: " + response.dump());
    return response;
}

Json state() { return send({{"op", "snapshot"}}).at("state"); }

Json saved_note(const std::string& identifier) {
    auto page = send({{"op", "note.get"}, {"id", identifier}});
    Json note = page.at("note");
    while (!page.at("next_offset").is_null()) {
        page = send({{"op", "note.get"}, {"id", identifier}, {"offset", page.at("next_offset")}});
        for (const auto& item : page.at("note").at("messages")) note["messages"].push_back(item);
    }
    return note;
}

Json read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Expected saved JSON file is missing");
    return Json::parse(input);
}

void require_png(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::array<unsigned char, 8> signature{};
    input.read(reinterpret_cast<char*>(signature.data()), signature.size());
    require(input.gcount() == 8 && signature == std::array<unsigned char, 8>{137, 80, 78, 71, 13, 10, 26, 10},
            "Pasted image was not persisted as a PNG");
}

void put_clipboard_image(HWND owner) {
    eventually([&] { return OpenClipboard(owner) != 0; }, "opening test clipboard", 1000ms);
    struct Close { ~Close() { CloseClipboard(); } } close;
    require(EmptyClipboard() != 0, "Cannot replace the private test clipboard");
    constexpr std::array<unsigned char, 16> pixels{
        0, 0, 255, 0, 255, 0, 0, 0,
        255, 0, 0, 255, 255, 255, 0, 0
    };
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = 2;
    header.biHeight = 2;
    header.biPlanes = 1;
    header.biBitCount = 24;
    header.biCompression = BI_RGB;
    header.biSizeImage = pixels.size();
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, sizeof(header) + pixels.size());
    require(block != nullptr, "Cannot allocate clipboard image");
    auto* bytes = static_cast<unsigned char*>(GlobalLock(block));
    if (!bytes) { GlobalFree(block); throw std::runtime_error("Cannot lock clipboard image"); }
    std::memcpy(bytes, &header, sizeof(header));
    std::memcpy(bytes + sizeof(header), pixels.data(), pixels.size());
    GlobalUnlock(block);
    if (!SetClipboardData(CF_DIB, block)) {
        GlobalFree(block);
        throw std::runtime_error("Cannot publish CF_DIB image");
    }
}

Json result(const Json& request) {
    return send({{"op", "request.result"}, {"id", request.at("id")}}).at("request");
}

Json create_request(const char* chat) {
    return send({{"op", "request.create"}, {"chat", chat},
                 {"message", "May I use mouse and keyboard for interface tests?"},
                 {"timeout_seconds", 300}, {"lease_seconds", 300}}).at("request");
}

void archive(const Json& request) {
    send({{"op", "item.dismiss"}, {"kind", "request"}, {"id", request.at("id")}});
}

HWND focus_of(HWND root) {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    const auto thread = GetWindowThreadProcessId(root, nullptr);
    require(GetGUIThreadInfo(thread, &info) != 0, "Cannot inspect GUI focus");
    return info.hwndFocus;
}

RECT screen_rect(HWND window) {
    RECT bounds{};
    require(GetWindowRect(window, &bounds) != 0, "Cannot inspect control bounds");
    return bounds;
}

bool same_rect(RECT a, RECT b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

std::wstring clipboard_text() {
    eventually([] { return OpenClipboard(nullptr) != 0; }, "opening clipboard", 1000ms);
    struct Close { ~Close() { CloseClipboard(); } } close;
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    require(handle != nullptr, "Copy did not publish CF_UNICODETEXT");
    const auto size = GlobalSize(handle);
    require(size >= sizeof(wchar_t) && size <= 1024 * 1024, "Unexpected clipboard text allocation");
    const auto* data = static_cast<const wchar_t*>(GlobalLock(handle));
    require(data != nullptr, "Cannot read clipboard text");
    struct Unlock { HANDLE handle; ~Unlock() { GlobalUnlock(handle); } } unlock{handle};
    const auto end = std::find(data, data + size / sizeof(wchar_t), L'\0');
    require(end != data + size / sizeof(wchar_t), "Clipboard text is not terminated");
    return {data, end};
}

struct Application {
    std::filesystem::path directory;
    std::filesystem::path executable = knock::executable_path().parent_path() / L"KnockTray.exe";
    HANDLE process = nullptr;
    DWORD process_id = 0;
    HWND root = nullptr;

    Application() {
        wchar_t temporary[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temporary) > 0, "Cannot find temporary directory");
        directory = std::filesystem::path(temporary) / (L"knock-ui-tests-" + knock::widen(knock::make_id()));
        require(SetEnvironmentVariableW(L"KNOCK_DATA_DIR", directory.c_str()) != 0,
                "Cannot isolate test data directory");
    }

    ~Application() { try { close(); } catch (...) {} }

    void launch() {
        require(!process, "Test process already running");
        require(std::filesystem::is_regular_file(executable), "KnockTray.exe must be beside ui_tests.exe");
        auto command = knock::command_line({executable.wstring(), L"--show"});
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION created{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                            &startup, &created)) {
            const auto error = GetLastError();
            throw std::runtime_error("Could not launch the test tray: Win32 error " + std::to_string(error));
        }
        CloseHandle(created.hThread);
        process = created.hProcess;
        process_id = created.dwProcessId;
        eventually([&] {
            HWND candidate = FindWindowW(tray_class, nullptr);
            if (!candidate) return false;
            DWORD owner = 0;
            GetWindowThreadProcessId(candidate, &owner);
            require(owner == process_id, "A different Knock process appeared; refusing to control it");
            root = candidate;
            return IsWindowVisible(root) && buttons(root, L"Notes").size() == 1;
        }, "tray window startup");
        eventually([&] {
            try { return knock::send_command({{"op", "snapshot"}}, false).value("ok", false); }
            catch (...) { return false; }
        }, "tray IPC startup");
    }

    void close() {
        if (!process) return;
        if (root && IsWindow(root)) {
            DWORD owner = 0;
            GetWindowThreadProcessId(root, &owner);
            require(owner == process_id, "Refusing to close a window from another process");
            require(message(root, WM_QUERYENDSESSION) != FALSE, "GUI refused to save before session end");
            require(PostMessageW(root, WM_ENDSESSION, TRUE, 0) != 0, "Could not request normal test tray shutdown");
        }
        require(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Test tray did not shut down normally");
        CloseHandle(process);
        process = nullptr;
        process_id = 0;
        root = nullptr;
    }

    void cli(const std::vector<std::wstring>& arguments) {
        const auto executable_cli = executable.parent_path() / L"knock.exe";
        auto full = arguments;
        full.insert(full.begin(), executable_cli.wstring());
        auto command = knock::command_line(full);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(executable_cli.c_str(), command.data(), nullptr, nullptr, FALSE,
                               CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &child) != 0,
                "Could not launch test CLI");
        CloseHandle(child.hThread);
        const auto waited = WaitForSingleObject(child.hProcess, 5000);
        DWORD exit_code = 5;
        GetExitCodeProcess(child.hProcess, &exit_code);
        CloseHandle(child.hProcess);
        require(waited == WAIT_OBJECT_0 && exit_code == 0, "Test CLI failed or timed out");
    }
};

void passed(const char* label) { std::cout << "PASS " << label << '\n' << std::flush; }
} // namespace

int main() {
    if (FindWindowW(tray_class, nullptr)) {
        std::cout << "SKIP: close KnockTray before running native GUI integration tests.\n";
        return 77;
    }
    try {
        Application app;
        app.launch();
        const auto style = GetWindowLongPtrW(app.root, GWL_STYLE);
        const auto extended = GetWindowLongPtrW(app.root, GWL_EXSTYLE);
        require((extended & WS_EX_TOOLWINDOW) && !(extended & WS_EX_APPWINDOW), "Popup can appear in the taskbar");
        require(!(style & WS_CAPTION) && !(style & WS_SYSMENU) && !(style & WS_MINIMIZEBOX), "Popup has window chrome");
        const auto notification_tab = button(app.root, L"Notifications");
        const auto task_tab = button(app.root, L"Task List");
        const auto note_tab = button(app.root, L"Notes");
        const auto tab_bounds = screen_rect(notification_tab);
        passed("tray popup uses toolwindow style with no taskbar or caption");
        const auto original_window = app.root;
        message(app.root, WM_CLOSE);
        eventually([&] { return !IsWindowVisible(app.root); }, "close hides popup");
        require(IsWindow(app.root) && WaitForSingleObject(app.process, 0) == WAIT_TIMEOUT,
                "Closing the popup terminated the tray process");
        message(app.root, WM_APP + 20); // The app's existing reopen message.
        eventually([&] { return IsWindowVisible(app.root); }, "reopen hidden popup");
        require(FindWindowW(tray_class, nullptr) == original_window, "Reopening replaced the tray window");
        passed("close hides the popup and reopen retains the running tray and HWND");
        // The default shortcut is registered under this module ID. Deliver its
        // normal window message directly; do not inject a desktop key chord.
        message(app.root, WM_HOTKEY, 0x7FFF, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        require(IsWindowVisible(app.root), "An unrelated hotkey message toggled the popup");
        message(app.root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        eventually([&] { return !IsWindowVisible(app.root); }, "registered shortcut hides popup");
        message(app.root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        eventually([&] { return IsWindowVisible(app.root); }, "registered shortcut opens popup");
        require(FindWindowW(tray_class, nullptr) == original_window, "Shortcut toggle replaced the tray window");
        passed("registered shortcut dispatch toggles the existing popup and ignores unrelated IDs");

        auto denied = create_request("GUI-NO");
        eventually([&] { return buttons(app.root, L"NO").size() == 1; }, "denial controls");
        click(button(app.root, L"NO"));
        eventually([&] { return result(denied)["status"] == "denied"; }, "GUI denial");
        require(state()["lease"].is_null(), "No granted an input lease");
        archive(denied);
        eventually([&] { return buttons(app.root, L"DISMISS").empty(); }, "denial archived");

        auto custom = create_request("GUI-CUSTOM");
        eventually([&] { return buttons(app.root, L"CUSTOM").size() == 1; }, "custom answer button");
        click(button(app.root, L"CUSTOM"));
        eventually([&] { return edit(app.root, 35) != nullptr; }, "custom edit control");
        const auto reply = edit(app.root, 35);
        const auto custom_text = knock::widen("Not right now — I’m busy.\r\nContinue with headless tests. 日本語");
        set_text(reply, custom_text);
        message(reply, EM_SETSEL, static_cast<WPARAM>(custom_text.size()), static_cast<LPARAM>(custom_text.size()));
        message(reply, WM_CHAR, L'!');
        click(button(app.root, L"SEND"));
        eventually([&] { return result(custom)["status"] == "custom"; }, "GUI custom reply");
        require(result(custom)["response"] == knock::narrow(custom_text + L"!"), "Custom answer text changed");
        require(state()["lease"].is_null(), "Custom reply granted an input lease");
        archive(custom);
        eventually([&] { return buttons(app.root, L"DISMISS").empty(); }, "custom reply archived");
        passed("GUI No and custom replies preserve decisions and exact Unicode text");

        auto first = create_request("GUI-LEASE-A");
        eventually([&] { return buttons(app.root, L"YES").size() == 1; }, "first Yes button");
        HWND first_yes = button(app.root, L"YES");
        auto second = create_request("GUI-LEASE-B");
        eventually([&] { return buttons(app.root, L"YES").size() == 2; }, "second Yes button");
        const auto yes_buttons = buttons(app.root, L"YES");
        const auto other = std::find_if(yes_buttons.begin(), yes_buttons.end(), [&](HWND value) { return value != first_yes; });
        require(other != yes_buttons.end(), "First request control was unexpectedly recreated");
        HWND second_yes = *other;
        click(first_yes);
        eventually([&] { return result(first)["status"] == "approved" && !IsWindowEnabled(second_yes); }, "exclusive lease UI");
        require(state()["lease"]["id"] == first["id"], "GUI Yes granted the wrong request");
        require(result(second)["status"] == "pending", "Other input request was implicitly approved");
        click(button(app.root, L"RELEASE"));
        eventually([&] { return result(first)["status"] == "released" && IsWindowEnabled(second_yes); }, "release re-enables competing request");
        click(second_yes);
        eventually([&] { return result(second)["status"] == "approved" && buttons(app.root, L"RELEASE").size() == 1; }, "second GUI approval");
        click(button(app.root, L"RELEASE"));
        eventually([&] { return state()["lease"].is_null(); }, "second lease release");
        archive(first);
        archive(second);
        require(button(app.root, L"Notifications") == notification_tab && button(app.root, L"Task List") == task_tab &&
                button(app.root, L"Notes") == note_tab && same_rect(tab_bounds, screen_rect(notification_tab)),
                "State updates recreated or moved the tab controls");
        passed("GUI Yes, exclusive lease disable, release, and stable tab controls");

        const std::string continuation = "@COPY-1: the build finished.\r\nReview job 123 and continue — 日本語 ✓";
        const auto notice = send({{"op", "notify"}, {"chat", "COPY-1"}, {"message", "Build finished; copy the continuation."},
                                  {"resume_text", continuation}}).at("job");
        eventually([&] { return buttons(app.root, L"COPY").size() == 1; }, "continuation Copy button");
        click(button(app.root, L"COPY"));
        require(clipboard_text() == knock::widen(continuation), "Copy did not preserve the exact continuation text");
        send({{"op", "item.dismiss"}, {"kind", "job"}, {"id", notice["id"]}});
        eventually([&] { return buttons(app.root, L"COPY").empty(); }, "copied notification archived");
        passed("actual Copy button publishes the exact Unicode continuation to the clipboard");

        click(note_tab);
        eventually([&] { return edit(app.root, 30) != nullptr; }, "notes footer");
        require_no_run_buttons(app.root);
        set_text(edit(app.root, 30), L"Working note");
        enter(edit(app.root, 30));
        eventually([&] { return edit(app.root, 37) != nullptr; }, "blank conversation composer");
        const auto working_id = state()["notes"].back()["id"].get<std::string>();
        auto composer = edit(app.root, 37);
        require(focus_of(app.root) == composer && window_text(composer).empty(),
                "Opening a new conversation did not focus its blank composer");
        require(!edit(app.root, 31) && !edit(app.root, 32) && saved_note(working_id)["messages"].empty(),
                "New conversation contains placeholder content or old note editors");
        const auto sent_text = knock::widen("A useful website: https://example.com/\r\nCafé — 日本語 ✓");
        set_text(composer, sent_text);
        auto incoming = create_request("GUI-DURING-EDIT");
        std::this_thread::sleep_for(200ms);
        message(app.root, WM_NULL);
        require(edit(app.root, 37) == composer && window_text(composer) == sent_text &&
                focus_of(app.root) == composer, "Incoming request replaced the composer, draft, or focus");
        require(saved_note(working_id)["messages"].empty(), "Typing or an incoming request silently sent a draft");
        click(button(app.root, L"SEND"));
        eventually([&] { return saved_note(working_id)["messages"].size() == 1; }, "Unicode conversation message sent");
        require(saved_note(working_id)["messages"][0]["text"] == knock::narrow(sent_text),
                "Send changed Unicode text or CRLF line breaks");
        require(edit(app.root, 37) == composer && window_text(composer).empty() && focus_of(app.root) == composer,
                "Send destroyed the composer or failed to clear and refocus it");
        passed("blank conversation, exact Unicode multiline Send, and stable composer during incoming requests");

        put_clipboard_image(app.root);
        message(composer, WM_PASTE);
        const auto working_draft_path = app.directory / L"drafts" / (knock::widen(working_id) + L".conversation.json");
        eventually([&] {
            return std::filesystem::is_regular_file(working_draft_path) &&
                   read_json(working_draft_path).value("images", Json::array()).size() == 1;
        }, "clipboard image staged in unsent draft");
        require(saved_note(working_id)["messages"].size() == 1, "Pasting an image silently sent a message");
        click(button(app.root, L"SEND"));
        eventually([&] { return saved_note(working_id)["messages"].size() == 2; }, "image-only message sent");
        const auto image_message = saved_note(working_id)["messages"][1];
        require(image_message["text"] == "" && image_message["images"].size() == 1,
                "Image-only send created the wrong message content");
        require_png(app.directory / L"note-images" / knock::widen(image_message["images"][0].get<std::string>()));
        require(!std::filesystem::exists(working_draft_path), "Sent image remained in the unsent draft file");
        const auto working_messages = saved_note(working_id)["messages"];
        passed("real CF_DIB clipboard paste produces an image-only entry and a durable PNG");

        const auto external_draft = knock::widen("Still thinking about this\r\nUnsent — 日本語");
        set_text(composer, external_draft);
        app.cli({L"notes", L"update", knock::widen(working_id), L"--tag", L"External"});
        std::this_thread::sleep_for(200ms);
        message(app.root, WM_NULL);
        require(edit(app.root, 37) == composer && window_text(composer) == external_draft &&
                focus_of(app.root) == composer, "External tag edit replaced the active composer or its draft");
        require(saved_note(working_id)["messages"] == working_messages, "External tag edit changed messages");
        click(button(app.root, L"EDIT"));
        eventually([&] { return edit(app.root, 31) && edit(app.root, 32); }, "conversation description editors");
        const auto metadata_body = edit(app.root, 32);
        const std::wstring description = L"Websites and references. " + std::wstring(700, L'x');
        set_text(metadata_body, description);
        app.cli({L"notes", L"update", knock::widen(working_id), L"--tag", L"Reference"});
        std::this_thread::sleep_for(100ms);
        message(app.root, WM_NULL);
        require(edit(app.root, 32) == metadata_body && window_text(metadata_body) == description,
                "External tag edit replaced the unsaved description");
        click(button(app.root, L"EDIT"));
        eventually([&] { return !edit(app.root, 31) && !edit(app.root, 32) && IsWindowVisible(edit(app.root, 37)); },
                   "return from description to conversation");
        require(saved_note(working_id)["body"] == knock::narrow(description) &&
                saved_note(working_id)["tags"] == Json::array({"Reference"}) &&
                saved_note(working_id)["messages"] == working_messages,
                "Description save lost full text, external tags, or conversation history");
        require(edit(app.root, 37) == composer && window_text(composer) == external_draft,
                "Editing metadata lost the unsent conversation draft");
        click(task_tab);
        eventually([&] { return edit(app.root, 30) != nullptr; }, "task list after draft");
        click(note_tab);
        eventually([&] { return buttons(app.root, L"Working note").size() == 1; }, "working conversation card");
        click(button(app.root, L"Working note"));
        eventually([&] { return edit(app.root, 37) != nullptr; }, "reopened unsent conversation");
        composer = edit(app.root, 37);
        require(window_text(composer) == external_draft && saved_note(working_id)["messages"] == working_messages,
                "Tab switch lost a draft or implicitly sent it");
        message(app.root, WM_CLOSE);
        eventually([&] { return !IsWindowVisible(app.root); }, "hide with a conversation draft");
        message(app.root, WM_APP + 20);
        eventually([&] { return IsWindowVisible(app.root); }, "reopen with a conversation draft");
        require(edit(app.root, 37) == composer && window_text(composer) == external_draft &&
                saved_note(working_id)["messages"] == working_messages,
                "Hide/reopen changed the unsent draft or history");
        click(button(app.root, L"EDIT"));
        eventually([&] { return edit(app.root, 32) != nullptr; }, "reopened full description");
        require(window_text(edit(app.root, 32)) == description, "Opening metadata loaded a truncated snapshot preview");
        click(button(app.root, L"EDIT"));
        passed("external tags, metadata, tabs, and hide preserve the unsent draft and existing message IDs");

        const std::wstring deleted_draft = L"Keep this fresh draft even if an agent removes its note";
        set_text(edit(app.root, 37), deleted_draft);
        app.cli({L"notes", L"remove", knock::widen(working_id)});
        eventually([&] { return !edit(app.root, 37) && edit(app.root, 30); }, "removed note exits stale conversation");
        require(state()["notes"].empty(), "CLI deletion did not remove the note");
        require(read_json(working_draft_path)["text"] == knock::narrow(deleted_draft),
                "External deletion lost the latest unsent draft");
        set_text(edit(app.root, 30), L"Persistent note");
        enter(edit(app.root, 30));
        eventually([&] { return edit(app.root, 37) != nullptr; }, "new conversation after external deletion");
        const auto persistent_body = knock::widen("Saved across restart.\r\nA thought to keep — 日本語.");
        const auto persistent_id = state()["notes"].back()["id"].get<std::string>();
        set_text(edit(app.root, 37), persistent_body);
        enter(edit(app.root, 37));
        eventually([&] { return saved_note(persistent_id)["messages"].size() == 1; }, "Enter sends a conversation message");
        const auto persistent_messages = saved_note(persistent_id)["messages"];
        require(persistent_messages[0]["text"] == knock::narrow(persistent_body), "Enter changed the sent text");
        click(button(app.root, L"TAGS"));
        eventually([&] { return edit(app.root, 36) != nullptr; }, "note tag selector");
        click(button(app.root, L"[ ] Personal"));
        eventually([&] { return buttons(app.root, L"[x] Personal").size() == 1; }, "Personal tag selected");
        click(button(app.root, L"[ ] Ideas"));
        eventually([&] { return saved_note(persistent_id)["tags"] == Json::array({"Personal", "Ideas"}); },
                   "tag choices autosaved");
        click(button(app.root, L"DONE"));
        click(button(app.root, L"TAGS"));
        eventually([&] { return edit(app.root, 36) != nullptr; }, "reopened tag selector query");
        const auto tag_query = edit(app.root, 36);
        const auto custom_tag = knock::widen("旅行の計画");
        set_text(tag_query, custom_tag);
        require(focus_of(app.root) == tag_query, "Tag selector did not retain query focus");
        auto tag_incoming = create_request("GUI-DURING-TAGS");
        std::this_thread::sleep_for(200ms);
        message(app.root, WM_NULL);
        require(edit(app.root, 36) == tag_query && window_text(tag_query) == custom_tag && focus_of(app.root) == tag_query,
                "Incoming request lost the tag query, control, or focus");
        click(button(app.root, L"CREATE TAG"));
        const auto expected_tags = Json::array({"Personal", "Ideas", knock::narrow(custom_tag)});
        eventually([&] { return saved_note(persistent_id)["tags"] == expected_tags; }, "new Unicode tag saved");
        click(button(app.root, L"DONE"));
        eventually([&] { return !edit(app.root, 36) && IsWindowVisible(edit(app.root, 37)); }, "tag selector closed");
        require(saved_note(persistent_id)["messages"] == persistent_messages, "Tag editing changed saved messages");
        const auto restart_draft = knock::widen("Unsent after restart\r\nStill deciding — café ✓");
        set_text(edit(app.root, 37), restart_draft);
        put_clipboard_image(app.root);
        message(edit(app.root, 37), WM_PASTE);
        const auto persistent_draft_path = app.directory / L"drafts" / (knock::widen(persistent_id) + L".conversation.json");
        eventually([&] {
            return std::filesystem::is_regular_file(persistent_draft_path) &&
                   read_json(persistent_draft_path).value("images", Json::array()).size() == 1;
        }, "unsent text and image saved for restart");
        const auto restart_images = read_json(persistent_draft_path)["images"];
        click(button(app.root, L"< BACK"));
        passed("CLI deletion preserves the unsent draft and new conversations remain usable");
        passed("conversation tags select, create Unicode labels, and retain query focus through requests");

        const auto unrelated = send({{"op", "note.add"}, {"title", "Untagged personal note"},
                                     {"body", "A separate note for checking the tag filter."}}).at("note");
        eventually([&] { return buttons(app.root, L"Untagged personal note").size() == 1; }, "second note card");
        click(button(app.root, L"FILTER"));
        eventually([&] { return edit(app.root, 36) != nullptr; }, "notes tag filter query");
        set_text(edit(app.root, 36), custom_tag);
        // Post Enter through the app's main message loop, where keyboard
        // navigation is handled, rather than calling an internal action.
        require(PostMessageW(edit(app.root, 36), WM_KEYDOWN, VK_RETURN, 0) != 0,
                "Could not submit the tag filter");
        eventually([&] {
            return !edit(app.root, 36) && buttons(app.root, L"Persistent note").size() == 1 &&
                   buttons(app.root, L"Untagged personal note").empty();
        }, "Unicode tag filter shows only its matching note");
        click(button(app.root, L"FILTER"));
        eventually([&] { return buttons(app.root, L"All notes").size() == 1; }, "reset tag filter option");
        click(button(app.root, L"All notes"));
        eventually([&] {
            return buttons(app.root, L"Persistent note").size() == 1 &&
                   buttons(app.root, L"Untagged personal note").size() == 1;
        }, "All notes restores both cards");
        click(button(app.root, L"FILTER"));
        eventually([&] { return buttons(app.root, L"Untagged").size() == 1; }, "untagged filter option");
        click(button(app.root, L"Untagged"));
        eventually([&] {
            return buttons(app.root, L"Persistent note").empty() &&
                   buttons(app.root, L"Untagged personal note").size() == 1;
        }, "untagged filter excludes tagged notes");
        click(button(app.root, L"FILTER"));
        eventually([&] { return buttons(app.root, L"All notes").size() == 1; }, "clear untagged filter");
        click(button(app.root, L"All notes"));
        require(saved_note(unrelated["id"].get<std::string>())["tags"].empty(), "Filtering modified an untagged note");
        passed("note list filters by a Unicode tag or untagged state and resets to all notes");

        click(task_tab);
        eventually([&] { return edit(app.root, 30) != nullptr; }, "task footer");
        set_text(edit(app.root, 30), L"Interface regression task");
        require_no_run_buttons(app.root);
        enter(edit(app.root, 30));
        eventually([&] { return buttons(app.root, L"Interface regression task").size() == 1; }, "new task card");
        click(button(app.root, L"Interface regression task"));
        eventually([&] { return edit(app.root, 33) && edit(app.root, 34); }, "task metadata editors");
        set_text(edit(app.root, 32), L"Native GUI regression coverage");
        set_text(edit(app.root, 33), L"GUI-1");
        set_text(edit(app.root, 34), L"KNOCK");
        click(button(app.root, L"IN PROGRESS"));
        eventually([&] { return state()["tasks"].back()["status"] == "in_progress"; }, "task in progress");
        click(button(app.root, L"DONE"));
        eventually([&] { return state()["tasks"].back()["status"] == "done"; }, "task complete");
        const auto task = state()["tasks"].back();
        require(task["operator"] == "GUI-1" && task["project"] == "KNOCK" && task["body"] == "Native GUI regression coverage",
                "Task status change lost edited metadata");
        passed("task detail metadata and status controls persist their changes");

        send({{"op", "request.cancel"}, {"id", incoming["id"]}});
        send({{"op", "request.cancel"}, {"id", tag_incoming["id"]}});
        const auto before_restart = state();
        require(saved_note(persistent_id)["messages"] == persistent_messages,
                "An unsent text/image draft reached history before restart");
        app.close();
        app.launch();
        require(state() == before_restart, "Normal quit and relaunch changed persisted data");
        click(button(app.root, L"Notes"));
        eventually([&] { return buttons(app.root, L"Persistent note").size() == 1; }, "persisted note card");
        click(button(app.root, L"Persistent note"));
        eventually([&] { return edit(app.root, 37) != nullptr; }, "persisted conversation composer");
        require(window_text(edit(app.root, 37)) == restart_draft,
                "Restored unsent draft differs from its exact text");
        require(saved_note(persistent_id)["messages"] == persistent_messages,
                "Restart changed message IDs/text or silently sent the draft");
        require(read_json(persistent_draft_path)["images"] == restart_images,
                "Restart lost the unsent attachment reference");
        require_png(app.directory / L"note-images" / knock::widen(restart_images[0].get<std::string>()));
        require(saved_note(persistent_id)["tags"] == expected_tags, "Saved note tags changed across restart");
        click(button(app.root, L"TAGS"));
        eventually([&] {
            return buttons(app.root, L"[x] Personal").size() == 1 && buttons(app.root, L"[x] Ideas").size() == 1 &&
                   buttons(app.root, (L"[x] " + custom_tag).c_str()).size() == 1;
        }, "restored tag selector shows saved selections");
        click(button(app.root, L"DONE"));
        require(window_text(edit(app.root, 37)) == restart_draft, "Restored tag selector replaced the draft");
        click(button(app.root, L"SEND"));
        eventually([&] { return saved_note(persistent_id)["messages"].size() == 2; }, "explicitly send restored draft");
        const auto restored = saved_note(persistent_id)["messages"];
        require(restored[0] == persistent_messages[0] && restored[1]["text"] == knock::narrow(restart_draft) &&
                restored[1]["images"] == restart_images,
                "Sending the restored draft changed previous history or lost its text/image");
        require(window_text(edit(app.root, 37)).empty() && !std::filesystem::exists(persistent_draft_path),
                "Restored draft was not cleared after explicit Send");
        app.close();
        // Model the crash boundary after the append committed but before the
        // unsent-draft file was cleared. Use the real committed submission ID.
        require(restored[1].contains("client_id") && restored[1]["client_id"].is_string(),
                "A UI submission did not retain its durable client ID");
        {
            std::ofstream stale(persistent_draft_path, std::ios::binary | std::ios::trunc);
            stale << Json{{"note_id", persistent_id}, {"title", "Persistent note"},
                          {"text", restored[1]["text"]}, {"images", restored[1]["images"]},
                          {"client_id", restored[1]["client_id"]}}.dump();
            stale.flush();
            require(static_cast<bool>(stale), "Cannot prepare the stale committed-draft fixture");
        }
        app.launch();
        click(button(app.root, L"Notes"));
        eventually([&] { return buttons(app.root, L"Persistent note").size() == 1; }, "stale-draft note card");
        click(button(app.root, L"Persistent note"));
        eventually([&] { return edit(app.root, 37) != nullptr; }, "stale-draft recovery composer");
        require(window_text(edit(app.root, 37)).empty() && saved_note(persistent_id)["messages"] == restored,
                "A committed stale draft was restored as unsent content or duplicated in history");
        require(!std::filesystem::exists(persistent_draft_path), "Committed stale draft was not consumed");
        require_png(app.directory / L"note-images" / knock::widen(restart_images[0].get<std::string>()));
        app.close();
        std::filesystem::remove_all(app.directory);
        passed("restart preserves history IDs, unsent text/images, tags, tasks, and decisions until explicit Send");
        passed("committed stale-draft recovery keeps one message and retains its PNG");
        std::cout << "All native GUI integration checks passed. No global input was generated.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL native GUI integration: " << error.what() << '\n';
        return 1;
    }
}
