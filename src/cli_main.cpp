#include "knock/platform.hpp"

#include <iostream>
#include <vector>

int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    try {
        if (!arguments.empty() && arguments.front() == L"_worker") {
            return knock::run_worker(arguments);
        }
        return knock::run_cli(arguments);
    } catch (const std::exception& error) {
        const knock::Json result{{"ok", false}, {"error", error.what()}};
        std::cout << result.dump(-1, ' ', true, knock::Json::error_handler_t::replace) << '\n';
        return 5;
    }
}
