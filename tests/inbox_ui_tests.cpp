#include "knock/platform.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using knock::Json;
using namespace std::chrono_literals;

namespace {
constexpr wchar_t tray_class[] = L"Knock.Tray.v1";
void require(bool value, const std::string& text) { if (!value) throw std::runtime_error(text); }

template<class Predicate>
void eventually(Predicate predicate, const std::string& text, std::chrono::milliseconds timeout = 5000ms) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    do {
        if (predicate()) return;
        std::this_thread::sleep_for(20ms);
    } while (std::chrono::steady_clock::now() < until);
    throw std::runtime_error("Timed out: " + text);
}
LRESULT message(HWND window, UINT id, WPARAM wparam = 0, LPARAM lparam = 0) {
    require(window && IsWindow(window), "Native control is missing");
    DWORD_PTR result = 0;
    require(SendMessageTimeoutW(window, id, wparam, lparam, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                               3000, &result) != 0, "Native control timed out");
    return static_cast<LRESULT>(result);
}
std::wstring text(HWND window) {
    const auto length = static_cast<std::size_t>(message(window, WM_GETTEXTLENGTH));
    std::wstring value(length + 1, L'\0');
    value.resize(static_cast<std::size_t>(message(window, WM_GETTEXT, value.size(), reinterpret_cast<LPARAM>(value.data()))));
    return value;
}
void set_text(HWND window, const std::wstring& value) {
    require(message(window, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(value.c_str())) != 0, "Cannot fill native edit");
    if (GetWindowLongPtrW(window, GWL_STYLE) & ES_MULTILINE) {
        message(window, EM_SETSEL, value.size(), value.size());
        message(window, WM_CHAR, L' ');
        message(window, WM_CHAR, VK_BACK);
    }
}
struct Query { const wchar_t* type; std::wstring caption; int id; std::vector<HWND> matches; };
BOOL CALLBACK collect(HWND window, LPARAM data) {
    auto& query = *reinterpret_cast<Query*>(data);
    wchar_t type[64]{};
    GetClassNameW(window, type, 64);
    if (_wcsicmp(type, query.type) != 0 || (query.id && GetDlgCtrlID(window) != query.id)) return TRUE;
    if (!query.caption.empty() && text(window) != query.caption) return TRUE;
    if (IsWindowVisible(window)) query.matches.push_back(window);
    return TRUE;
}
std::vector<HWND> find(HWND root, const wchar_t* type, const std::wstring& caption = {}, int id = 0) {
    Query query{type, caption, id, {}};
    EnumChildWindows(root, collect, reinterpret_cast<LPARAM>(&query));
    return query.matches;
}
HWND button(HWND root, const std::wstring& caption) {
    const auto matches = find(root, L"BUTTON", caption);
    require(matches.size() == 1, "Expected one button: " + knock::narrow(caption) + "; got " + std::to_string(matches.size()));
    return matches.front();
}
HWND edit(HWND root, int id) {
    const auto matches = find(root, L"EDIT", {}, id);
    return matches.empty() ? nullptr : matches.front();
}
void click(HWND control) {
    require(IsWindowEnabled(control), "Button is disabled");
    message(control, BM_CLICK);
}
void enter(HWND control) { require(PostMessageW(control, WM_KEYDOWN, VK_RETURN, 0), "Cannot post Enter"); }
GUITHREADINFO gui(HWND root) {
    GUITHREADINFO result{};
    result.cbSize = sizeof(result);
    require(GetGUIThreadInfo(GetWindowThreadProcessId(root, nullptr), &result), "Cannot inspect GUI thread");
    return result;
}
void menu_action(HWND root, const std::function<void()>& open, std::initializer_list<UINT> keys) {
    open();
    HWND owner = nullptr;
    try {
        eventually([&] {
            const auto state = gui(root);
            owner = state.hwndMenuOwner;
            return (state.flags & GUI_INMENUMODE) && owner &&
                   GetWindowThreadProcessId(owner, nullptr) == GetWindowThreadProcessId(root, nullptr);
        }, "native menu opens");
        for (auto key : keys) require(PostMessageW(owner, WM_KEYDOWN, key, 1), "Cannot navigate test menu");
        eventually([&] { return !(gui(root).flags & GUI_INMENUMODE); }, "native menu closes");
        message(root, WM_NULL);
    } catch (...) { PostMessageW(root, WM_CANCELMODE, 0, 0); throw; }
}
void button_menu(HWND root, const std::wstring& caption, std::initializer_list<UINT> keys) {
    const auto target = button(root, caption);
    menu_action(root, [&] { require(PostMessageW(target, BM_CLICK, 0, 0), "Cannot open button menu"); }, keys);
}
void open_settings(HWND root) {
    menu_action(root, [&] {
        require(PostMessageW(root, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(root), -1), "Cannot open app menu");
    }, {VK_HOME, VK_RETURN});
    eventually([&] { return edit(root, 41) && edit(root, 42); }, "Settings opens");
}
void back(HWND root) { click(button(root, L"< BACK")); }
Json send(const Json& request) {
    auto result = knock::send_command(request, false);
    require(result.value("ok", false), "IPC failed: " + result.dump());
    return result;
}
Json state() { return send({{"op", "snapshot"}}).at("state"); }
Json preferences() { return state().at("preferences"); }
Json request(const char* chat, int seconds = 300) {
    return send({{"op", "request.create"}, {"chat", chat}, {"message", "May I test the interface?"},
        {"timeout_seconds", seconds}, {"lease_seconds", 180}}).at("request");
}
Json result(const Json& request) {
    return send({{"op", "request.result"}, {"id", request.at("id")}}).at("request");
}
Json notice(const char* chat) {
    return send({{"op", "notify"}, {"chat", chat}, {"message", "Build finished"}}).at("job");
}
RECT bounds(HWND window) { RECT value{}; require(GetWindowRect(window, &value), "Cannot read bounds"); return value; }
void inside_parent(HWND control) {
    const auto child = bounds(control);
    const auto parent = bounds(GetParent(control));
    require(child.left >= parent.left && child.top >= parent.top && child.right <= parent.right &&
            child.bottom <= parent.bottom && child.right > child.left && child.bottom > child.top,
            "Control is clipped by its parent: " + knock::narrow(text(control)));
}
void settings_bounds(HWND root) {
    for (const auto* type : {L"BUTTON", L"EDIT"})
        for (HWND control : find(root, type)) inside_parent(control);
}
void capture(HWND root, const std::filesystem::path& path) {
    require(RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN),
            "Cannot redraw test window before capture");
    const auto box = bounds(root);
    const int width = box.right - box.left, height = box.bottom - box.top;
    HDC dc = GetWindowDC(root);
    require(dc != nullptr, "Cannot capture test window");
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    require(memory && bitmap, "Cannot allocate test screenshot");
    auto old = SelectObject(memory, bitmap);
    require(BitBlt(memory, 0, 0, width, height, dc, 0, 0, SRCCOPY), "Cannot copy test screenshot");
    SelectObject(memory, old);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    require(GetDIBits(memory, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS), "Cannot read test screenshot");
    BITMAPFILEHEADER header{};
    header.bfType = 0x4D42;
    header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header), sizeof(header));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    require(static_cast<bool>(output), "Cannot save test screenshot");
    DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(root, dc);
}
struct Application {
    std::filesystem::path directory;
    std::filesystem::path executable = knock::executable_path().parent_path() / L"KnockTray.exe";
    HANDLE process = nullptr;
    DWORD pid = 0;
    HWND root = nullptr;
    Application() {
        wchar_t temp[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temp), "Cannot find temp folder");
        directory = std::filesystem::path(temp) / (L"knock-inbox-ui-" + knock::widen(knock::make_id()));
        require(SetEnvironmentVariableW(L"KNOCK_DATA_DIR", directory.c_str()), "Cannot isolate application data");
    }
    ~Application() { try { close(); } catch (...) {} }
    void launch() {
        auto line = knock::command_line({executable.wstring(), L"--show"});
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        require(CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, executable.parent_path().c_str(), &startup, &child), "Cannot launch private test app");
        CloseHandle(child.hThread); process = child.hProcess; pid = child.dwProcessId;
        eventually([&] {
            root = FindWindowW(tray_class, nullptr);
            if (!root) return false;
            DWORD actual = 0; GetWindowThreadProcessId(root, &actual);
            require(actual == pid, "Refusing to control another app's window");
            return IsWindowVisible(root) && find(root, L"BUTTON", L"Notifications").size() == 1;
        }, "application startup");
        eventually([&] { try { return knock::send_command({{"op", "status"}}, false).value("ok", false); } catch (...) { return false; } }, "IPC startup");
        // Pin this test popup through the app's own hotkey message. No global
        // input is generated, even if this test runs on a normal desktop.
        message(root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        message(root, WM_HOTKEY, 0x4B10, MAKELPARAM(MOD_CONTROL | MOD_ALT, 'K'));
        eventually([&] { return IsWindowVisible(root); }, "hotkey-pinned popup");
    }
    void close() {
        if (!process) return;
        if (root && IsWindow(root)) {
            DWORD actual = 0; GetWindowThreadProcessId(root, &actual);
            require(actual == pid, "Refusing to close another process");
            require(message(root, WM_QUERYENDSESSION), "Application refused normal test shutdown");
            require(PostMessageW(root, WM_ENDSESSION, TRUE, 0), "Cannot end test session");
        }
        require(WaitForSingleObject(process, 5000) == WAIT_OBJECT_0, "Test app did not exit");
        CloseHandle(process); process = nullptr; root = nullptr;
    }
};
void passed(const char* label) { std::cout << "PASS " << label << '\n' << std::flush; }
}

int main() {
    if (FindWindowW(tray_class, nullptr)) { std::cout << "SKIP: close KnockTray before inbox tests.\n"; return 77; }
    try {
        Application app;
        app.launch();
        const auto defaults = preferences();
        require(defaults.at("saved_replies").size() == 3, "Fresh saved reply templates missing");
        const auto blocked = knock::send_command({{"op", "preferences.update"}, {"busy_until", knock::now_ms() + 60000}}, false);
        require(!blocked.value("ok", true) && preferences() == defaults, "IPC changed GUI-only preferences");
        passed("only the interface can change saved replies and busy preferences");

        open_settings(app.root);
        click(button(app.root, L"MORE >"));
        button_menu(app.root, L"SET TIMER", {VK_HOME, VK_RETURN});
        auto deadline = preferences().at("busy_until").get<std::int64_t>();
        require(deadline > knock::now_ms() + 14 * 60000 && deadline <= knock::now_ms() + 15 * 60000,
                "15-minute busy menu used the wrong deadline");
        back(app.root); back(app.root);
        require(find(app.root, L"BUTTON", L"END").size() == 1, "Busy strip is missing");
        const auto first = request("AUDIO-1");
        eventually([&] { return find(app.root, L"BUTTON", L"CUSTOM").size() == 1; }, "request arrives during busy mode");
        click(button(app.root, L"CUSTOM"));
        button_menu(app.root, L"SAVED REPLIES", {VK_HOME, VK_RETURN});
        const auto reply = edit(app.root, 35);
        require(reply && text(reply) == knock::widen(defaults["saved_replies"][0].get<std::string>()), "Preset did not fill composer");
        require(result(first).at("status") == "pending" && state().at("lease").is_null(), "Selecting a preset granted permission");
        const std::wstring custom = L"I'm busy — finish the headless tests first. Café 日本語.";
        set_text(reply, custom);
        notice("BUILD-1");
        eventually([&] { return find(app.root, L"BUTTON", L"@BUILD-1").size() == 1; }, "incoming build update");
        require(edit(app.root, 35) == reply && text(reply) == custom && gui(app.root).hwndFocus == reply,
                "Incoming update replaced the custom reply or its focus");
        capture(app.root, app.directory / L"saved-reply.bmp");
        click(button(app.root, L"SEND"));
        eventually([&] { return result(first).at("status") == "custom"; }, "custom reply submitted");
        require(result(first).at("response") == knock::narrow(custom) && state().at("lease").is_null(), "Custom text became an approval");
        passed("saved reply selection stays pending; edited Unicode reply sends as custom");

        const auto pending = request("UI-3");
        notice("BUILD-2");
        eventually([&] { return find(app.root, L"BUTTON", L"@BUILD-2").size() == 1; }, "newer completed notice");
        require(bounds(button(app.root, L"@UI-3")).top < bounds(button(app.root, L"@BUILD-2")).top,
                "A newer completion pushed a pending question below it");
        click(button(app.root, L"@AUDIO-1"));
        require(find(app.root, L"BUTTON", L"@AUDIO-1").size() == 1 && find(app.root, L"BUTTON", L"@UI-3").empty() &&
                find(app.root, L"BUTTON", L"@BUILD-2").empty(), "Chat label did not filter the inbox");
        click(button(app.root, L"ALL CHATS"));
        require(find(app.root, L"BUTTON", L"@UI-3").size() == 1 && find(app.root, L"BUTTON", L"@BUILD-2").size() == 1,
                "All chats did not restore the inbox");
        button_menu(app.root, L"CHATS", {VK_HOME, VK_DOWN, VK_RETURN});
        require(find(app.root, L"BUTTON", L"@AUDIO-1").size() == 1 && find(app.root, L"BUTTON", L"@UI-3").empty(),
                "Chat selector did not select the first alphabetical chat");
        click(button(app.root, L"ALL CHATS"));
        require(result(pending).at("status") == "pending" && state().at("lease").is_null() &&
                preferences().at("busy_until") == deadline, "Busy mode changed input permission");
        const auto expiring = request("EXPIRES", 1);
        eventually([&] { return result(expiring).at("status") == "expired"; }, "request expiry while busy");
        require(state().at("lease").is_null(), "Busy expiry granted input");
        passed("chat filters, pending-first order, and ordinary request expiry during busy mode");

        open_settings(app.root);
        click(button(app.root, L"MORE >"));
        const auto retention = edit(app.root, 43);
        set_text(retention, L"0"); click(button(app.root, L"APPLY"));
        require(preferences().at("trash_retention_days") == 30, "Invalid retention was saved");
        set_text(retention, L"7"); enter(retention);
        eventually([&] { return preferences().at("trash_retention_days") == 7; }, "retention saves with Enter");
        click(button(app.root, L"MANAGE"));
        click(button(app.root, knock::widen(defaults["saved_replies"][0].get<std::string>())));
        const auto saved_editor = edit(app.root, 44);
        const std::wstring saved = L"Later, please — continue with headless tests. 日本語";
        set_text(saved_editor, saved);
        notice("SETTINGS-UPDATE");
        // This posted character follows the incoming store event on the same
        // GUI queue. Its appearance proves the editor survived that event.
        require(PostMessageW(saved_editor, WM_CHAR, L'x', 0), "Cannot continue editing after the incoming result");
        eventually([&] { return IsWindow(saved_editor) && text(saved_editor) == saved + L"x"; },
                   "incoming event finishes before the next edit");
        message(saved_editor, WM_CHAR, VK_BACK);
        require(edit(app.root, 44) == saved_editor && text(saved_editor) == saved && gui(app.root).hwndFocus == saved_editor,
                "An incoming result replaced the saved reply editor or focus");
        click(button(app.root, L"SAVE"));
        require(preferences()["saved_replies"][0] == knock::narrow(saved), "Saved reply edit was not persisted");
        click(button(app.root, L"ADD"));
        set_text(edit(app.root, 44), L" \n "); click(button(app.root, L"SAVE"));
        require(edit(app.root, 44) && preferences()["saved_replies"].size() == 3, "Blank reply was accepted");
        const std::wstring extra = L"Ask again after the headless tests finish.";
        set_text(edit(app.root, 44), extra); click(button(app.root, L"SAVE"));
        require(preferences()["saved_replies"].size() == 4, "New reply was not saved");
        click(button(app.root, extra)); click(button(app.root, L"DELETE"));
        require(preferences()["saved_replies"].size() == 3 && preferences()["saved_replies"][0] == knock::narrow(saved),
                "Deleting one reply changed the others");
        click(button(app.root, saved));
        set_text(edit(app.root, 44), L"Unsaved replacement"); click(button(app.root, L"CANCEL"));
        require(preferences()["saved_replies"][0] == knock::narrow(saved), "Cancel saved an edited preset");
        passed("saved reply add/edit/delete/cancel, blank rejection, and retention validation");

        back(app.root); back(app.root);
        set_text(edit(app.root, 41), L"360"); set_text(edit(app.root, 42), L"360");
        click(button(app.root, L"APPLY"));
        settings_bounds(app.root);
        back(app.root);
        const auto lease_check = request("LEASE-CHECK");
        eventually([&] { return find(app.root, L"BUTTON", L"@LEASE-CHECK").size() == 1; }, "lease layout fixture arrives");
        click(button(app.root, L"@LEASE-CHECK"));
        click(button(app.root, L"YES"));
        eventually([&] { return result(lease_check).at("status") == "approved"; }, "UI grants private test lease");
        open_settings(app.root);
        settings_bounds(app.root);
        click(button(app.root, L"MORE >")); settings_bounds(app.root);
        capture(app.root, app.directory / L"preferences-minimum.bmp");
        click(button(app.root, L"MANAGE")); settings_bounds(app.root);
        click(button(app.root, saved)); settings_bounds(app.root);
        capture(app.root, app.directory / L"reply-editor-minimum.bmp");
        click(button(app.root, L"CANCEL")); back(app.root); back(app.root); back(app.root);
        click(button(app.root, L"RELEASE"));
        eventually([&] { return state().at("lease").is_null(); }, "private test lease is released");
        click(button(app.root, L"ALL CHATS"));
        click(button(app.root, L"END"));
        require(preferences().at("busy_until") == 0 && find(app.root, L"BUTTON", L"END").empty(), "Ending busy mode did not clear it");
        require(result(pending).at("status") == "pending", "Ending busy mode answered a request");
        passed("360-pixel Settings pages fit with an active lease; busy END preserves requests");

        open_settings(app.root);
        click(button(app.root, L"MORE >"));
        button_menu(app.root, L"SET TIMER", {VK_HOME, VK_DOWN, VK_RETURN});
        back(app.root); back(app.root);
        deadline = preferences().at("busy_until").get<std::int64_t>();
        require(deadline > knock::now_ms() + 29 * 60000 && deadline <= knock::now_ms() + 30 * 60000,
                "Settings did not start a 30-minute timer");
        app.close(); app.launch();
        require(preferences()["saved_replies"][0] == knock::narrow(saved) && preferences().at("trash_retention_days") == 7 &&
                preferences().at("busy_until") == deadline, "Preferences did not survive restart");
        require(find(app.root, L"BUTTON", L"END").size() == 1, "Busy mode did not resume after restart");
        click(button(app.root, L"END"));
        passed("busy timer and all preferences survive restart");

        app.close();
        {
            // A short private fixture exercises the real timer expiry without
            // waiting fifteen minutes or changing the system clock.
            knock::Store fixture(app.directory);
            fixture.load();
            const auto applied = fixture.handle({{"op", "preferences.update"}, {"busy_until", knock::now_ms() + 1500}}, true);
            require(applied.value("ok", false), "Could not prepare expiry fixture");
        }
        const auto saved_state = app.directory / L"state.json";
        struct ReadLock {
            HANDLE handle;
            ~ReadLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } lock{CreateFileW(saved_state.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr)};
        require(lock.handle != INVALID_HANDLE_VALUE, "Cannot lock the private state file for the retry check");
        app.launch();
        const auto expiring_deadline = preferences().at("busy_until").get<std::int64_t>();
        eventually([&] { return knock::now_ms() > expiring_deadline + 1200; }, "busy deadline passes while file replacement is blocked");
        require(preferences().at("busy_until") == expiring_deadline && expiring_deadline != 0,
                "Failed completion save discarded the busy deadline");
        CloseHandle(lock.handle); lock.handle = INVALID_HANDLE_VALUE;
        eventually([&] { return preferences().at("busy_until") == 0; },
                   "busy completion retries after the state file is unlocked", 8000ms);
        require(result(pending).at("status") == "pending" && state().at("lease").is_null(), "Timer expiry answered the pending question");
        app.close(); app.launch();
        require(preferences().at("busy_until") == 0 && find(app.root, L"BUTTON", L"END").empty(), "Completed busy timer reactivated on restart");
        passed("busy completion survives a failed save, retries once unlocked, and never grants permission");
        std::cout << "Test data and screenshots: " << knock::narrow(app.directory.wstring()) << '\n';
        app.close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
