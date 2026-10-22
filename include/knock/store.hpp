#pragma once

#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>

namespace knock {
using Json = nlohmann::json;
using ChangeCallback = std::function<void(const Json&)>;

// The tray process owns this store. Only direct GUI calls set trusted_ui=true.
class Store {
public:
    explicit Store(std::filesystem::path directory);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void load();
    Json handle(const Json& command, bool trusted_ui = false);
    Json snapshot();
    void set_changed(ChangeCallback callback);
    void tick();
    void stop();
    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    Json state_;
    std::mutex mutex_;
    std::condition_variable changed_;
    ChangeCallback callback_;
    bool stopped_ = false;
    bool loaded_ = false;

    void save_locked();
    bool expire_locked();
};

std::int64_t now_ms();
std::string make_id();
}
