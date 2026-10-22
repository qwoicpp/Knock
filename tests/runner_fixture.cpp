#include <windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <iostream>
#include <string>

namespace {
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string output(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr);
    return output;
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) return 5;
    const std::wstring action(argv[1]);
    if (action == L"echo") {
        auto result = nlohmann::json::array();
        for (int index = 2; index < argc; ++index) result.push_back(utf8(argv[index]));
        std::cout << result.dump() << '\n';
        std::cerr << "stderr-captured\n";
        return 0;
    }
    if (action == L"flood") {
        std::string block(1024 * 1024, 'x');
        for (int index = 0; index < 17; ++index) std::cout << block;
        std::cout << "\nTAIL_SENTINEL\n";
        return 23;
    }
    if (action == L"wait") {
        Sleep(2500);
        std::cout << "finished-after-tray-stop\n";
        return 0;
    }
    return 5;
}
