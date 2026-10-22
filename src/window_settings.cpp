#include "knock/window_settings.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace knock {
namespace {
using Json = nlohmann::json;
constexpr std::size_t max_settings_bytes = 64 * 1024;

Json read_settings(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return Json::object();
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > max_settings_bytes)
        throw std::runtime_error("invalid settings file");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open settings");
    auto settings = Json::parse(input);
    if (!settings.is_object()) throw std::runtime_error("invalid settings object");
    return settings;
}

int dimension(const Json& window, const char* key, int minimum, int maximum) {
    const auto found = window.find(key);
    if (found == window.end() || !found->is_number_integer())
        throw std::runtime_error("invalid window size");
    // Check unsigned values before converting so overflowing JSON integers
    // cannot turn into an apparently valid signed size.
    if (found->is_number_unsigned() && found->get<std::uint64_t>() > static_cast<std::uint64_t>(maximum))
        throw std::runtime_error("invalid window size");
    const auto value = found->get<std::int64_t>();
    if (value < minimum || value > maximum) throw std::runtime_error("invalid window size");
    return static_cast<int>(value);
}

WindowSize parse_size(const Json& settings) {
    const auto window = settings.find("window");
    if (window == settings.end()) return {};
    if (!window->is_object()) throw std::runtime_error("invalid window settings");
    return {dimension(*window, "width", 360, 800), dimension(*window, "height", 360, 1000)};
}

void write_settings(const std::filesystem::path& directory, const std::string& bytes) {
    std::filesystem::create_directories(directory);
    static std::atomic<unsigned int> sequence{0};
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = ::getpid();
#endif
    const auto temporary = directory / (".window-settings-" + std::to_string(process) + "-" +
        std::to_string(ticks) + "-" + std::to_string(sequence.fetch_add(1)) + ".tmp");
    const auto destination = directory / "settings.json";
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "create settings temporary file");
    try {
        DWORD written = 0;
        if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) || written != bytes.size())
            throw std::system_error(GetLastError(), std::system_category(), "write settings");
        if (!FlushFileBuffers(file))
            throw std::system_error(GetLastError(), std::system_category(), "flush settings");
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw std::system_error(GetLastError(), std::system_category(), "close settings");
        }
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "replace settings");
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(temporary.c_str());
        throw;
    }
#else
    int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) throw std::system_error(errno, std::generic_category(), "create settings temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(errno, std::generic_category(), "write settings");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(file) != 0) throw std::system_error(errno, std::generic_category(), "flush settings");
        if (::close(file) != 0) {
            file = -1;
            throw std::system_error(errno, std::generic_category(), "close settings");
        }
        file = -1;
        if (::rename(temporary.c_str(), destination.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "replace settings");
        // Replacement is the commit point. A later directory-flush failure
        // must not claim the previous settings are still in place.
        const int folder = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
        if (folder >= 0) { (void)::fsync(folder); ::close(folder); }
    } catch (...) {
        if (file >= 0) ::close(file);
        ::unlink(temporary.c_str());
        throw;
    }
#endif
}
}

bool valid_window_size(WindowSize size, std::wstring* reason) {
    if (reason) reason->clear();
    if (size.width < 360 || size.width > 800) {
        if (reason) *reason = L"Width must be between 360 and 800.";
        return false;
    }
    if (size.height < 360 || size.height > 1000) {
        if (reason) *reason = L"Height must be between 360 and 1000.";
        return false;
    }
    return true;
}

WindowSettings::WindowSettings(std::filesystem::path directory) : directory_(std::move(directory)) {}

bool WindowSettings::initialize() {
    error_.clear();
    try {
        const auto loaded = parse_size(read_settings(directory_ / "settings.json"));
        size_ = loaded;
        return true;
    } catch (const std::exception&) {
        error_ = L"Window settings could not be read. The current size and settings file were kept.";
        return false;
    }
}

bool WindowSettings::apply(WindowSize next) {
    error_.clear();
    if (!valid_window_size(next, &error_)) return false;
    try {
        auto settings = read_settings(directory_ / "settings.json");
        // An explicit Apply can repair invalid size fields in otherwise valid
        // settings. Keep unrelated preferences, including shortcut changes
        // saved after this WindowSettings instance was initialized.
        auto& window = settings["window"];
        if (!window.is_object()) window = Json::object();
        window["width"] = next.width;
        window["height"] = next.height;
        const auto contents = settings.dump(2) + "\n";
        if (contents.size() > max_settings_bytes) throw std::runtime_error("settings file would be too large");
        write_settings(directory_, contents);
        size_ = next;
        return true;
    } catch (const std::exception&) {
        error_ = L"Could not save the window size. The previous size and settings were kept.";
        return false;
    }
}

}
