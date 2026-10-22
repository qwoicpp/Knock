#pragma once

#include <filesystem>
#include <string>

namespace knock {

struct WindowSize {
    int width = 420;
    int height = 510;
    bool operator==(const WindowSize&) const = default;
};

bool valid_window_size(WindowSize size, std::wstring* reason = nullptr);

// Sizes are in logical pixels. Call from the UI thread, like HotkeyManager;
// each save reads the latest shared settings before replacing the file.
class WindowSettings {
public:
    explicit WindowSettings(std::filesystem::path data_directory);

    bool initialize();
    bool apply(WindowSize size);
    const WindowSize& size() const noexcept { return size_; }
    const std::wstring& error() const noexcept { return error_; }

private:
    std::filesystem::path directory_;
    WindowSize size_;
    std::wstring error_;
};

}
