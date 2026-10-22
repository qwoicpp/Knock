#include "knock/hotkey.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace knock {
namespace {
using Json = nlohmann::json;
constexpr int first_hotkey_id = 0x4B10;
constexpr int second_hotkey_id = 0x4B11;
constexpr UINT modifier_mask = MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN;
constexpr COLORREF background = RGB(9, 9, 9);
constexpr COLORREF foreground = RGB(225, 225, 217);
constexpr COLORREF muted = RGB(151, 151, 145);
constexpr COLORREF border = RGB(71, 71, 71);
constexpr COLORREF orange = RGB(224, 129, 88);

bool modifier_key(UINT key) {
    return key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU ||
        key == VK_LSHIFT || key == VK_RSHIFT || key == VK_LCONTROL ||
        key == VK_RCONTROL || key == VK_LMENU || key == VK_RMENU ||
        key == VK_LWIN || key == VK_RWIN;
}

UINT held_modifiers() {
    UINT result = 0;
    if (GetKeyState(VK_CONTROL) < 0) result |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) < 0) result |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) < 0) result |= MOD_SHIFT;
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) result |= MOD_WIN;
    return result;
}

Json read_settings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return Json::object();
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 64 * 1024)
        throw std::runtime_error("invalid settings file");
    std::ifstream input(path, std::ios::binary);
    auto settings = Json::parse(input);
    if (!settings.is_object()) throw std::runtime_error("invalid settings object");
    return settings;
}

HotkeyBinding parse_binding(const Json& settings) {
    if (!settings.contains("shortcut")) return {};
    const auto& shortcut = settings.at("shortcut");
    if (!shortcut.is_object() || !shortcut.contains("enabled") || !shortcut.at("enabled").is_boolean() ||
        !shortcut.contains("modifiers") || !shortcut.at("modifiers").is_number_unsigned() ||
        !shortcut.contains("key") || !shortcut.at("key").is_number_unsigned())
        throw std::runtime_error("invalid shortcut settings");
    const auto modifiers = shortcut.at("modifiers").get<std::uint64_t>();
    const auto key = shortcut.at("key").get<std::uint64_t>();
    if (modifiers > modifier_mask || key > 0xFE) throw std::runtime_error("invalid shortcut values");
    HotkeyBinding binding{static_cast<UINT>(modifiers), static_cast<UINT>(key), shortcut.at("enabled").get<bool>()};
    if (!valid_hotkey(binding)) throw std::runtime_error("invalid shortcut binding");
    return binding;
}

bool atomic_settings(const std::filesystem::path& directory, const std::string& contents) {
    std::filesystem::create_directories(directory);
    static unsigned int sequence = 0;
    const auto temporary = directory / (L".settings-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence) + L".tmp");
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr) != 0 &&
        written == contents.size() && FlushFileBuffers(file) != 0;
    if (!CloseHandle(file)) ok = false;
    if (ok) ok = MoveFileExW(temporary.c_str(), (directory / L"settings.json").c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary.c_str());
    return ok;
}

void fill(HDC dc, RECT bounds, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &bounds, brush);
    DeleteObject(brush);
}

struct ShortcutDialog {
    HotkeyManager& manager;
    HotkeyBinding draft;
    HWND window = nullptr;
    HWND capture = nullptr;
    HWND save_button = nullptr;
    HFONT font = nullptr;
    HBRUSH brush = CreateSolidBrush(background);
    UINT dpi = 96;
    bool saved = false;
    std::wstring status;

    explicit ShortcutDialog(HotkeyManager& value) : manager(value), draft(value.binding()) {}
    ~ShortcutDialog() {
        if (window && IsWindow(window)) DestroyWindow(window);
        if (font) DeleteObject(font);
        DeleteObject(brush);
    }
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    RECT rect(int x, int y, int width, int height) const {
        return {px(x), px(y), px(x + width), px(y + height)};
    }
    void text(HDC dc, const std::wstring& value, RECT bounds, COLORREF color = foreground,
              UINT flags = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX) const {
        const auto old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &bounds, flags);
        SelectObject(dc, old);
    }
    HWND button(int id, const wchar_t* caption, RECT bounds) {
        HWND control = CreateWindowExW(0, L"BUTTON", caption, WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
            bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top,
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        ShowWindow(control, SW_SHOW);
        return control;
    }
    void update() {
        const auto label = hotkey_label(draft);
        SetWindowTextW(capture, label.c_str());
        EnableWindow(save_button, valid_hotkey(draft));
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
    void key(UINT value) {
        if (modifier_key(value)) return;
        const UINT modifiers = held_modifiers();
        if (value == VK_ESCAPE && !modifiers) { DestroyWindow(window); return; }
        if (value == VK_TAB && !(modifiers & ~MOD_SHIFT)) {
            SetFocus(GetNextDlgTabItem(window, capture, (modifiers & MOD_SHIFT) != 0));
            return;
        }
        draft = {modifiers, value, true};
        status.clear();
        valid_hotkey(draft, &status);
        update();
    }
    void draw_button(const DRAWITEMSTRUCT& item) {
        RECT bounds = item.rcItem;
        fill(item.hDC, bounds, item.itemState & ODS_SELECTED ? RGB(31, 31, 29) : background);
        auto frame = CreateSolidBrush((item.itemState & ODS_FOCUS) ? orange : border);
        FrameRect(item.hDC, &bounds, frame);
        DeleteObject(frame);
        wchar_t caption[128]{};
        GetWindowTextW(item.hwndItem, caption, 128);
        InflateRect(&bounds, -px(5), 0);
        text(item.hDC, caption, bounds, (item.itemState & ODS_DISABLED) ? muted : foreground,
             DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    LRESULT message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT bounds{}; GetClientRect(window, &bounds);
            fill(dc, bounds, background);
            auto frame = CreateSolidBrush(border);
            FrameRect(dc, &bounds, frame);
            DeleteObject(frame);
            text(dc, L"KEYBOARD SHORTCUT", rect(16, 16, 308, 20));
            text(dc, L"Click the box and press your shortcut.", rect(16, 46, 308, 20), muted);
            text(dc, status.empty() ? L"Opens or hides Knock. Ctrl, Alt, Shift and Win are supported." : status,
                 rect(16, 113, 308, 52), status.empty() ? muted : orange);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(wparam), background);
            SetTextColor(reinterpret_cast<HDC>(wparam), foreground);
            return reinterpret_cast<LRESULT>(brush);
        case WM_DRAWITEM: draw_button(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam)); return TRUE;
        case WM_COMMAND:
            if (HIWORD(wparam) != BN_CLICKED) return 0;
            if (LOWORD(wparam) == 101) SetFocus(capture);
            if (LOWORD(wparam) == 102) {
                if (manager.apply(draft)) { saved = true; DestroyWindow(window); }
                else { status = manager.error(); update(); }
            }
            if (LOWORD(wparam) == 103) DestroyWindow(window);
            if (LOWORD(wparam) == 104) {
                draft = {0, 0, false}; status.clear(); update(); SetFocus(save_button);
            }
            return 0;
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) { DestroyWindow(window); return 0; }
            break;
        case WM_CLOSE: DestroyWindow(window); return 0;
        case WM_NCDESTROY: window = nullptr; return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* dialog = reinterpret_cast<ShortcutDialog*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            dialog = static_cast<ShortcutDialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            dialog->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
        }
        return dialog ? dialog->message(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
    }
};
}

bool valid_hotkey(const HotkeyBinding& binding, std::wstring* reason) {
    const auto fail = [&](const wchar_t* text) { if (reason) *reason = text; return false; };
    if (binding.modifiers & ~modifier_mask) return fail(L"Unsupported modifier.");
    if (!binding.enabled) return binding.key == 0 && binding.modifiers == 0 ? true : fail(L"Invalid disabled shortcut.");
    if (binding.key < VK_BACK || binding.key > 0xFE || modifier_key(binding.key) ||
        binding.key == VK_PROCESSKEY || binding.key == VK_PACKET)
        return fail(L"Choose a key as well as your modifiers.");
    if (binding.key == VK_F12) return fail(L"Windows reserves F12 for debugging. Choose another key.");
    if (!binding.modifiers && !(binding.key >= VK_F1 && binding.key <= VK_F24))
        return fail(L"Add Ctrl, Alt, Shift or Win, or choose a function key.");
    return true;
}

std::wstring hotkey_label(const HotkeyBinding& binding) {
    if (!binding.enabled) return L"Disabled";
    std::wstring result;
    if (binding.modifiers & MOD_CONTROL) result += L"Ctrl + ";
    if (binding.modifiers & MOD_ALT) result += L"Alt + ";
    if (binding.modifiers & MOD_SHIFT) result += L"Shift + ";
    if (binding.modifiers & MOD_WIN) result += L"Win + ";
    if ((binding.key >= 'A' && binding.key <= 'Z') || (binding.key >= '0' && binding.key <= '9'))
        result += static_cast<wchar_t>(binding.key);
    else if (binding.key >= VK_F1 && binding.key <= VK_F24)
        result += L"F" + std::to_wstring(binding.key - VK_F1 + 1);
    else {
        UINT scan = MapVirtualKeyW(binding.key, MAPVK_VK_TO_VSC);
        if (binding.key == VK_LEFT || binding.key == VK_RIGHT || binding.key == VK_UP || binding.key == VK_DOWN ||
            binding.key == VK_PRIOR || binding.key == VK_NEXT || binding.key == VK_HOME || binding.key == VK_END ||
            binding.key == VK_INSERT || binding.key == VK_DELETE || binding.key == VK_DIVIDE || binding.key == VK_NUMLOCK)
            scan |= 0x100;
        wchar_t name[64]{};
        if (GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64) > 0) result += name;
        else result += L"Key " + std::to_wstring(binding.key);
    }
    return result;
}

HotkeyManager::HotkeyManager(std::filesystem::path directory) : directory_(std::move(directory)) {}
HotkeyManager::~HotkeyManager() { stop(); }

bool HotkeyManager::initialize(HWND owner) {
    stop();
    error_.clear();
    owner_ = owner;
    if (!IsWindow(owner_)) { error_ = L"The shortcut needs an application window."; return false; }
    try { binding_ = parse_binding(read_settings(directory_ / L"settings.json")); }
    catch (const std::exception&) {
        error_ = L"Shortcut settings could not be read. The existing settings file was kept.";
        return false;
    }
    if (!binding_.enabled) return true;
    if (!RegisterHotKey(owner_, first_hotkey_id, binding_.modifiers | MOD_NOREPEAT, binding_.key)) {
        error_ = L"Shortcut is already in use or reserved by Windows. Choose another shortcut from the tray menu.";
        return false;
    }
    active_id_ = first_hotkey_id;
    return true;
}

bool HotkeyManager::save(const HotkeyBinding& binding) {
    try {
        auto settings = read_settings(directory_ / L"settings.json");
        settings["shortcut"] = {{"enabled", binding.enabled}, {"modifiers", binding.modifiers}, {"key", binding.key}};
        const auto contents = settings.dump(2) + "\n";
        if (contents.size() <= 64 * 1024 && atomic_settings(directory_, contents)) return true;
    } catch (const std::exception&) {}
    error_ = L"Could not save the shortcut. The previous shortcut and settings were kept.";
    return false;
}

bool HotkeyManager::apply(HotkeyBinding next) {
    error_.clear();
    if (!valid_hotkey(next, &error_)) return false;
    if (!IsWindow(owner_)) { error_ = L"The shortcut needs an application window."; return false; }
    int next_id = 0;
    const bool unchanged = active_id_ && next == binding_;
    if (next.enabled && !unchanged) {
        next_id = active_id_ == first_hotkey_id ? second_hotkey_id : first_hotkey_id;
        if (!RegisterHotKey(owner_, next_id, next.modifiers | MOD_NOREPEAT, next.key)) {
            error_ = L"Shortcut is already in use or reserved by Windows. Choose another combination.";
            return false;
        }
    }
    if (!save(next)) {
        if (next_id) UnregisterHotKey(owner_, next_id);
        return false;
    }
    if (unchanged) return true;
    if (active_id_) UnregisterHotKey(owner_, active_id_);
    active_id_ = next_id;
    binding_ = next;
    return true;
}

void HotkeyManager::stop() noexcept {
    if (active_id_ && owner_) UnregisterHotKey(owner_, active_id_);
    active_id_ = 0;
    owner_ = nullptr;
}

bool HotkeyManager::matches(WPARAM id) const noexcept {
    return active_id_ != 0 && id == static_cast<WPARAM>(active_id_);
}

std::wstring HotkeyManager::label() const {
    auto result = hotkey_label(binding_);
    if (binding_.enabled && !active_id_) result += L" (unavailable)";
    return result;
}

bool HotkeyManager::configure(HWND owner) {
    error_.clear();
    if (!IsWindow(owner)) { error_ = L"The shortcut needs an application window."; return false; }
    ShortcutDialog dialog(*this);
    dialog.dpi = GetDpiForWindow(owner);
    if (!dialog.dpi) dialog.dpi = 96;
    dialog.font = CreateFontW(-dialog.px(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");
    WNDCLASSW window_class{};
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"Knock.Shortcut.v1";
    window_class.lpfnWndProc = ShortcutDialog::proc;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error_ = L"Could not open shortcut settings."; return false;
    }
    RECT parent{}; GetWindowRect(owner, &parent);
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
    const int width = dialog.px(340), height = dialog.px(220);
    const int x = std::clamp(static_cast<int>((parent.left + parent.right - width) / 2),
        static_cast<int>(monitor.rcWork.left), std::max(static_cast<int>(monitor.rcWork.left), static_cast<int>(monitor.rcWork.right) - width));
    const int y = std::clamp(static_cast<int>((parent.top + parent.bottom - height) / 2),
        static_cast<int>(monitor.rcWork.top), std::max(static_cast<int>(monitor.rcWork.top), static_cast<int>(monitor.rcWork.bottom) - height));
    if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
        window_class.lpszClassName, L"Knock shortcut", WS_POPUP | WS_CLIPCHILDREN,
        x, y, width, height, owner, nullptr, window_class.hInstance, &dialog)) {
        error_ = L"Could not open shortcut settings."; return false;
    }
    dialog.capture = dialog.button(101, hotkey_label(dialog.draft).c_str(), dialog.rect(16, 76, 308, 29));
    dialog.save_button = dialog.button(102, L"SAVE", dialog.rect(228, 180, 96, 24));
    dialog.button(103, L"CANCEL", dialog.rect(122, 180, 96, 24));
    dialog.button(104, L"DISABLE", dialog.rect(16, 180, 96, 24));
    const bool was_enabled = IsWindowEnabled(owner) != 0;
    if (was_enabled) EnableWindow(owner, FALSE);
    ShowWindow(dialog.window, SW_SHOWNORMAL);
    SetForegroundWindow(dialog.window);
    SetFocus(dialog.capture);
    MSG message{};
    while (dialog.window && IsWindow(dialog.window)) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
            break;
        }
        if (message.hwnd == dialog.capture && (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)) {
            dialog.key(static_cast<UINT>(message.wParam)); continue;
        }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
            (message.hwnd == dialog.window || IsChild(dialog.window, message.hwnd))) {
            DestroyWindow(dialog.window); continue;
        }
        if (dialog.window && IsDialogMessageW(dialog.window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (IsWindow(owner) && was_enabled) {
        EnableWindow(owner, TRUE);
        if (IsWindowVisible(owner)) SetForegroundWindow(owner);
    }
    return dialog.saved;
}
}
