#include "knock/hotkey.hpp"

#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Json = nlohmann::json;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

std::string read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
void write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary);
    stream << contents;
    require(static_cast<bool>(stream), "Could not prepare settings fixture");
}

struct Context {
    HWND first = nullptr, second = nullptr;
    std::filesystem::path directory;
    Context() {
        WNDCLASSW type{};
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"Knock.Hotkey.Test";
        type.lpfnWndProc = DefWindowProcW;
        require(RegisterClassW(&type) != 0, "Could not register test window class");
        first = CreateWindowExW(0, type.lpszClassName, L"Hotkey test", WS_OVERLAPPED,
            0, 0, 400, 300, nullptr, nullptr, type.hInstance, nullptr);
        second = CreateWindowExW(0, type.lpszClassName, L"Hotkey conflict test", WS_OVERLAPPED,
            0, 0, 400, 300, nullptr, nullptr, type.hInstance, nullptr);
        require(first && second, "Could not create isolated test windows");
        wchar_t temporary[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temporary) > 0, "No temporary directory");
        directory = std::filesystem::path(temporary) / (L"knock-hotkey-tests-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
    }
    ~Context() {
        if (first) DestroyWindow(first);
        if (second) DestroyWindow(second);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    bool available(const knock::HotkeyBinding& binding) const {
        const bool result = RegisterHotKey(second, 225, binding.modifiers | MOD_NOREPEAT, binding.key) != 0;
        if (result) UnregisterHotKey(second, 225);
        return result;
    }
};

void validation() {
    std::wstring reason;
    require(knock::valid_hotkey({}), "Default shortcut must be valid");
    require(knock::hotkey_label({}) == L"Ctrl + Alt + K", "Default label is wrong");
    require(knock::valid_hotkey({MOD_WIN | MOD_SHIFT, 'V', true}), "Win+Shift combination rejected");
    require(knock::valid_hotkey({0, VK_F8, true}), "Unmodified function key rejected");
    require(!knock::valid_hotkey({0, 'K', true}, &reason) && !reason.empty(), "Plain character must be rejected");
    require(!knock::valid_hotkey({MOD_CONTROL, VK_CONTROL, true}), "Modifier-only shortcut accepted");
    require(!knock::valid_hotkey({MOD_CONTROL, VK_F12, true}), "Reserved F12 accepted");
    require(!knock::valid_hotkey({0x4000, 'K', true}), "Unsupported stored modifier accepted");
    require(!knock::valid_hotkey({MOD_CONTROL, 0xFFFF, true}), "Invalid virtual key accepted");
    require(knock::valid_hotkey({0, 0, false}), "Disabled binding rejected");
    require(knock::hotkey_label({0, 0, false}) == L"Disabled", "Disabled label is wrong");
    std::cout << "PASS validation and labels\n";
}

void registration(Context& context) {
    knock::HotkeyManager manager(context.directory);
    require(manager.initialize(context.first), "Could not register the default shortcut in the private desktop");
    require(manager.matches(manager.registration_id()), "Active ID is not recognized");
    require(!manager.matches(224), "An unrelated hotkey was recognized");
    require(!context.available(manager.binding()), "Default shortcut was not reserved");
    const auto settings = context.directory / L"settings.json";
    write(settings, "{\"unrelated_preference\":\"keep this\"}");
    const knock::HotkeyBinding first{MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'J', true};
    require(manager.apply(first), "Changing shortcut failed");
    require(manager.binding() == first && !context.available(first), "New shortcut was not retained");
    require(context.available({}), "Previous shortcut remains registered");
    require(Json::parse(read(settings)).at("unrelated_preference") == "keep this", "Saving shortcut overwrote other settings");
    const auto saved = read(settings);
    const knock::HotkeyBinding occupied{MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'H', true};
    require(RegisterHotKey(context.second, 226, occupied.modifiers | MOD_NOREPEAT, occupied.key), "Could not reserve conflict fixture");
    require(!manager.apply(occupied) && !manager.error().empty(), "Conflicting shortcut accepted");
    require(manager.binding() == first && !context.available(first), "Conflict lost the working shortcut");
    require(read(settings) == saved, "Conflict changed persisted settings");
    UnregisterHotKey(context.second, 226);
    std::cout << "PASS registration, conflict and settings preservation\n";

    std::filesystem::rename(settings, context.directory / L"settings-good.json");
    std::filesystem::create_directory(settings);
    const knock::HotkeyBinding replacement{MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'G', true};
    require(!manager.apply(replacement), "Failed persistence was reported as success");
    require(manager.binding() == first && !context.available(first), "Failed save lost the working shortcut");
    require(context.available(replacement), "Failed save leaked the replacement registration");
    std::filesystem::remove(settings);
    std::filesystem::rename(context.directory / L"settings-good.json", settings);
    require(read(settings) == saved, "Failed save changed previous settings");
    std::cout << "PASS failed-save rollback\n";

    manager.stop();
    require(context.available(first), "Stop did not unregister the shortcut");
    require(manager.initialize(context.first) && manager.binding() == first, "Shortcut did not survive restart");
    require(manager.apply({0, 0, false}), "Disabling shortcut failed");
    require(!manager.matches(0) && manager.registration_id() == 0 && context.available(first), "Disabled shortcut is still registered");
    manager.stop();
    require(manager.initialize(context.first) && !manager.binding().enabled, "Disabled state did not survive restart");
    require(manager.apply({0, VK_F8, true}), "Enabling an unmodified function key failed");
    manager.stop();
    require(context.available({0, VK_F8, true}), "Final shortcut was not released");
    std::cout << "PASS restart, disable and teardown\n";
}

void corrupt_settings(Context& context) {
    const auto settings = context.directory / L"settings.json";
    write(settings, "{broken input");
    knock::HotkeyManager manager(context.directory);
    require(!manager.initialize(context.first), "Corrupt settings accepted");
    require(read(settings) == "{broken input", "Corrupt settings were overwritten");
    require(!manager.apply({}) && context.available({}), "Corrupt settings lost or leaked a registration");
    write(settings, "{\"shortcut\":{\"enabled\":true,\"key\":75,\"modifiers\":-1}}");
    require(!manager.initialize(context.first), "Negative modifiers accepted");
    write(settings, "{\"shortcut\":{\"enabled\":true,\"key\":75,\"modifiers\":18446744073709551615}}");
    require(!manager.initialize(context.first), "Integer overflow accepted");
    std::filesystem::remove(settings);
    std::cout << "PASS corrupt and invalid settings preservation\n";
}

void dialog(Context& context) {
    knock::HotkeyManager manager(context.directory);
    require(manager.initialize(context.first), "Dialog fixture could not register shortcut");
    bool found = false;
    std::thread automation([&] {
        for (int attempt = 0; attempt < 250; ++attempt) {
            HWND window = FindWindowW(L"Knock.Shortcut.v1", nullptr);
            DWORD process = 0;
            if (window) GetWindowThreadProcessId(window, &process);
            if (window && process == GetCurrentProcessId()) {
                HWND capture = GetDlgItem(window, 101);
                HWND save = GetDlgItem(window, 102);
                if (capture && save) {
                    found = true;
                    PostMessageW(capture, WM_KEYDOWN, VK_F9, 0);
                    PostMessageW(window, WM_COMMAND, MAKEWPARAM(102, BN_CLICKED), reinterpret_cast<LPARAM>(save));
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        PostMessageW(context.first, WM_QUIT, 1, 0);
    });
    const bool saved = manager.configure(context.first);
    automation.join();
    require(found && saved, "Native shortcut dialog did not capture and save a key");
    require(manager.binding() == knock::HotkeyBinding{0, VK_F9, true}, "Dialog saved the wrong key");
    require(IsWindowEnabled(context.first), "Dialog left its owner disabled");
    std::cout << "PASS native shortcut capture and save dialog\n";
}
}

int main() {
    if (FindWindowW(L"Knock.Tray.v1", nullptr)) {
        std::cout << "SKIP: close Knock before running shortcut tests.\n";
        return 77;
    }
    try {
        validation();
        Context context;
        registration(context);
        corrupt_settings(context);
        dialog(context);
        std::cout << "All hotkey checks passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
