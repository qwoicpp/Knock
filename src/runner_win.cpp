#include "knock/runner.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <process.h>
#include <stdexcept>

namespace knock {
namespace {

constexpr std::size_t max_log_bytes = 16 * 1024 * 1024;
constexpr std::size_t max_tail_bytes = 16000;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() { const auto value = value_; value_ = nullptr; return value; }
    void reset(HANDLE value = nullptr) {
        if (*this) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

std::string windows_error(std::string prefix, DWORD code = GetLastError()) {
    wchar_t* message = nullptr;
    const DWORD count = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    prefix += " (" + std::to_string(code) + ")";
    if (count && message) {
        prefix += ": " + narrow(std::wstring_view(message, count));
        LocalFree(message);
        while (!prefix.empty() && (prefix.back() == '\r' || prefix.back() == '\n')) prefix.pop_back();
    }
    return prefix;
}

void write_all(HANDLE file, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written == 0) {
            throw std::runtime_error(windows_error("Could not write file"));
        }
        offset += written;
    }
}

DWORD launch_worker(const std::filesystem::path& executable, std::wstring arguments,
                    const std::filesystem::path& directory) {
    SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle null(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            &inherited, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null) throw std::runtime_error(windows_error("Could not detach worker standard handles"));
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        throw std::runtime_error(windows_error("Could not initialize worker handles"));
    }
    struct Cleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~Cleanup() { DeleteProcThreadAttributeList(list); }
    } cleanup{list};
    HANDLE inherited_handle = null.get();
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &inherited_handle,
                                   sizeof(inherited_handle), nullptr, nullptr)) {
        throw std::runtime_error(windows_error("Could not restrict worker handles"));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null.get();
    startup.StartupInfo.hStdOutput = null.get();
    startup.StartupInfo.hStdError = null.get();
    startup.lpAttributeList = list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, directory.c_str(), &startup.StartupInfo, &process)) {
        throw std::runtime_error(windows_error("Could not start command worker"));
    }
    Handle process_handle(process.hProcess);
    Handle thread_handle(process.hThread);
    return process.dwProcessId;
}

void write_json_atomic(const std::filesystem::path& destination, const Json& document) {
    std::filesystem::create_directories(destination.parent_path());
    auto temporary = destination;
    temporary += "." + make_id() + ".tmp";
    Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) throw std::runtime_error(windows_error("Could not create worker state"));
    try {
        write_all(file.get(), document.dump(-1, ' ', false, Json::error_handler_t::replace));
        if (!FlushFileBuffers(file.get())) throw std::runtime_error(windows_error("Could not flush worker state"));
        file.reset();
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error(windows_error("Could not commit worker state"));
        }
    } catch (...) {
        file.reset();
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

Json read_spec(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 1024 * 1024) throw std::runtime_error("Worker specification is too large");
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open worker specification");
    return Json::parse(file);
}

std::string clean_tail(std::string_view bytes) {
    std::string stripped;
    stripped.reserve(bytes.size());
    bool escape = false;
    bool sequence = false;
    for (const unsigned char character : bytes) {
        if (escape) {
            escape = false;
            if (character == '[') sequence = true;
            continue;
        }
        if (sequence) {
            if (character >= 0x40 && character <= 0x7e) sequence = false;
            continue;
        }
        if (character == 0x1b) { escape = true; continue; }
        if (character < 0x20 && character != '\r' && character != '\n' && character != '\t') continue;
        stripped.push_back(static_cast<char>(character));
    }
    if (stripped.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, stripped.data(), static_cast<int>(stripped.size()), nullptr, 0);
    if (needed == 0) return "[Output could not be decoded; open the raw log.]";
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, stripped.data(), static_cast<int>(stripped.size()), wide.data(), needed);
    auto result = narrow(wide);
    if (result.size() > 14000) {
        std::size_t start = result.size() - 14000;
        while (start < result.size() && (static_cast<unsigned char>(result[start]) & 0xc0) == 0x80) ++start;
        result.erase(0, start);
    }
    return result;
}

struct Capture {
    HANDLE input = nullptr;
    HANDLE log = nullptr;
    std::atomic<bool> stopping{false};
    std::string tail;
    std::string error;
    std::size_t saved = 0;
    bool truncated = false;
};

unsigned __stdcall drain_output(void* context) {
    auto& capture = *static_cast<Capture*>(context);
    std::array<char, 8192> buffer{};
    try {
        while (!capture.stopping.load()) {
            DWORD count = 0;
            if (!ReadFile(capture.input, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                const auto error = GetLastError();
                if (error != ERROR_BROKEN_PIPE && error != ERROR_OPERATION_ABORTED) {
                    capture.error = windows_error("Could not read command output", error);
                }
                break;
            }
            if (!count) break;
            capture.tail.append(buffer.data(), count);
            if (capture.tail.size() > max_tail_bytes) capture.tail.erase(0, capture.tail.size() - max_tail_bytes);
            const auto keep = std::min<std::size_t>(count, max_log_bytes - capture.saved);
            if (keep && capture.error.empty()) {
                try {
                    write_all(capture.log, std::string_view(buffer.data(), keep));
                } catch (const std::exception& error) {
                    capture.error = error.what();
                }
                capture.saved += keep;
            }
            if (keep < count) capture.truncated = true;
        }
    } catch (const std::exception& error) {
        capture.error = error.what();
    }
    return 0;
}

struct ProcessResult {
    std::int32_t exit_code = -1;
    std::string tail;
    std::string error;
};

ProcessResult execute(const std::vector<std::wstring>& command, const std::filesystem::path& directory,
                      const std::filesystem::path& log_path) {
    if (command.empty() || command.front().empty()) throw std::runtime_error("No program to execute");
    auto line = command_line(command);
    if (line.size() >= 32767) throw std::runtime_error("Command exceeds the Windows command-line limit");

    std::filesystem::create_directories(log_path.parent_path());
    Handle log(CreateFileW(log_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!log) throw std::runtime_error(windows_error("Could not create command log"));
    SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &inherited, 0)) throw std::runtime_error(windows_error("Could not create output pipe"));
    Handle reader(read_raw);
    Handle writer(write_raw);
    if (!SetHandleInformation(reader.get(), HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error(windows_error("Could not protect output pipe"));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!input) throw std::runtime_error(windows_error("Could not open command input"));

    SIZE_T attributes_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
    std::vector<unsigned char> attributes(attributes_size);
    auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attributes_size)) {
        throw std::runtime_error(windows_error("Could not initialize command handles"));
    }
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(list); }
    } cleanup{attribute_list};
    HANDLE inherited_handles[] = {writer.get(), input.get()};
    if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles, sizeof(inherited_handles), nullptr, nullptr)) {
        throw std::runtime_error(windows_error("Could not restrict inherited handles"));
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = writer.get();
    startup.StartupInfo.hStdError = writer.get();
    startup.lpAttributeList = attribute_list;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
                        &startup.StartupInfo, &process)) {
        throw std::runtime_error(windows_error("Could not start " + narrow(command.front())));
    }
    Handle process_handle(process.hProcess);
    Handle primary_thread(process.hThread);
    writer.reset();
    input.reset();

    Capture capture;
    capture.input = reader.get();
    capture.log = log.get();
    Handle capture_thread(reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, drain_output, &capture, 0, nullptr)));
    if (!capture_thread) {
        TerminateProcess(process_handle.get(), static_cast<UINT>(-1));
        WaitForSingleObject(process_handle.get(), INFINITE);
        throw std::runtime_error("Could not start command output reader");
    }
    const auto waited = WaitForSingleObject(process_handle.get(), INFINITE);
    DWORD exit_code = static_cast<DWORD>(-1);
    if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process_handle.get(), &exit_code)) {
        capture.stopping.store(true);
        CancelSynchronousIo(capture_thread.get());
    }
    const bool descendant_output_open = WaitForSingleObject(capture_thread.get(), 2000) == WAIT_TIMEOUT;
    if (descendant_output_open) {
        capture.stopping.store(true);
        do {
            CancelSynchronousIo(capture_thread.get());
        } while (WaitForSingleObject(capture_thread.get(), 100) == WAIT_TIMEOUT);
    }

    std::string warnings;
    if (capture.truncated) warnings += "\n[Knock: raw log capped at 16 MiB; later output was drained and its latest tail retained.]\n";
    if (descendant_output_open) warnings += "\n[Knock: the command exited but detached descendants kept output open; later descendant output is not captured.]\n";
    if (!capture.error.empty()) warnings += "\n[Knock: " + capture.error + "]\n";
    if (!warnings.empty()) {
        try { write_all(log.get(), warnings); } catch (...) { /* The completion still records the error. */ }
    }
    if (!FlushFileBuffers(log.get()) && capture.error.empty()) capture.error = windows_error("Could not flush command log");
    auto tail = clean_tail(capture.tail);
    tail += warnings;
    return {static_cast<std::int32_t>(exit_code), std::move(tail), std::move(capture.error)};
}

std::string continuation(const Json& spec, std::int32_t code) {
    return "The command for @" + spec.at("chat").get<std::string>() + " has finished (exit code " + std::to_string(code) + ").\n"
           "Job: " + spec.at("id").get<std::string>() + "\n"
           "Task: " + spec.at("title").get<std::string>() + "\n"
           "Log: " + spec.at("log_path").get<std::string>() + "\n\n"
           "Read the log and continue from the result. You can use `knock job " + spec.at("id").get<std::string>() +
           "` to read the exit code and latest output. This message does not grant mouse or keyboard permission.";
}

std::string bounded_text(std::string_view text, std::size_t limit) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) return "[Text could not be decoded.]";
    std::wstring decoded(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), decoded.data(), count);
    auto valid = narrow(decoded);
    if (valid.size() <= limit) return valid;
    constexpr std::string_view suffix = "\n[Knock: text truncated.]";
    std::size_t end = limit - suffix.size();
    while (end && (static_cast<unsigned char>(valid[end]) & 0xc0) == 0x80) --end;
    valid.resize(end);
    valid += suffix;
    return valid;
}

void finish_job(Json command) {
    for (const auto* field : {"output_tail", "resume_text", "error"}) {
        if (command.contains(field)) command[field] = bounded_text(command.at(field).get<std::string>(), 16000);
    }
    const auto path = data_directory() / L"completions" / (widen(command.at("id").get<std::string>()) + L".json");
    bool persisted = false;
    std::string storage_error;
    try {
        write_json_atomic(path, command);
        persisted = true;
    } catch (const std::exception& error) {
        storage_error = error.what();
    }
    try {
        const auto response = send_command(command);
        if (response.value("ok", false)) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
            return;
        }
    } catch (...) {
        // The durable completion is replayed by the tray next time it starts.
    }
    if (!persisted) throw std::runtime_error("Could not deliver or save completion: " + storage_error);
}

} // namespace

Json start_job(const RunOptions& options) {
    const auto directory = data_directory();
    const auto token = make_id();
    const auto spec_path = directory / L"jobs" / (widen(token) + L".json");
    const auto log_path = directory / L"logs" / (widen(token) + L".log");
    std::filesystem::create_directories(spec_path.parent_path());
    std::filesystem::create_directories(log_path.parent_path());
    if (command_line(options.command).size() >= 32767) throw std::runtime_error("Command exceeds the Windows command-line limit");
    auto response = send_command({{"op", "job.create"}, {"chat", options.chat}, {"title", options.title},
                                  {"command", narrow(command_line(options.command))},
                                  {"cwd", narrow(options.directory.wstring())}, {"log_path", narrow(log_path.wstring())}});
    if (!response.value("ok", false)) return response;
    const auto id = response.at("job").at("id").get<std::string>();
    Json spec{{"id", id}, {"chat", options.chat}, {"title", options.title},
              {"cwd", narrow(options.directory.wstring())}, {"log_path", narrow(log_path.wstring())},
              {"command", Json::array()}};
    for (const auto& argument : options.command) spec["command"].push_back(narrow(argument));
    try {
        write_json_atomic(spec_path, spec);
        auto arguments = command_line({executable_path().wstring(), L"_worker", widen(token)});
        response["worker_pid"] = launch_worker(executable_path(), std::move(arguments), options.directory);
        response["detached"] = true;
        return response;
    } catch (const std::exception& error) {
        std::error_code ignored;
        std::filesystem::remove(spec_path, ignored);
        finish_job({{"op", "job.finish"}, {"id", id}, {"exit_code", -1},
                    {"output_tail", error.what()}, {"error", error.what()}, {"resume_text", continuation(spec, -1)}});
        return {{"ok", false}, {"error", error.what()}, {"job_id", id}};
    }
}

int run_worker(const std::vector<std::wstring>& arguments) {
    if (arguments.size() != 2 || arguments[0] != L"_worker") return 5;
    const auto& token = arguments[1];
    if (token.empty() || token.size() > 80 || token.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-") != std::wstring::npos) return 5;
    const auto path = data_directory() / L"jobs" / (token + L".json");
    const auto spec = read_spec(path);
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    const auto id = spec.at("id").get<std::string>();
    ProcessResult result;
    try {
        const auto started = send_command({{"op", "job.started"}, {"id", id}, {"worker_pid", GetCurrentProcessId()}});
        if (!started.value("ok", false)) throw std::runtime_error("Could not register command worker: " + started.value("error", "unknown error"));
        std::vector<std::wstring> command;
        for (const auto& argument : spec.at("command")) command.push_back(widen(argument.get<std::string>()));
        result = execute(command, std::filesystem::path(widen(spec.at("cwd").get<std::string>())),
                         std::filesystem::path(widen(spec.at("log_path").get<std::string>())));
    } catch (const std::exception& error) {
        result.exit_code = -1;
        result.error = error.what();
        result.tail = error.what();
    }
    Json completed{{"op", "job.finish"}, {"id", id}, {"exit_code", result.exit_code},
                   {"output_tail", result.tail}, {"resume_text", continuation(spec, result.exit_code)}};
    if (!result.error.empty()) completed["error"] = result.error;
    finish_job(completed);
    return 0;
}
} // namespace knock
