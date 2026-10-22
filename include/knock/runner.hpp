#pragma once

#include "knock/platform.hpp"

namespace knock {
struct RunOptions {
    std::string chat;
    std::string title;
    std::filesystem::path directory;
    std::vector<std::wstring> command;
};

Json start_job(const RunOptions& options);
}
