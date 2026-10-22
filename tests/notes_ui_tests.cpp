#include "knock/platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

void require(bool condition, const std::string& description) {
    if (!condition) throw std::runtime_error(description);
}
template<class Predicate>
void eventually(Predicate predicate, const char* description) {
    const auto until = std::chrono::steady_clock::now() + 6s;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error(std::string("Timed out: ") + description);
}
LRESULT message(HWND window, UINT type, WPARAM first = 0, LPARAM second = 0) {
    require(window && IsWindow(window), "Test control does not exist");
    DWORD_PTR result = 0;
    SetLastError(ERROR_SUCCESS);
    if (!SendMessageTimeoutW(window, type, first, second, SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result)) {
        const auto error = GetLastError();
        throw std::runtime_error("Native test message failed: type=" + std::to_string(type) +
            " control=" + std::to_string(GetDlgCtrlID(window)) + " error=" + std::to_string(error));
    }
    return static_cast<LRESULT>(result);
}
std::wstring caption(HWND window) {
    std::wstring text(static_cast<std::size_t>(message(window, WM_GETTEXTLENGTH)) + 1, L'\0');
    const auto size = message(window, WM_GETTEXT, text.size(), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<std::size_t>(size));
    return text;
}
struct Controls { const wchar_t* kind; int id; std::wstring text; std::vector<HWND> items; };
BOOL CALLBACK collect(HWND window, LPARAM context) {
    auto& query = *reinterpret_cast<Controls*>(context);
    wchar_t kind[32]{};
    GetClassNameW(window, kind, 32);
    if (_wcsicmp(kind, query.kind) || (query.id && GetDlgCtrlID(window) != query.id)) return TRUE;
    if (!query.text.empty() && caption(window) != query.text) return TRUE;
    query.items.push_back(window);
    return TRUE;
}
std::vector<HWND> buttons(HWND root, const std::wstring& text = {}) {
    Controls query{L"BUTTON", 0, text, {}};
    EnumChildWindows(root, collect, reinterpret_cast<LPARAM>(&query));
    return query.items;
}
HWND button(HWND root, const std::wstring& text) {
    const auto found = buttons(root, text);
    require(found.size() == 1, "Expected one button: " + knock::narrow(text));
    return found.front();
}
HWND edit(HWND root, int id) {
    Controls query{L"EDIT", id, {}, {}};
    EnumChildWindows(root, collect, reinterpret_cast<LPARAM>(&query));
    return query.items.empty() ? nullptr : query.items.front();
}
RECT control_bounds(HWND control) {
    RECT bounds{};
    require(control && IsWindowVisible(control) && GetWindowRect(control, &bounds), "Visible control bounds are unavailable");
    return bounds;
}
void inside_parent(HWND control) {
    const auto bounds = control_bounds(control);
    const auto parent = control_bounds(GetParent(control));
    require(bounds.left >= parent.left && bounds.top >= parent.top && bounds.right <= parent.right &&
            bounds.bottom <= parent.bottom && bounds.right > bounds.left && bounds.bottom > bounds.top,
            "Control is clipped at minimum size: " + knock::narrow(caption(control)));
}
void click(HWND control) {
    require(IsWindowEnabled(control), "Test action is disabled");
    const auto label = knock::narrow(caption(control));
    try { message(control, BM_CLICK); }
    catch (const std::exception& error) {
        throw std::runtime_error("Click " + label + ": " + error.what());
    }
}
void set_text(HWND control, const std::wstring& text) {
    message(control, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
    if (GetWindowLongPtrW(control, GWL_STYLE) & ES_MULTILINE) {
        message(control, EM_SETSEL, text.size(), text.size());
        message(control, WM_CHAR, L' ');
        message(control, WM_CHAR, VK_BACK);
    }
}
void enter(HWND control) { message(control, WM_KEYDOWN, VK_RETURN); }
Json send(const Json& command) {
    auto result = knock::send_command(command, false);
    require(result.value("ok", false), "IPC operation failed: " + result.dump());
    return result;
}
Json snapshot() { return send({{"op", "snapshot"}}).at("state"); }
Json entry(const std::string& note, const std::string& id) {
    return send({{"op", "note.message.get"}, {"id", note}, {"message_id", id}}).at("message");
}
Json read_json(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    require(static_cast<bool>(file), "Expected draft file is missing");
    return Json::parse(file);
}
GUITHREADINFO gui(HWND root) {
    GUITHREADINFO info{};
    info.cbSize = sizeof(info);
    require(GetGUIThreadInfo(GetWindowThreadProcessId(root, nullptr), &info) != 0, "Cannot inspect test UI thread");
    return info;
}
void screenshot(HWND root, const wchar_t* filename);
void menu(HWND root, HWND source, LPARAM point, std::initializer_list<UINT> keys) {
    require(source == root || IsChild(root, source), "Refusing another process's menu source");
    require(PostMessageW(source, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(source), point) != 0,
            "Cannot open native context menu");
    HWND owner = nullptr;
    try {
        eventually([&] {
            const auto info = gui(root);
            owner = info.hwndMenuOwner;
            return (info.flags & GUI_INMENUMODE) && owner;
        }, "native menu opens");
    } catch (...) {
        const auto info = gui(root);
        std::cerr << "Menu timeout: visible=" << IsWindowVisible(root) << " source=" << source
                  << " flags=" << info.flags << " active=" << info.hwndActive
                  << " focus=" << info.hwndFocus << " owner=" << info.hwndMenuOwner << '\n';
        screenshot(root, L"note-menu-failure.bmp");
        throw;
    }
    require(GetWindowThreadProcessId(owner, nullptr) == GetWindowThreadProcessId(root, nullptr),
            "Refusing another process's menu");
    for (const auto key : keys) PostMessageW(owner, WM_KEYDOWN, key, 1);
    eventually([&] { return !(gui(root).flags & GUI_INMENUMODE); }, "native menu closes");
    message(root, WM_NULL);
}
void entry_menu(HWND root, int y, std::initializer_list<UINT> keys) {
    const auto content = FindWindowExW(root, nullptr, L"Knock.Content.v1", nullptr);
    POINT point{18, y};
    ClientToScreen(content, &point);
    menu(root, content, MAKELPARAM(point.x, point.y), keys);
}
void open_settings(HWND root) {
    menu(root, root, -1, {VK_HOME, VK_RETURN});
    eventually([&] { return IsWindowVisible(edit(root, 41)); }, "Settings opens");
}
void ctrl_find(HWND root) {
    // Share only the test app's input queue. SetKeyboardState changes the
    // queue state used by GetKeyState; it does not generate desktop input.
    const DWORD thread = GetWindowThreadProcessId(root, nullptr);
    require(AttachThreadInput(GetCurrentThreadId(), thread, TRUE) != 0, "Cannot attach test input queue");
    std::array<BYTE, 256> previous{};
    GetKeyboardState(previous.data());
    auto pressed = previous;
    pressed[VK_CONTROL] = 0x80;
    SetKeyboardState(pressed.data());
    try { message(edit(root, 37), WM_KEYDOWN, 'F'); }
    catch (...) {
        SetKeyboardState(previous.data());
        AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
        throw;
    }
    SetKeyboardState(previous.data());
    AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
    eventually([&] { return IsWindowVisible(edit(root, 52)); }, "Ctrl+F opens note search");
}
std::wstring clipboard() {
    eventually([] { return OpenClipboard(nullptr) != 0; }, "clipboard opens");
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    require(handle != nullptr, "Entry copy did not write text");
    const auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
    require(text != nullptr, "Cannot read copied entry");
    const std::wstring result(text);
    GlobalUnlock(handle);
    CloseClipboard();
    return result;
}
void command(HWND root, const std::wstring& text) {
    set_text(edit(root, 30), text);
    enter(edit(root, 30));
    eventually([&] { return IsWindowVisible(edit(root, 51)); }, "note command page opens");
}
void back(HWND root) { click(button(root, L"< BACK")); }
void answer_delete(HWND root, int answer) {
    PostMessageW(button(root, L"DELETE FOREVER"), BM_CLICK, 0, 0);
    HWND dialog = nullptr;
    eventually([&] {
        dialog = FindWindowW(L"#32770", L"Knock");
        return dialog && GetWindow(dialog, GW_OWNER) == root;
    }, "permanent-delete confirmation opens");
    require(GetWindowThreadProcessId(dialog, nullptr) == GetWindowThreadProcessId(root, nullptr),
            "Refusing another process's dialog");
    click(GetDlgItem(dialog, answer));
    eventually([&] { return !IsWindow(dialog); }, "delete confirmation closes");
}
void start_busy_timer(HWND root) {
    PostMessageW(button(root, L"SET TIMER"), BM_CLICK, 0, 0);
    HWND owner = nullptr;
    eventually([&] {
        const auto info = gui(root);
        owner = info.hwndMenuOwner;
        return (info.flags & GUI_INMENUMODE) && owner;
    }, "busy timer choices open");
    require(GetWindowThreadProcessId(owner, nullptr) == GetWindowThreadProcessId(root, nullptr),
            "Refusing another process's menu");
    PostMessageW(owner, WM_KEYDOWN, VK_HOME, 1);
    PostMessageW(owner, WM_KEYDOWN, VK_RETURN, 1);
    eventually([&] { return snapshot().at("preferences").at("busy_until").get<std::int64_t>() > knock::now_ms(); },
               "busy timer starts");
}
void passed(const char* label) { std::cout << "PASS " << label << '\n' << std::flush; }
void screenshot(HWND root, const wchar_t* filename) {
    wchar_t folder[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"KNOCK_TEST_SCREENSHOTS", folder, MAX_PATH)) return;
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RECT bounds{};
    GetWindowRect(root, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    HDC source = GetWindowDC(root);
    HDC memory = CreateCompatibleDC(source);
    HBITMAP bitmap = CreateCompatibleBitmap(source, width, height);
    const auto previous = SelectObject(memory, bitmap);
    BitBlt(memory, 0, 0, width, height, source, 0, 0, SRCCOPY);
    SelectObject(memory, previous);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    const int rows = GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(root, source);
    require(rows == height, "Cannot capture the test app");
    BITMAPFILEHEADER file{};
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info.bmiHeader);
    file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size());
    std::filesystem::create_directories(folder);
    std::ofstream output(std::filesystem::path(folder) / filename, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    require(static_cast<bool>(output), "Cannot save test screenshot");
}

struct Application {
    std::filesystem::path directory;
    HANDLE process = nullptr;
    DWORD pid = 0;
    HWND root = nullptr;
    Application() {
        wchar_t path[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, path) != 0, "Cannot find temporary folder");
        directory = std::filesystem::path(path) / (L"knock-notes-ui-" + knock::widen(knock::make_id()));
        require(SetEnvironmentVariableW(L"KNOCK_DATA_DIR", directory.c_str()) != 0, "Cannot isolate test state");
    }
    ~Application() { try { close(); } catch (...) {} }
    void launch() {
        const auto exe = knock::executable_path().parent_path() / L"KnockTray.exe";
        auto args = knock::command_line({exe.wstring(), L"--show"});
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                               nullptr, exe.parent_path().c_str(), &startup, &child) != 0, "Cannot start test app");
        CloseHandle(child.hThread);
        process = child.hProcess;
        pid = child.dwProcessId;
        eventually([&] {
            root = FindWindowW(tray_class, nullptr);
            if (!root) return false;
            DWORD owner = 0;
            GetWindowThreadProcessId(root, &owner);
            require(owner == pid, "Refusing another Knock process");
            return IsWindowVisible(root) && buttons(root, L"Notes").size() == 1;
        }, "test app starts");
        eventually([&] {
            try { return knock::send_command({{"op", "status"}}, false).value("ok", false); }
            catch (...) { return false; }
        }, "test IPC starts");
        message(root, WM_CLOSE);
        message(root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        eventually([&] { return IsWindowVisible(root); }, "pinned popup opens");
    }
    void close() {
        if (!process) return;
        if (root && IsWindow(root)) {
            DWORD owner = 0;
            GetWindowThreadProcessId(root, &owner);
            require(owner == pid, "Refusing another process shutdown");
            require(message(root, WM_QUERYENDSESSION) != FALSE, "App could not save before shutdown");
            PostMessageW(root, WM_ENDSESSION, TRUE, 0);
        }
        require(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Test app did not stop");
        CloseHandle(process);
        process = nullptr;
        root = nullptr;
    }
};
} // namespace

int main() {
    if (FindWindowW(tray_class, nullptr)) {
        std::cout << "SKIP: close Knock before running the notes UI checks.\n";
        return 77;
    }
    try {
        Application app;
        std::string archive;
        {
            // Seed large history while the tray is off. Every search and jump
            // below still uses the real controls and production IPC reads.
            knock::Store fixture(app.directory);
            fixture.load();
            const auto added = fixture.handle({{"op", "note.add"}, {"title", "Archive"}});
            require(added.value("ok", false), "Cannot create the history fixture");
            archive = added.at("note").at("id").get<std::string>();
            for (int index = 0; index < 220; ++index) {
                const auto text = (index == 0 || index == 120 ? "Needle café entry " : "Ordinary entry ") + std::to_string(index);
                require(fixture.handle({{"op", "note.append"}, {"id", archive}, {"text", text}}).value("ok", false),
                        "Cannot seed the history fixture");
            }
        }
        app.launch();
        std::filesystem::create_directories(app.directory / L"note-images");
        std::filesystem::copy_file(knock::executable_path().parent_path() / L"assets" / L"note.png",
                                   app.directory / L"note-images" / L"reference.png");
        const auto note = send({{"op", "note.add"}, {"title", "Websites"}, {"body", "handy links for later"}, {"tags", {"Personal"}}}).at("note");
        const auto id = note.at("id").get<std::string>();
        const auto original = send({{"op", "note.append"}, {"id", id}, {"text", "https://developer.mozilla.org/"},
                                   {"images", {"reference.png"}}}).at("message");
        const auto message_id = original.at("id").get<std::string>();
        click(button(app.root, L"Notes"));
        command(app.root, L"/delete-note Websites");
        enter(edit(app.root, 51));
        require(snapshot().at("notes").size() == 2, "Enter on delete query deleted a note");
        click(button(app.root, L"DELETE Websites"));
        eventually([&] { return snapshot().at("notes").size() == 1; }, "note enters Trash");
        require(snapshot().at("trash").front().at("id") == id, "Trash changed the note ID");
        click(button(app.root, L"UNDO"));
        eventually([&] { return snapshot().at("trash").empty(); }, "Undo restores note");
        require(entry(id, message_id) == original, "Undo changed the message or attachment references");
        require(send({{"op", "note.get"}, {"id", id}}).at("note").at("tags") == Json::array({"Personal"}), "Undo lost tags");
        passed("click deletion moves notes to Trash; Undo restores original IDs, entries, images and tags");
        back(app.root);
        command(app.root, L"/delete-note Websites");
        click(button(app.root, L"DELETE Websites"));
        back(app.root);
        command(app.root, L"/trash");
        eventually([&] { return buttons(app.root, L"RESTORE Websites").size() == 1; }, "Trash lists deleted note");
        screenshot(app.root, L"note-trash.bmp");
        std::this_thread::sleep_for(600ms); // Rows protect against a double-click deleting/restoring the next note.
        click(button(app.root, L"RESTORE Websites"));
        eventually([&] { return snapshot().at("trash").empty(); }, "Trash Restore works");
        back(app.root);
        passed("Trash restore returns the same complete note to the normal list");
        send({{"op", "note.add"}, {"title", "Temporary"}});
        command(app.root, L"/delete-note Temporary");
        click(button(app.root, L"DELETE Temporary"));
        back(app.root);
        command(app.root, L"/trash Temporary");
        answer_delete(app.root, IDNO);
        require(snapshot().at("trash").size() == 1, "No in the purge dialog still deleted the note");
        answer_delete(app.root, IDYES);
        eventually([&] { return snapshot().at("trash").empty(); }, "confirmed purge removes trashed note");
        require(snapshot().at("notes").size() == 2 && entry(id, message_id) == original,
                "Purging a note altered another active note");
        back(app.root);
        passed("permanent deletion requires its explicit confirmation and affects only the chosen trashed note");

        click(button(app.root, L"Websites"));
        eventually([&] { return edit(app.root, 37) != nullptr; }, "conversation opens");
        set_text(edit(app.root, 37), L"An unfinished thought");
        entry_menu(app.root, 46, {VK_END, VK_UP, VK_RETURN});
        eventually([&] { return buttons(app.root, L"SAVE EDIT").size() == 1; }, "entry edit opens");
        require(caption(edit(app.root, 37)) == L"https://developer.mozilla.org/", "Edit did not load the selected entry");
        set_text(edit(app.root, 37), L"https://developer.mozilla.org/\r\nHandy docs for my next project.");
        screenshot(app.root, L"note-entry-edit.bmp");
        click(button(app.root, L"SAVE EDIT"));
        eventually([&] { return entry(id, message_id).at("revision") == 1; }, "entry edit commits");
        require(entry(id, message_id).at("images") == original.at("images"), "Edit lost an image reference");
        require(entry(id, message_id).at("created_at") == original.at("created_at") &&
                entry(id, message_id).contains("edited_at"), "Edit changed chronology or omitted edited marker");
        require(caption(edit(app.root, 37)) == L"An unfinished thought", "Saving an edit lost the parked composer");
        passed("native Edit entry preserves attachment references, chronology and the unsent composer");

        entry_menu(app.root, 46, {VK_END, VK_UP, VK_RETURN});
        set_text(edit(app.root, 37), L"My pending correction");
        send({{"op", "note.message.edit"}, {"id", id}, {"message_id", message_id},
              {"expected_revision", 1}, {"text", "Changed elsewhere"}});
        click(button(app.root, L"SAVE EDIT"));
        require(entry(id, message_id).at("text") == "Changed elsewhere", "Stale UI edit overwrote a concurrent change");
        require(caption(edit(app.root, 37)) == L"My pending correction", "Conflict discarded the pending edit");
        click(button(app.root, L"CANCEL"));
        require(caption(edit(app.root, 37)) == L"An unfinished thought", "Cancelling a conflict lost the normal draft");
        passed("concurrent entry changes are preserved and conflicts keep the pending correction");

        entry_menu(app.root, 46, {VK_END, VK_UP, VK_RETURN});
        set_text(edit(app.root, 37), L"An edit kept across restart");
        app.close();
        const auto draft = read_json(app.directory / L"drafts" / (id + ".conversation.json"));
        require(draft.at("text") == "An unfinished thought" &&
                draft.at("edit").at("text") == "An edit kept across restart",
                "Saved edit envelope did not keep both drafts independently");
        app.launch();
        click(button(app.root, L"Notes"));
        click(button(app.root, L"Websites"));
        eventually([&] { return buttons(app.root, L"SAVE EDIT").size() == 1; }, "edit draft restores");
        require(caption(edit(app.root, 37)) == L"An edit kept across restart", "Restart lost pending edit text");
        click(button(app.root, L"CANCEL"));
        require(caption(edit(app.root, 37)) == L"An unfinished thought", "Restart lost the separately parked draft");
        passed("edit drafts and the parked normal composer both survive restart");

        click(button(app.root, L"SEND"));
        eventually([&] { return caption(edit(app.root, 37)).empty(); }, "composer empties after send");
        message(edit(app.root, 37), WM_KEYDOWN, VK_UP);
        eventually([&] { return buttons(app.root, L"SAVE EDIT").size() == 1; }, "Up edits last entry");
        require(caption(edit(app.root, 37)) == L"An unfinished thought", "Up Arrow did not select the newest entry");
        click(button(app.root, L"CANCEL"));
        passed("Up Arrow in an empty composer edits the latest entry");
        back(app.root);

        command(app.root, L"/search-notes NEEDLE CAFÉ");
        eventually([&] { return buttons(app.root, L"Archive").size() == 2; }, "Unicode content search finds two entries");
        screenshot(app.root, L"note-search.bmp");
        const auto query = edit(app.root, 51);
        enter(query);
        eventually([&] { return edit(app.root, 37) != nullptr; }, "search opens conversation");
        entry_menu(app.root, 46, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Needle café entry 0", "Search did not jump to the first old matching entry");
        back(app.root);
        require(edit(app.root, 51) == query && caption(query) == L"NEEDLE CAFÉ", "Back lost the content query");
        message(query, WM_KEYDOWN, VK_DOWN);
        enter(query);
        entry_menu(app.root, 46, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Needle café entry 120", "Second search result opened the wrong message");
        passed("Unicode content search opens exact old entries and Back preserves the query and selection");

        ctrl_find(app.root);
        const auto find = edit(app.root, 52);
        set_text(find, L"needle");
        eventually([&] { return IsWindowEnabled(button(app.root, L"NEXT")); }, "within-note search completes");
        screenshot(app.root, L"note-find-in-note.bmp");
        entry_menu(app.root, 104, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Needle café entry 0", "Ctrl+F did not search older paginated history");
        click(button(app.root, L"NEXT"));
        entry_menu(app.root, 104, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Needle café entry 120", "Next match did not jump to the second entry");
        click(button(app.root, L"PREV"));
        entry_menu(app.root, 104, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Needle café entry 0", "Previous match did not wrap back correctly");
        passed("Ctrl+F, Next and Previous search the full conversation across history pages");

        set_text(edit(app.root, 37), L"Keep this composer while finding things");
        const auto composer = edit(app.root, 37);
        open_settings(app.root);
        send({{"op", "note.append"}, {"id", archive}, {"text", "Needle added while Settings was open"}});
        PostMessageW(button(app.root, L"< BACK"), BM_CLICK, 0, 0);
        eventually([&] { return IsWindowVisible(find); }, "incoming entry and Settings Back reach the UI");
        require(edit(app.root, 52) == find && caption(find) == L"needle" && IsWindowVisible(find),
                "Settings or incoming entry replaced the note find field");
        require(edit(app.root, 37) == composer && caption(composer) == L"Keep this composer while finding things",
                "Settings replaced the unsent composer");
        message(find, WM_KEYDOWN, VK_ESCAPE);
        require(!IsWindowVisible(find) && IsWindowVisible(composer), "Escape did not close only the find bar");
        passed("Settings and incoming entries preserve note search controls and composer drafts");

        back(app.root);
        set_text(edit(app.root, 51), L"Ordinary entry");
        eventually([&] { return buttons(app.root, L"Archive").size() >= 3; }, "large content search loads first page");
        for (int index = 0; index < 100; ++index) message(edit(app.root, 51), WM_KEYDOWN, VK_DOWN);
        screenshot(app.root, L"note-search-second-page.bmp");
        enter(edit(app.root, 51));
        entry_menu(app.root, 46, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Ordinary entry 101", "Next result page did not continue after its first 100 entries");
        back(app.root);
        message(edit(app.root, 51), WM_KEYDOWN, VK_UP);
        enter(edit(app.root, 51));
        entry_menu(app.root, 46, {VK_END, VK_UP, VK_UP, VK_RETURN});
        require(clipboard() == L"Ordinary entry 100", "Previous result page did not retain its last selected entry");
        back(app.root);
        back(app.root);
        passed("content search moves across bounded result pages in both directions");
        send({{"op", "request.create"}, {"chat", "NOTES-TEST"}, {"message", "Test the compact layout"},
              {"timeout_seconds", 300}, {"lease_seconds", 300}});
        click(button(app.root, L"Notifications"));
        eventually([&] { return buttons(app.root, L"YES").size() == 1; }, "test request appears");
        // These actions remove/rebuild their own controls. Dispatch them on
        // the UI queue, then verify the resulting state instead of waiting
        // for a synchronous reply from a destroyed button window.
        PostMessageW(button(app.root, L"YES"), BM_CLICK, 0, 0);
        eventually([&] { return !snapshot().at("lease").is_null(); }, "input strip appears");
        click(button(app.root, L"Notes"));
        open_settings(app.root);
        set_text(edit(app.root, 41), L"360");
        set_text(edit(app.root, 42), L"360");
        PostMessageW(button(app.root, L"APPLY"), BM_CLICK, 0, 0);
        eventually([&] {
            RECT bounds{};
            GetWindowRect(app.root, &bounds);
            return bounds.right - bounds.left == 360 && bounds.bottom - bounds.top == 360;
        }, "minimum window size applies");
        PostMessageW(button(app.root, L"MORE >"), BM_CLICK, 0, 0);
        eventually([&] { return buttons(app.root, L"SET TIMER").size() == 1; }, "More Settings opens");
        start_busy_timer(app.root);
        PostMessageW(button(app.root, L"< BACK"), BM_CLICK, 0, 0);
        eventually([&] { return IsWindowVisible(edit(app.root, 41)); }, "general Settings returns");
        PostMessageW(button(app.root, L"< BACK"), BM_CLICK, 0, 0);
        eventually([&] { return !IsWindowVisible(edit(app.root, 41)); }, "Settings closes");
        send({{"op", "note.add"}, {"title", "Undo sample"}});
        command(app.root, L"/delete-note Undo sample");
        PostMessageW(button(app.root, L"DELETE Undo sample"), BM_CLICK, 0, 0);
        eventually([&] { return snapshot().at("trash").size() == 1; }, "Undo sample enters Trash");
        PostMessageW(button(app.root, L"< BACK"), BM_CLICK, 0, 0);
        eventually([&] { return IsWindowVisible(edit(app.root, 30)); }, "Notes list returns");
        set_text(edit(app.root, 30), L"/");
        for (int count = 0; count < 4; ++count) message(edit(app.root, 30), WM_KEYDOWN, VK_DOWN);
        const auto content = FindWindowExW(app.root, nullptr, L"Knock.Content.v1", nullptr);
        RECT bounds{};
        GetWindowRect(content, &bounds);
        for (HWND control : buttons(app.root)) {
            if (!caption(control).starts_with(L"/")) continue;
            RECT item{};
            GetWindowRect(control, &item);
            require(item.top >= bounds.bottom, "Slash suggestions overlap the Notes content at minimum size");
        }
        screenshot(app.root, L"note-commands-small.bmp");
        passed("slash suggestions fit at minimum size with busy, input and Undo strips present");

        const auto compact_note = send({{"op", "note.add"}, {"title", "Compact layout"}}).at("note");
        send({{"op", "note.append"}, {"id", compact_note.at("id")}, {"text", "Layout attachment"},
              {"images", {"reference.png"}}});
        command(app.root, L"/find-note Compact layout");
        enter(edit(app.root, 51));
        eventually([&] { return IsWindowVisible(edit(app.root, 37)); }, "compact conversation opens");
        entry_menu(app.root, 46, {VK_END, VK_UP, VK_RETURN});
        eventually([&] { return buttons(app.root, L"SAVE EDIT").size() == 1; }, "image entry edit opens");
        require(buttons(app.root, L"x").size() == 1, "Attachment is missing from the compact edit draft");
        ctrl_find(app.root);
        for (HWND control : buttons(app.root)) if (IsWindowVisible(control)) inside_parent(control);
        inside_parent(edit(app.root, 37));
        inside_parent(edit(app.root, 52));
        require(control_bounds(edit(app.root, 37)).bottom <= control_bounds(button(app.root, L"SAVE EDIT")).top,
                "Note composer overlaps its actions with Find, attachment, presence, input and quiet strips");
        screenshot(app.root, L"note-find-attachment-small.bmp");
        passed("note Find and attachment composer fit at minimum size with presence, input and quiet strips");

        send({{"op", "task.add"}, {"title", "Compact task"}, {"body", "Keep the metadata controls separate."}});
        click(button(app.root, L"Task List"));
        eventually([&] { return buttons(app.root, L"Compact task").size() == 1; }, "compact task appears");
        PostMessageW(button(app.root, L"Compact task"), BM_CLICK, 0, 0);
        eventually([&] { return IsWindowVisible(edit(app.root, 33)) && IsWindowVisible(edit(app.root, 34)); },
                   "compact task metadata opens");
        for (int id : {31, 32, 33, 34}) inside_parent(edit(app.root, id));
        for (HWND control : buttons(app.root)) if (IsWindowVisible(control)) inside_parent(control);
        require(control_bounds(edit(app.root, 32)).bottom <= control_bounds(edit(app.root, 33)).top &&
                control_bounds(edit(app.root, 32)).bottom <= control_bounds(edit(app.root, 34)).top,
                "Task description overlaps metadata with presence, input and quiet strips");
        screenshot(app.root, L"task-details-small.bmp");
        passed("task metadata fits at minimum size with presence, input and quiet strips");
        app.close();
        std::filesystem::remove_all(app.directory);
        std::cout << "All notes UI checks passed. No global input was generated.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL notes UI: " << error.what() << '\n';
        return 1;
    }
}
