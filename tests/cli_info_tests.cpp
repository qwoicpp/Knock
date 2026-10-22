#include "knock/runner.hpp"
#include "knock/agent_info.hpp"

#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
unsigned int platform_calls = 0;
bool fake_ipc_enabled = false;
std::vector<knock::Json> ipc_commands;
std::vector<knock::Json> ipc_results;
std::size_t ipc_result_index = 0;

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

class FakeIpc {
public:
    explicit FakeIpc(std::vector<knock::Json> responses) {
        ipc_commands.clear();
        ipc_results = std::move(responses);
        ipc_result_index = 0;
        fake_ipc_enabled = true;
    }
    ~FakeIpc() { fake_ipc_enabled = false; }
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

void presence_commands_use_readers_and_respect_wait_intent() {
    using knock::Json;
    const Json presence{{"state", "away"}, {"message", "Out for coffee."}, {"updated_at", 1000}};
    const Json query{{"ok", true}, {"presence", presence}};
    {
        FakeIpc ipc({query});
        Capture capture;
        require(knock::run_cli({L"presence"}) == 0 && Json::parse(capture.text()) == query,
                "presence did not return its read-only response");
        require(ipc_commands == std::vector<Json>{{{"op", "presence.get"}}},
                "presence command did not use the read-only operation");
    }
    const Json pending{{"id", "pending-1"}, {"kind", "input"}, {"status", "pending"}, {"lease_until", 0}};
    const Json created{{"ok", true}, {"request", pending}, {"presence", presence}};
    auto waiting = created;
    waiting["wait_reason"] = "presence";
    const std::vector<std::wstring> request{L"request", L"--chat", L"AUDIO-1", L"--message", L"May I test the interface?"};
    {
        FakeIpc ipc({created, waiting});
        Capture capture;
        require(knock::run_cli(request) == 7 && Json::parse(capture.text()) == waiting,
                "unavailable presence did not return the pending ID, guidance, and exit 7");
        require(ipc_commands.size() == 2 && ipc_commands[0]["op"] == "request.create" &&
                ipc_commands[1] == Json{{"op", "request.wait"}, {"id", "pending-1"},
                    {"wait_seconds", 1801}, {"stop_when_unavailable", true}},
                "ordinary request skipped the final decision check or omitted presence-aware waiting");
    }
    {
        FakeIpc ipc({created});
        Capture capture;
        auto detached = request;
        detached.push_back(L"--detach");
        require(knock::run_cli(detached) == 0 && Json::parse(capture.text()) == created,
                "detached request changed its pending response or success exit code");
        require(ipc_commands.size() == 1 && ipc_commands[0]["op"] == "request.create",
                "detached request unexpectedly waited");
    }
    {
        FakeIpc ipc({created});
        Capture capture;
        require(knock::run_cli({L"result", L"pending-1", L"--wait", L"300"}) == 7 &&
                Json::parse(capture.text()) == created, "explicit wait lost a pending response or presence");
        require(ipc_commands == std::vector<Json>{{{"op", "request.wait"}, {"id", "pending-1"}, {"wait_seconds", 300}}},
                "explicit result wait changed its time budget or enabled early presence return");
    }
    for (const auto& arguments : std::vector<std::vector<std::wstring>>{
            {L"presence", L"away"}, {L"presence", L"--state", L"away"}, {L"presence", L"--detach"}}) {
        FakeIpc ipc({});
        Capture capture;
        bool rejected = false;
        try { (void)knock::run_cli(arguments); } catch (const std::runtime_error&) { rejected = true; }
        require(rejected && ipc_commands.empty() && capture.text().empty(),
                "presence accepted editing arguments or contacted IPC before rejecting them");
    }
}
}

// Help must not need IPC, tray startup, a working data directory, text files,
// or a clock. The command checks explicitly enable a small fake IPC boundary.
namespace knock {
std::wstring widen(std::string_view) { unexpected_platform_call(); }
std::string narrow(std::wstring_view value) {
    if (!fake_ipc_enabled) unexpected_platform_call();
    std::string result;
    for (const auto character : value) {
        require(static_cast<unsigned int>(character) <= 127, "Fake CLI arguments must be ASCII");
        result.push_back(static_cast<char>(character));
    }
    return result;
}
Json send_command(const Json& command, bool) {
    if (!fake_ipc_enabled) unexpected_platform_call();
    ipc_commands.push_back(command);
    require(ipc_result_index < ipc_results.size(), "Unexpected extra IPC command");
    return ipc_results[ipc_result_index++];
}
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
        require(help.find("knock info") != std::string::npos && help.find("knock presence") != std::string::npos,
                "Short help does not expose info and presence");
        std::cout << "PASS short help points to the embedded agent guide\n";

        for (const auto* action : {L"info", L"get-info", L"help", L"--help", L"-h"}) {
            rejects({action, L"unexpected"});
            rejects({action, L"--body-file", L"missing-private-file.txt"});
            rejects({action, L"--"});
        }
        require(platform_calls == 0, "Help or info contacted the application");
        std::cout << "PASS extra arguments fail without tray, IPC, or platform access\n";

        presence_commands_use_readers_and_respect_wait_intent();
        require(platform_calls == 0, "CLI routing checks crossed an unexpected platform boundary");
        std::cout << "PASS presence, pending exit codes, and explicit wait intent\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
