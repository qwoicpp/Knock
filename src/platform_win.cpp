#include "knock/platform.hpp"

#include <sddl.h>
#include <shlobj.h>

#include <limits>
#include <stdexcept>

namespace knock {
namespace {

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE value) : value_(value) {}
    ~ScopedHandle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

std::runtime_error windows_error(const char* operation) {
    return std::runtime_error(std::string(operation) + " failed (Windows error " +
                              std::to_string(GetLastError()) + ")");
}

std::wstring environment_value(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), required);
    if (written == 0 || written >= required) {
        throw windows_error("Reading environment variable");
    }
    value.resize(written);
    return value;
}

} // namespace

std::wstring widen(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-8 text is too long");
    }
    const auto length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                            value.data(), length, nullptr, 0);
    if (required == 0) {
        throw std::runtime_error("Invalid UTF-8 text");
    }
    std::wstring result(required, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
                            result.data(), required) != required) {
        throw windows_error("Converting UTF-8 text");
    }
    return result;
}

std::string narrow(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("UTF-16 text is too long");
    }
    const auto length = static_cast<int>(value.size());
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                            value.data(), length, nullptr, 0,
                                            nullptr, nullptr);
    if (required == 0) {
        throw std::runtime_error("Invalid UTF-16 text");
    }
    std::string result(required, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), length,
                            result.data(), required, nullptr, nullptr) != required) {
        throw windows_error("Converting UTF-16 text");
    }
    return result;
}

std::filesystem::path executable_path() {
    std::wstring path(512, L'\0');
    for (;;) {
        const DWORD written = GetModuleFileNameW(nullptr, path.data(),
                                                static_cast<DWORD>(path.size()));
        if (written == 0) {
            throw windows_error("Finding executable path");
        }
        if (written < path.size()) {
            path.resize(written);
            return std::filesystem::path(path);
        }
        if (path.size() >= 32768) {
            throw std::runtime_error("Executable path is too long");
        }
        path.resize(path.size() * 2);
    }
}

std::filesystem::path data_directory() {
    if (auto override_path = environment_value(L"KNOCK_DATA_DIR"); !override_path.empty()) {
        return std::filesystem::absolute(std::filesystem::path(override_path));
    }
    if (auto local = environment_value(L"LOCALAPPDATA"); !local.empty()) {
        return std::filesystem::path(local) / L"Knock";
    }
    wchar_t path[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE,
                                nullptr, SHGFP_TYPE_CURRENT, path))) {
        throw std::runtime_error("Cannot locate the current user's local app data");
    }
    return std::filesystem::path(path) / L"Knock";
}

std::wstring user_identity() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        throw windows_error("Opening current user token");
    }
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0) {
        const auto error = windows_error("Reading current user token");
        CloseHandle(token);
        throw error;
    }
    std::vector<unsigned char> buffer(required);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), required, &required)) {
        const auto error = windows_error("Reading current user token");
        CloseHandle(token);
        throw error;
    }
    CloseHandle(token);
    LPWSTR sid_text = nullptr;
    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    if (!ConvertSidToStringSidW(token_user->User.Sid, &sid_text)) {
        throw windows_error("Reading current user identity");
    }
    std::wstring identity(sid_text);
    LocalFree(sid_text);
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        throw windows_error("Reading current Windows session");
    }
    return identity + L"." + std::to_wstring(session);
}

std::wstring pipe_name() {
    return L"\\\\.\\pipe\\Knock.v1." + user_identity();
}

std::wstring quote_argument(std::wstring_view argument) {
    if (argument.find(L'\0') != std::wstring_view::npos) {
        throw std::runtime_error("Command arguments cannot contain a null character");
    }
    if (!argument.empty() && argument.find_first_of(L" \t\r\n\v\f\"") == std::wstring_view::npos) {
        return std::wstring(argument);
    }
    // Windows CRT parsing doubles backslashes only before a quote or the final quote.
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
        } else {
            result.append(slashes, L'\\');
        }
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring command_line(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    for (const auto& argument : arguments) {
        if (!result.empty()) {
            result.push_back(L' ');
        }
        result += quote_argument(argument);
    }
    if (result.size() > 32766) {
        throw std::runtime_error("Command line exceeds the Windows length limit");
    }
    return result;
}

bool start_tray(bool show) {
    if (const HWND window = FindWindowW(L"Knock.Tray.v1", nullptr)) {
        if (show) {
            DWORD process_id = 0;
            GetWindowThreadProcessId(window, &process_id);
            if (process_id != 0) {
                AllowSetForegroundWindow(process_id);
            }
            return PostMessageW(window, WM_APP + 20, 0, 0) != FALSE;
        }
        return true;
    }
    const auto executable = executable_path().parent_path() / L"KnockTray.exe";
    if (!std::filesystem::is_regular_file(executable)) {
        return false;
    }
    auto arguments = command_line({executable.wstring(), show ? L"--show" : L"--background"});
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    ScopedHandle null_device(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (null_device.get() == INVALID_HANDLE_VALUE) {
        return false;
    }
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attribute_storage(attribute_bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) {
        return false;
    }
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attribute_guard{attributes};
    HANDLE allowed[] = {null_device.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   allowed, sizeof(allowed), nullptr, nullptr)) {
        return false;
    }
    // A long-lived GUI must not retain an agent host's captured output pipes.
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_device.get();
    startup.StartupInfo.hStdOutput = null_device.get();
    startup.StartupInfo.hStdError = null_device.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    const auto directory = executable.parent_path().wstring();
    if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr,
                         TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, directory.c_str(),
                         &startup.StartupInfo, &process)) {
        return false;
    }
    if (show) {
        AllowSetForegroundWindow(process.dwProcessId);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

} // namespace knock
