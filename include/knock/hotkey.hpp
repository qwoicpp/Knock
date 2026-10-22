#pragma once

#include <windows.h>
#include <filesystem>
#include <string>

namespace knock {

struct HotkeyBinding {
    UINT modifiers = MOD_CONTROL | MOD_ALT;
    UINT key = 'K';
    bool enabled = true;
    bool operator==(const HotkeyBinding&) const = default;
};

bool valid_hotkey(const HotkeyBinding& binding, std::wstring* reason = nullptr);
std::wstring hotkey_label(const HotkeyBinding& binding);

// All calls belong on the thread that owns the tray window. Two fixed IDs let
// us reserve a replacement before giving up the currently working shortcut.
class HotkeyManager {
public:
    explicit HotkeyManager(std::filesystem::path data_directory);
    ~HotkeyManager();
    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    bool initialize(HWND owner);
    bool apply(HotkeyBinding binding);
    bool configure(HWND owner);
    void stop() noexcept;
    bool matches(WPARAM id) const noexcept;
    int registration_id() const noexcept { return active_id_; }
    const HotkeyBinding& binding() const noexcept { return binding_; }
    std::wstring label() const;
    const std::wstring& error() const noexcept { return error_; }

private:
    std::filesystem::path directory_;
    HWND owner_ = nullptr;
    HotkeyBinding binding_;
    int active_id_ = 0;
    std::wstring error_;
    bool save(const HotkeyBinding& binding);
};
}
