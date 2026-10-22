#include "knock/platform.hpp"

#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace knock {
namespace {

constexpr DWORD frame_limit = 1024 * 1024;
constexpr DWORD request_io_timeout = 15000;
constexpr unsigned connection_limit = 64;
constexpr unsigned waiter_limit = 32;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }
    HANDLE get() const { return value_; }
    bool valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (valid()) {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_;
};

std::runtime_error io_error(const char* operation, DWORD error = GetLastError()) {
    return std::runtime_error(std::string(operation) + " failed (Windows error " +
                              std::to_string(error) + ")");
}

DWORD time_left(ULONGLONG deadline) {
    const ULONGLONG now = GetTickCount64();
    return now < deadline ? static_cast<DWORD>(deadline - now) : 0;
}

DWORD wait_io(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout) {
    HANDLE events[]{stop, operation.hEvent};
    const DWORD waited = stop ? WaitForMultipleObjects(2, events, FALSE, timeout)
                              : WaitForSingleObject(operation.hEvent, timeout);
    const DWORD completed = stop ? WAIT_OBJECT_0 + 1 : WAIT_OBJECT_0;
    if (waited != completed) {
        const DWORD error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_OPERATION_ABORTED;
        CancelIoEx(pipe, &operation);
        DWORD ignored = 0;
        // The OVERLAPPED and its event must remain alive until cancellation completes.
        GetOverlappedResult(pipe, &operation, &ignored, TRUE);
        throw io_error("Local pipe operation", error);
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, &operation, &transferred, FALSE)) {
        throw io_error("Local pipe operation");
    }
    return transferred;
}

void transfer_exact(HANDLE pipe, void* bytes, DWORD count, bool writing,
                    HANDLE stop, ULONGLONG deadline) {
    auto* cursor = static_cast<unsigned char*>(bytes);
    while (count != 0) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            throw io_error("Local pipe operation", ERROR_OPERATION_ABORTED);
        }
        if (time_left(deadline) == 0) {
            throw io_error("Local pipe operation", ERROR_TIMEOUT);
        }
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) {
            throw io_error("Creating pipe event");
        }
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        DWORD transferred = 0;
        const BOOL completed = writing
            ? WriteFile(pipe, cursor, count, &transferred, &operation)
            : ReadFile(pipe, cursor, count, &transferred, &operation);
        if (!completed) {
            if (GetLastError() != ERROR_IO_PENDING) {
                throw io_error(writing ? "Writing local pipe" : "Reading local pipe");
            }
            transferred = wait_io(pipe, operation, stop, time_left(deadline));
        }
        if (transferred == 0) {
            throw io_error("Local pipe closed", ERROR_BROKEN_PIPE);
        }
        cursor += transferred;
        count -= transferred;
    }
}

Json read_frame(HANDLE pipe, HANDLE stop, DWORD timeout) {
    const ULONGLONG deadline = GetTickCount64() + timeout;
    unsigned char header[4]{};
    transfer_exact(pipe, header, 4, false, stop, deadline);
    const DWORD length = static_cast<DWORD>(header[0]) |
                        (static_cast<DWORD>(header[1]) << 8) |
                        (static_cast<DWORD>(header[2]) << 16) |
                        (static_cast<DWORD>(header[3]) << 24);
    if (length == 0 || length > frame_limit) {
        throw std::runtime_error("Local pipe frame must contain 1 to 1048576 bytes");
    }
    std::string payload(length, '\0');
    transfer_exact(pipe, payload.data(), length, false, stop, deadline);
    return Json::parse(payload);
}

void write_frame(HANDLE pipe, const Json& value, HANDLE stop, DWORD timeout) {
    const std::string payload = value.dump();
    if (payload.empty() || payload.size() > frame_limit) {
        throw std::runtime_error("Local pipe response exceeds the 1 MiB limit");
    }
    const auto length = static_cast<DWORD>(payload.size());
    unsigned char header[]{static_cast<unsigned char>(length),
                           static_cast<unsigned char>(length >> 8),
                           static_cast<unsigned char>(length >> 16),
                           static_cast<unsigned char>(length >> 24)};
    const ULONGLONG deadline = GetTickCount64() + timeout;
    transfer_exact(pipe, header, 4, true, stop, deadline);
    transfer_exact(pipe, const_cast<char*>(payload.data()), length, true, stop, deadline);
}

void finish_connection(HANDLE pipe, HANDLE stop, DWORD timeout = 2000) {
    // Clients close after reading the reply. Wait briefly for that close before
    // disconnecting, which otherwise can discard unread pipe data. Never use
    // FlushFileBuffers here: a non-reading client could block shutdown forever.
    unsigned char ignored = 0;
    try {
        transfer_exact(pipe, &ignored, 1, false, stop, GetTickCount64() + timeout);
    } catch (...) {
    }
    DisconnectNamedPipe(pipe);
}

DWORD response_timeout(const Json& command) {
    if (command.is_object() && command.value("op", "") == "request.wait") {
        const auto seconds = std::clamp<std::int64_t>(
            command.value("wait_seconds", std::int64_t{86400}), 1, 86400);
        return static_cast<DWORD>(seconds * 1000 + 10000);
    }
    return 30000;
}

} // namespace

Json send_command(const Json& command, bool start_app) {
    try {
        const auto name = pipe_name();
        const ULONGLONG deadline = GetTickCount64() + (start_app ? 8000 : 1000);
        bool launch_attempted = false;
        Handle pipe;
        for (;;) {
            pipe.reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                                       SECURITY_IDENTIFICATION,
                                   nullptr));
            if (pipe.valid()) {
                break;
            }
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY &&
                error != ERROR_SEM_TIMEOUT) {
                throw io_error("Connecting to Knock", error);
            }
            if (start_app && !launch_attempted && error == ERROR_FILE_NOT_FOUND) {
                launch_attempted = true;
                if (!start_tray()) {
                    throw std::runtime_error("Cannot start KnockTray.exe; keep it beside knock.exe");
                }
            }
            if (time_left(deadline) == 0) {
                throw std::runtime_error("Knock is not responding; open KnockTray.exe and try again");
            }
            if (error == ERROR_PIPE_BUSY) {
                WaitNamedPipeW(name.c_str(), std::min<DWORD>(200, time_left(deadline)));
            } else {
                Sleep(std::min<DWORD>(50, time_left(deadline)));
            }
        }
        if (command.is_object() && command.value("op", "") == "ui.show") {
            ULONG process_id = 0;
            if (GetNamedPipeServerProcessId(pipe.get(), &process_id)) {
                AllowSetForegroundWindow(process_id);
            }
        }
        write_frame(pipe.get(), command, nullptr, request_io_timeout);
        auto response = read_frame(pipe.get(), nullptr, response_timeout(command));
        if (!response.is_object() || !response.contains("ok") || !response["ok"].is_boolean()) {
            throw std::runtime_error("Knock returned an invalid protocol response");
        }
        return response;
    } catch (const std::exception& error) {
        return {{"ok", false}, {"error", error.what()}};
    }
}

struct PipeServer::Impl {
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> finished;
    };

    explicit Impl(Store& state) : store(state), name(pipe_name()) {
        const auto identity = user_identity();
        const auto sid = identity.substr(0, identity.find(L'.'));
        const auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &security_descriptor, nullptr)) {
            throw io_error("Creating local pipe security");
        }
        security.nLength = sizeof(security);
        security.lpSecurityDescriptor = security_descriptor;
        security.bInheritHandle = FALSE;
    }

    ~Impl() {
        if (security_descriptor) {
            LocalFree(security_descriptor);
        }
    }

    Handle make_pipe(bool first) {
        return Handle(CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                              (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            connection_limit + 1, 65536, 65536, 0, &security));
    }

    bool connect(HANDLE pipe) {
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event.valid()) {
            throw io_error("Creating pipe connection event");
        }
        OVERLAPPED operation{};
        operation.hEvent = event.get();
        if (ConnectNamedPipe(pipe, &operation)) {
            return true;
        }
        const DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            return true;
        }
        if (error != ERROR_IO_PENDING) {
            throw io_error("Accepting local pipe connection", error);
        }
        wait_io(pipe, operation, stop_event.get(), INFINITE);
        return true;
    }

    Json dispatch(const Json& command) {
        if (!command.is_object() || !command.contains("op") || !command["op"].is_string()) {
            return {{"ok", false}, {"error", "A command must have a string op"}};
        }
        const auto operation = command["op"].get<std::string>();
        if (operation == "request.resolve") {
            return {{"ok", false}, {"error", "Only the Knock interface can answer requests"}};
        }
        if (operation == "ui.show") {
            const HWND window = FindWindowW(L"Knock.Tray.v1", nullptr);
            const bool shown = window && PostMessageW(window, WM_APP + 20, 0, 0);
            return shown ? Json{{"ok", true}}
                         : Json{{"ok", false}, {"error", "Knock interface is unavailable"}};
        }
        if (operation == "request.wait") {
            if (waiters.fetch_add(1) >= waiter_limit) {
                waiters.fetch_sub(1);
                return {{"ok", false}, {"error", "Too many waiting agents; maximum is 32"}};
            }
            struct WaiterGuard {
                std::atomic_uint& count;
                ~WaiterGuard() { count.fetch_sub(1); }
            } guard{waiters};
            return store.handle(command, false);
        }
        return store.handle(command, false);
    }

    void serve(Handle pipe) {
        try {
            const Json command = read_frame(pipe.get(), stop_event.get(), request_io_timeout);
            Json response;
            try {
                response = dispatch(command);
            } catch (const std::exception& error) {
                response = {{"ok", false}, {"error", error.what()}};
            }
            if (response.dump().size() > frame_limit) {
                response = {{"ok", false}, {"error", "Response exceeds the 1 MiB protocol limit"}};
            }
            write_frame(pipe.get(), response, stop_event.get(), request_io_timeout);
        } catch (const std::exception& error) {
            if (WaitForSingleObject(stop_event.get(), 0) != WAIT_OBJECT_0) {
                try {
                    write_frame(pipe.get(), {{"ok", false}, {"error", error.what()}},
                                stop_event.get(), 1000);
                } catch (...) {
                }
            }
        } catch (...) {
            // A malformed client must not terminate the tray process.
        }
        finish_connection(pipe.get(), stop_event.get());
    }

    void collect_finished() {
        for (auto it = workers.begin(); it != workers.end();) {
            if (it->finished->load()) {
                it->thread.join();
                it = workers.erase(it);
            } else {
                ++it;
            }
        }
    }

    void listen(Handle pipe) {
        while (WaitForSingleObject(stop_event.get(), 0) != WAIT_OBJECT_0) {
            collect_finished();
            if (!pipe.valid()) {
                pipe = make_pipe(false);
                if (!pipe.valid()) {
                    WaitForSingleObject(stop_event.get(), 100);
                    continue;
                }
            }
            try {
                connect(pipe.get());
                if (active.load() >= connection_limit) {
                    (void)read_frame(pipe.get(), stop_event.get(), 500);
                    write_frame(pipe.get(), {{"ok", false}, {"error", "Knock is busy; try again"}},
                                stop_event.get(), 500);
                    finish_connection(pipe.get(), stop_event.get(), 500);
                    pipe.reset();
                    continue;
                }
                auto finished = std::make_shared<std::atomic_bool>(false);
                // Allocate the vector slot before creating a joinable thread. A
                // failed allocation must not destroy a live std::thread.
                workers.push_back(Worker{std::thread{}, finished});
                active.fetch_add(1);
                try {
                    workers.back().thread =
                        std::thread([this, client = std::move(pipe), finished]() mutable {
                            serve(std::move(client));
                            active.fetch_sub(1);
                            finished->store(true);
                        });
                } catch (...) {
                    active.fetch_sub(1);
                    workers.pop_back();
                    throw;
                }
            } catch (...) {
                pipe.reset();
                if (WaitForSingleObject(stop_event.get(), 0) != WAIT_OBJECT_0) {
                    WaitForSingleObject(stop_event.get(), 50);
                }
            }
        }
    }

    Store& store;
    std::wstring name;
    PSECURITY_DESCRIPTOR security_descriptor = nullptr;
    SECURITY_ATTRIBUTES security{};
    Handle stop_event;
    std::thread listener;
    std::vector<Worker> workers;
    std::atomic_uint active{0};
    std::atomic_uint waiters{0};
    bool started = false;
};

PipeServer::PipeServer(Store& store) : impl_(new Impl(store)) {}

PipeServer::~PipeServer() {
    stop();
    delete impl_;
}

void PipeServer::start() {
    if (impl_->started) {
        return;
    }
    impl_->stop_event.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!impl_->stop_event.valid()) {
        throw io_error("Creating pipe shutdown event");
    }
    auto first = impl_->make_pipe(true);
    if (!first.valid()) {
        throw io_error("Starting Knock local pipe");
    }
    impl_->listener = std::thread([impl = impl_, pipe = std::move(first)]() mutable {
        impl->listen(std::move(pipe));
    });
    impl_->started = true;
}

void PipeServer::stop() {
    if (!impl_->started) {
        return;
    }
    SetEvent(impl_->stop_event.get());
    impl_->store.stop();
    if (impl_->listener.joinable()) {
        impl_->listener.join();
    }
    for (auto& worker : impl_->workers) {
        if (worker.thread.joinable()) {
            worker.thread.join();
        }
    }
    impl_->workers.clear();
    impl_->started = false;
}

} // namespace knock
