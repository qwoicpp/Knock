#pragma once

#include "knock/store.hpp"
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace knock {
std::wstring widen(std::string_view value);
std::string narrow(std::wstring_view value);
std::filesystem::path executable_path();
std::filesystem::path data_directory();
std::wstring user_identity();
std::wstring pipe_name();
std::wstring quote_argument(std::wstring_view argument);
std::wstring command_line(const std::vector<std::wstring>& arguments);
Json send_command(const Json& command, bool start_app = true);
bool start_tray(bool show = false);
class PipeServer {
public:
    explicit PipeServer(Store& store);
    ~PipeServer();
    void start();
    void stop();
private:
    struct Impl;
    Impl* impl_;
};
int run_cli(const std::vector<std::wstring>& arguments);
int run_worker(const std::vector<std::wstring>& arguments);
}
