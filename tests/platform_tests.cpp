#include "knock/platform.hpp"
#include <shellapi.h>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

int main() {
    using namespace knock;
    try {
        if (FindWindowW(L"Knock.Tray.v1", nullptr)) {
            std::cout << "SKIP: close KnockTray before running local IPC tests.\n";
            return 77;
        }
        const std::string unicode = "Knock \xE2\x80\x94 \xF0\x9F\x8E\xA7 \xE6\xBC\xA2\xE5\xAD\x97";
        require(narrow(widen(unicode)) == unicode, "Unicode round trip failed");
        bool rejected = false;
        try { (void)widen("\xC0\xAF"); } catch (...) { rejected = true; }
        require(rejected, "Invalid UTF-8 accepted");
        rejected = false;
        try { (void)narrow(std::wstring(1, static_cast<wchar_t>(0xd800))); } catch (...) { rejected = true; }
        require(rejected, "Invalid UTF-16 accepted");
        require(quote_argument(L"/c") == L"/c", "Plain shell switch was unnecessarily quoted");
        require(quote_argument(L"plain") == L"plain", "Plain argument was unnecessarily quoted");
        std::vector<std::wstring> arguments{
            L"program.exe", L"", L"simple", L"a b", L"C:\\dir\\", L"a\\\"b", L"a\"b", L"\t",
            L"C:\\quoted path\\", L"two\\\\\"quotes\"\\", widen(unicode)};
        auto line = command_line(arguments);
        int count = 0;
        wchar_t** parsed = CommandLineToArgvW(line.c_str(), &count);
        require(parsed && static_cast<std::size_t>(count) == arguments.size(), "Argument count mismatch");
        for (int i = 0; i < count; ++i) require(parsed[i] == arguments[i], "Argument quoting mismatch");
        LocalFree(parsed);
        rejected = false;
        try { (void)quote_argument(std::wstring(L"x\0y", 3)); } catch (...) { rejected = true; }
        require(rejected, "Embedded null in command accepted");
        require(user_identity().find(L'.') != std::wstring::npos, "No session in identity");
        wchar_t temp[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temp) > 0, "No temp directory");
        const auto directory = std::filesystem::path(temp) / (L"knock-ipc-tests-" + std::to_wstring(GetCurrentProcessId()));
        SetEnvironmentVariableW(L"KNOCK_DATA_DIR", directory.c_str());
        require(data_directory() == directory, "Data directory override failed");
        Store store(directory);
        store.load();
        PipeServer server(store);
        server.start();
        auto created = send_command({{"op", "request.create"}, {"chat", "AUDIO-1"}, {"message", unicode}, {"timeout_seconds", 60}}, false);
        require(created.value("ok", false), "Request create failed");
        const std::string id = created["request"]["id"];
        auto attack = send_command({{"op", "request.resolve"}, {"id", id}, {"decision", "approved"}}, false);
        require(!attack.value("ok", true), "IPC permitted an approval");
        auto waiter = std::async(std::launch::async, [&] {
            return send_command({{"op", "request.wait"}, {"id", id}, {"wait_seconds", 60}}, false);
        });
        require(waiter.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
                "Pending wait did not block");
        auto approved = store.handle({{"op", "request.resolve"}, {"id", id}, {"decision", "approved"}}, true);
        require(approved.value("ok", false), "Trusted approval failed");
        auto answer = waiter.get();
        require(answer.value("ok", false) && answer["request"]["status"] == "approved", "Wait response failed");
        require(send_command({{"op", "request.release"}, {"id", id}}, false).value("ok", false), "Release failed");
        for (int i = 0; i < 100; ++i) {
            require(send_command({{"op", "snapshot"}}, false).value("ok", false), "Short IPC reply was lost");
        }
        auto note = send_command({{"op", "note.add"}, {"title", unicode}, {"body", unicode}}, false);
        require(note.value("ok", false) && note["note"]["body"] == unicode, "Unicode IPC failed");
        auto too_large = send_command({{"op", "notify"}, {"chat", "X"}, {"message", std::string(1024 * 1024, 'x')}}, false);
        require(!too_large.value("ok", true), "Oversized client frame accepted");
        HANDLE malformed = CreateFileW(pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
        require(malformed != INVALID_HANDLE_VALUE, "Malformed test connect failed");
        unsigned char header[4]{255, 255, 255, 255};
        DWORD bytes = 0;
        require(WriteFile(malformed, header, 4, &bytes, nullptr), "Malformed test write failed");
        require(ReadFile(malformed, header, 4, &bytes, nullptr) && bytes == 4, "Malformed frame got no error reply");
        const DWORD length = static_cast<DWORD>(header[0]) |
                             (static_cast<DWORD>(header[1]) << 8) |
                             (static_cast<DWORD>(header[2]) << 16) |
                             (static_cast<DWORD>(header[3]) << 24);
        require(length > 0 && length < 1024, "Malformed frame error length invalid");
        std::string response(length, '\0');
        require(ReadFile(malformed, response.data(), length, &bytes, nullptr), "Malformed frame error read failed");
        require(!Json::parse(response).value("ok", true), "Oversized server frame accepted");
        CloseHandle(malformed);
        auto pending = send_command({{"op", "request.create"}, {"chat", "WAIT-2"}, {"message", "wait"}, {"timeout_seconds", 60}}, false);
        const std::string pending_id = pending["request"]["id"];
        auto stopped_waiter = std::async(std::launch::async, [&] {
            return send_command({{"op", "request.wait"}, {"id", pending_id}, {"wait_seconds", 60}}, false);
        });
        require(stopped_waiter.wait_for(std::chrono::milliseconds(100)) == std::future_status::timeout,
                "Second pending wait did not block");
        HANDLE slow = CreateFileW(pipe_name().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        require(slow != INVALID_HANDLE_VALUE, "Slow client connect failed");
        Sleep(50);
        const auto start = std::chrono::steady_clock::now();
        server.stop();
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(3), "Shutdown stalled on reader/waiter");
        CloseHandle(slow);
        auto stopped = stopped_waiter.get();
        require(!stopped.value("ok", true), "Shutdown manufactured an answer");
        std::filesystem::remove_all(directory);
        std::cout << "PASS: strict Unicode, quoting, named-pipe frames, 100 replies, GUI-only decisions, wait/release, oversized frames, and bounded shutdown\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
