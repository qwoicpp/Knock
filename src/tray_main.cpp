#include "knock/ui.hpp"
#include <shellapi.h>
#include <string_view>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool show = true;
    for (int i = 1; args && i < count; ++i) {
        if (std::wstring_view(args[i]) == L"--background") show = false;
        if (std::wstring_view(args[i]) == L"--show") show = true;
    }
    LocalFree(args);
    return knock::run_tray(instance, show);
}
