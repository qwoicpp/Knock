#include "knock/runner.hpp"
#include "knock/agent_info.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace knock {
namespace {

constexpr std::string_view help_text = R"(Knock — a local inbox for your agents

  knock info                         Agent guide and token-saving workflow
  knock open
  knock status
  knock request --chat AUDIO-1 --message "May I use mouse and keyboard?"
      [--kind input|question] [--timeout 1800] [--lease 300] [--detach]
  knock result REQUEST_ID [--wait SECONDS]
  knock release REQUEST_ID
  knock cancel REQUEST_ID
  knock notify --chat AUDIO-1 --message "Build finished" [--resume-text TEXT]
  knock run --chat AUDIO-1 [--title "Build"] [--cwd DIRECTORY] -- PROGRAM ARGS...
  knock job JOB_ID
  knock jobs [--offset 0] [--limit 50]
  knock tasks list [--offset 0] [--limit 50]
  knock tasks add --title TEXT [--body TEXT] [--operator NAME] [--project NAME]
  knock tasks update ID [--title TEXT] [--body TEXT] [--status todo|in_progress|done]
      [--operator NAME] [--project NAME]
  knock tasks remove ID
  knock notes list [--offset 0] [--limit 50]
  knock notes get ID [--offset 0] [--limit 100]
  knock notes add --title TEXT [--body TEXT] [--tag LABEL ...]
  knock notes update ID [--title TEXT] [--body TEXT] [--tag LABEL ... | --clear-tags]
  knock notes append ID --body TEXT
  knock notes remove ID

Text can also come from UTF-8 files: --message-file, --body-file, --resume-file.
Repeat --tag for multiple note labels; supplying tags replaces the selection.
Notes are personal conversations. Add/update --body edits the description;
append adds a message. Get returns a bounded, oldest-first message page.
Help and info print text; other commands return one UTF-8 JSON object.
A request waits without model polling;
--detach returns its ID immediately. A run always detaches and notifies on finish.
Programs are executed directly. For shell syntax, explicitly use cmd.exe or
powershell.exe after --. No shell is added by Knock.

Request exit codes: 0 approved, 2 denied, 3 custom, 4 expired/released,
5 error, 6 cancelled, 7 still pending. Only 0 with a live input lease permits input.
Other successful commands return 0. A custom reply is never an approval.
Run knock info once per agent session for the full usage guide.
get-info is an alias for info.
)";

struct Arguments {
    std::map<std::wstring, std::wstring> options;
    std::vector<std::wstring> positional;
    std::vector<std::wstring> command;
    std::vector<std::wstring> tags;
    bool detach = false;
    bool clear_tags = false;

    Arguments(const std::vector<std::wstring>& arguments, std::size_t begin) {
        for (std::size_t index = begin; index < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            if (argument == L"--") {
                command.assign(arguments.begin() + static_cast<std::ptrdiff_t>(index + 1), arguments.end());
                break;
            }
            if (argument == L"--detach") {
                if (detach) throw std::runtime_error("Duplicate option --detach");
                detach = true;
            } else if (argument == L"--clear-tags") {
                if (clear_tags) throw std::runtime_error("Duplicate option --clear-tags");
                clear_tags = true;
            } else if (argument == L"--tag") {
                if (index + 1 == arguments.size() || arguments[index + 1].starts_with(L"--")) {
                    throw std::runtime_error("Missing value for --tag");
                }
                tags.push_back(arguments[++index]);
            } else if (argument.starts_with(L"--")) {
                if (index + 1 == arguments.size() || arguments[index + 1] == L"--") {
                    throw std::runtime_error("Missing value for " + narrow(argument));
                }
                if (!options.emplace(argument, arguments[index + 1]).second) {
                    throw std::runtime_error("Duplicate option " + narrow(argument));
                }
                ++index;
            } else {
                positional.push_back(argument);
            }
        }
    }

    void allow(std::initializer_list<std::wstring_view> names, bool allow_detach = false,
               bool allow_command = false, std::size_t positions = 0) const {
        for (const auto& [name, value] : options) {
            (void)value;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                throw std::runtime_error("Unknown option " + narrow(name));
            }
        }
        if (!tags.empty() && std::find(names.begin(), names.end(), L"--tag") == names.end())
            throw std::runtime_error("--tag is only valid for notes add or update");
        if (clear_tags && std::find(names.begin(), names.end(), L"--clear-tags") == names.end())
            throw std::runtime_error("--clear-tags is only valid for notes update");
        if (detach && !allow_detach) throw std::runtime_error("--detach is only valid for request");
        if (!command.empty() && !allow_command) throw std::runtime_error("Unexpected command after --");
        if (positional.size() != positions) throw std::runtime_error("Wrong number of positional arguments; use knock help");
    }

    bool has(std::wstring_view name) const { return options.contains(std::wstring(name)); }

    std::wstring wide(std::wstring_view name, std::wstring fallback = L"") const {
        const auto found = options.find(std::wstring(name));
        return found == options.end() ? std::move(fallback) : found->second;
    }

    std::string text(std::wstring_view name, std::string fallback = "") const {
        return has(name) ? narrow(wide(name)) : std::move(fallback);
    }

    int number(std::wstring_view name, int fallback, int minimum, int maximum) const {
        if (!has(name)) return fallback;
        const auto value = text(name);
        int result = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
        if (error != std::errc{} || end != value.data() + value.size() || result < minimum || result > maximum) {
            throw std::runtime_error(narrow(name) + " must be between " + std::to_string(minimum) + " and " + std::to_string(maximum));
        }
        return result;
    }
};

std::string read_text_file(const std::wstring& name) {
    std::ifstream file(std::filesystem::path(name), std::ios::binary);
    if (!file) throw std::runtime_error("Cannot open text file: " + narrow(name));
    std::string text(65537, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (file.bad()) throw std::runtime_error("Could not read text file: " + narrow(name));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text.size() > 65536) throw std::runtime_error("Text files must be no larger than 64 KiB");
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    (void)widen(text); // Reject invalid UTF-8 before sending any request.
    return text;
}

std::string text_option(const Arguments& arguments, std::wstring_view name, std::wstring_view file_name,
                        bool required = false) {
    if (arguments.has(name) && arguments.has(file_name)) {
        throw std::runtime_error("Choose " + narrow(name) + " or " + narrow(file_name) + ", not both");
    }
    auto result = arguments.has(file_name) ? read_text_file(arguments.wide(file_name)) : arguments.text(name);
    if (required && result.empty()) throw std::runtime_error(narrow(name) + " is required");
    return result;
}

void require_text(std::string_view value, std::string_view name, std::size_t limit) {
    if (value.empty() || value.size() > limit || value.find('\0') != std::string_view::npos) {
        throw std::runtime_error(std::string(name) + " must contain 1 to " + std::to_string(limit) + " UTF-8 bytes and no NUL characters");
    }
}

int output(const Json& result, bool decision = false) {
    // ASCII JSON escapes survive legacy PowerShell's redirected-output code page
    // while retaining every Unicode code point after JSON decoding.
    std::cout << result.dump(-1, ' ', true, Json::error_handler_t::replace) << '\n';
    if (!result.value("ok", false)) return 5;
    if (!decision) return 0;
    const auto& request = result.at("request");
    const auto status = request.value("status", "pending");
    if (status == "approved") {
        if (request.value("kind", "input") == "input" && request.value("lease_until", std::int64_t{0}) <= now_ms()) return 4;
        return 0;
    }
    if (status == "denied") return 2;
    if (status == "custom") return 3;
    if (status == "expired" || status == "released") return 4;
    if (status == "cancelled") return 6;
    return 7;
}

Json list_items(std::string_view key, const Arguments& arguments) {
    auto result = send_command({{"op", "list"}, {"collection", key},
                               {"offset", arguments.number(L"--offset", 0, 0, 1000000)},
                               {"limit", arguments.number(L"--limit", 50, 1, 100)},
                               {"summaries", key == "jobs"}});
    if (!result.value("ok", false)) return result;
    return {{"ok", true}, {std::string(key), result.at("items")},
            {"total", result.at("total")}, {"next_offset", result.at("next_offset")}};
}

int edit_collection(const std::vector<std::wstring>& arguments, bool tasks) {
    if (arguments.size() < 2) throw std::runtime_error(tasks ?
        "Specify list, add, update, or remove" : "Specify list, get, add, update, append, or remove");
    const auto action = arguments[1];
    const Arguments parsed(arguments, 2);
    const std::string singular = tasks ? "task" : "note";
    if (action == L"list") {
        parsed.allow({L"--offset", L"--limit"});
        return output(list_items(tasks ? "tasks" : "notes", parsed));
    }
    if (action == L"remove") {
        parsed.allow({}, false, false, 1);
        return output(send_command({{"op", singular + ".delete"}, {"id", narrow(parsed.positional[0])}}));
    }
    if (!tasks && action == L"get") {
        parsed.allow({L"--offset", L"--limit"}, false, false, 1);
        return output(send_command({{"op", "note.get"}, {"id", narrow(parsed.positional[0])},
            {"offset", parsed.number(L"--offset", 0, 0, 1000000)},
            {"limit", parsed.number(L"--limit", 100, 1, 100)}}));
    }
    if (!tasks && action == L"append") {
        parsed.allow({L"--body", L"--body-file"}, false, false, 1);
        const auto text = text_option(parsed, L"--body", L"--body-file", true);
        require_text(text, "--body", 64 * 1024);
        return output(send_command({{"op", "note.append"}, {"id", narrow(parsed.positional[0])}, {"text", text}}));
    }
    if (action != L"add" && action != L"update") throw std::runtime_error("Unknown " + singular + " action");
    const bool add = action == L"add";
    if (tasks) {
        parsed.allow({L"--title", L"--body", L"--body-file", L"--operator", L"--project", L"--status"}, false, false, add ? 0 : 1);
    } else {
        parsed.allow({L"--title", L"--body", L"--body-file", L"--tag", L"--clear-tags"}, false, false, add ? 0 : 1);
        if (add && parsed.clear_tags) throw std::runtime_error("--clear-tags is only valid for notes update");
        if (parsed.clear_tags && !parsed.tags.empty()) throw std::runtime_error("Choose --tag or --clear-tags, not both");
    }
    Json command{{"op", singular + (add ? ".add" : ".update")}};
    if (!add) command["id"] = narrow(parsed.positional[0]);
    if (add || parsed.has(L"--title")) {
        const auto title = parsed.text(L"--title");
        require_text(title, "--title", 160);
        command["title"] = title;
    }
    if (parsed.has(L"--body") || parsed.has(L"--body-file")) {
        command["body"] = text_option(parsed, L"--body", L"--body-file");
    }
    if (tasks) {
        for (const auto* field : {L"operator", L"project", L"status"}) {
            const auto option = L"--" + std::wstring(field);
            if (parsed.has(option)) command[narrow(field)] = parsed.text(option);
        }
        if (add && parsed.has(L"--status")) throw std::runtime_error("New tasks start as todo; change status with tasks update");
    } else if (parsed.clear_tags || !parsed.tags.empty()) {
        command["tags"] = Json::array();
        for (const auto& tag : parsed.tags) command["tags"].push_back(narrow(tag));
    }
    if (!add && command.size() == 2) throw std::runtime_error("No fields to update");
    return output(send_command(command));
}

} // namespace

int run_cli(const std::vector<std::wstring>& arguments) {
    if (arguments.empty()) {
        std::cout << help_text;
        return 0;
    }
    const auto& action = arguments[0];
    if (action == L"help" || action == L"--help" || action == L"-h" ||
        action == L"info" || action == L"get-info") {
        if (arguments.size() != 1)
            throw std::runtime_error("Help and info take no arguments; use knock help or knock info");
        std::cout << (action == L"info" || action == L"get-info" ? agent_info : help_text);
        return 0;
    }
    if (action == L"tasks" || action == L"notes") return edit_collection(arguments, action == L"tasks");
    const Arguments parsed(arguments, 1);

    if (action == L"open") {
        parsed.allow({});
        const bool started = start_tray(true);
        return output(started ? Json{{"ok", true}} : Json{{"ok", false}, {"error", "Could not open KnockTray.exe"}});
    }
    if (action == L"status") {
        parsed.allow({});
        return output(send_command({{"op", "status"}}));
    }
    if (action == L"request") {
        parsed.allow({L"--chat", L"--message", L"--message-file", L"--kind", L"--timeout", L"--lease"}, true);
        const auto chat = parsed.text(L"--chat");
        const auto message = text_option(parsed, L"--message", L"--message-file", true);
        require_text(chat, "--chat", 64);
        require_text(message, "--message", 16000);
        const auto kind = parsed.text(L"--kind", "input");
        if (kind != "input" && kind != "question") throw std::runtime_error("--kind must be input or question");
        const int timeout = parsed.number(L"--timeout", 1800, 1, 86400);
        const auto result = send_command({{"op", "request.create"}, {"chat", chat}, {"message", message},
                                          {"kind", kind}, {"timeout_seconds", timeout},
                                          {"lease_seconds", parsed.number(L"--lease", 300, 1, 3600)}});
        if (parsed.detach || !result.value("ok", false)) return output(result);
        const auto id = result.at("request").at("id").get<std::string>();
        auto answer = send_command({{"op", "request.wait"}, {"id", id}, {"wait_seconds", std::min(86400, timeout + 1)}});
        if (!answer.value("ok", false)) answer["request_id"] = id;
        return output(answer, true);
    }
    if (action == L"result") {
        parsed.allow({L"--wait"}, false, false, 1);
        Json command{{"op", parsed.has(L"--wait") ? "request.wait" : "request.result"},
                     {"id", narrow(parsed.positional[0])}};
        if (parsed.has(L"--wait")) command["wait_seconds"] = parsed.number(L"--wait", 1, 1, 86400);
        return output(send_command(command), true);
    }
    if (action == L"release" || action == L"cancel") {
        parsed.allow({}, false, false, 1);
        return output(send_command({{"op", action == L"release" ? "request.release" : "request.cancel"},
                                    {"id", narrow(parsed.positional[0])}}));
    }
    if (action == L"notify") {
        parsed.allow({L"--chat", L"--message", L"--message-file", L"--resume-text", L"--resume-file"});
        const auto chat = parsed.text(L"--chat");
        const auto message = text_option(parsed, L"--message", L"--message-file", true);
        require_text(chat, "--chat", 64);
        require_text(message, "--message", 16000);
        return output(send_command({{"op", "notify"}, {"chat", chat}, {"message", message},
                                    {"resume_text", text_option(parsed, L"--resume-text", L"--resume-file")}}));
    }
    if (action == L"run") {
        parsed.allow({L"--chat", L"--title", L"--cwd"}, false, true);
        if (parsed.command.empty()) throw std::runtime_error("run requires -- followed by a program and its arguments");
        RunOptions options;
        options.chat = parsed.text(L"--chat");
        options.title = parsed.text(L"--title", narrow(std::filesystem::path(parsed.command[0]).filename().wstring()));
        options.directory = parsed.has(L"--cwd") ? std::filesystem::absolute(parsed.wide(L"--cwd")) : std::filesystem::current_path();
        options.command = parsed.command;
        require_text(options.chat, "--chat", 64);
        require_text(options.title, "--title", 160);
        if (!std::filesystem::is_directory(options.directory)) throw std::runtime_error("--cwd is not an existing directory");
        return output(start_job(options));
    }
    if (action == L"jobs") {
        parsed.allow({L"--offset", L"--limit"});
        return output(list_items("jobs", parsed));
    }
    if (action == L"job") {
        parsed.allow({}, false, false, 1);
        return output(send_command({{"op", "job.result"}, {"id", narrow(parsed.positional[0])}}));
    }
    throw std::runtime_error("Unknown command; use knock help");
}
} // namespace knock
