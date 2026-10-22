#include "knock/runner.hpp"
#include "knock/agent_info.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
unsigned int platform_calls = 0;

[[noreturn]] void unexpected_platform_call() {
    ++platform_calls;
    throw std::runtime_error("Offline help attempted to use the platform");
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Capture {
public:
    Capture() : previous_(std::cout.rdbuf(stream_.rdbuf())) {}
    ~Capture() { std::cout.rdbuf(previous_); }
    std::string text() const { return stream_.str(); }
private:
    std::ostringstream stream_;
    std::streambuf* previous_;
};

std::string invoke(const std::vector<std::wstring>& arguments) {
    Capture capture;
    require(knock::run_cli(arguments) == 0, "Offline guide did not succeed");
    return capture.text();
}

void rejects(const std::vector<std::wstring>& arguments) {
    Capture capture;
    bool rejected = false;
    try {
        (void)knock::run_cli(arguments);
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).starts_with("Help and info take no arguments");
    }
    require(rejected, "Extra guide arguments were not rejected before platform access");
    require(capture.text().empty(), "Invalid guide arguments printed a partial guide");
}
}

// Link the real CLI parser to failing platform boundaries. Help must not need
// IPC, tray startup, a working data directory, text-file reads, or a clock.
namespace knock {
std::wstring widen(std::string_view) { unexpected_platform_call(); }
std::string narrow(std::wstring_view) { unexpected_platform_call(); }
Json send_command(const Json&, bool) { unexpected_platform_call(); }
bool start_tray(bool) { unexpected_platform_call(); }
Json start_job(const RunOptions&) { unexpected_platform_call(); }
std::int64_t now_ms() { unexpected_platform_call(); }
}

int main() {
    try {
        const auto guide = invoke({L"info"});
        require(guide == knock::agent_info, "Terminal guide differs from embedded documentation");
        require(invoke({L"get-info"}) == guide, "get-info differs from info");
        require(guide.find("request.lease_until") != std::string::npos &&
                guide.find("--detach") != std::string::npos &&
                guide.find("--wait 300") != std::string::npos &&
                guide.find("does not automatically wake a CLI coding agent") != std::string::npos &&
                guide.find("Do not poll") != std::string::npos &&
                guide.find("next_offset") != std::string::npos,
                "Guide is missing a required permission or efficient-waiting rule");
        require(guide.size() < 16 * 1024, "Agent guide grew beyond a small one-call response");
        std::cout << "PASS info and alias include permission, waiting, and bounded-output guidance\n";

        const auto help = invoke({L"help"});
        for (const auto& arguments : std::vector<std::vector<std::wstring>>{{}, {L"--help"}, {L"-h"}})
            require(invoke(arguments) == help, "Help aliases differ");
        require(help.find("knock info") != std::string::npos, "Short help does not expose info");
        std::cout << "PASS short help points to the embedded agent guide\n";

        for (const auto* action : {L"info", L"get-info", L"help", L"--help", L"-h"}) {
            rejects({action, L"unexpected"});
            rejects({action, L"--body-file", L"missing-private-file.txt"});
            rejects({action, L"--"});
        }
        require(platform_calls == 0, "Help or info contacted the application");
        std::cout << "PASS extra arguments fail without tray, IPC, or platform access\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
