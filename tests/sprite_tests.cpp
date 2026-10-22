#include "knock/sprite.hpp"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool same(RECT left, RECT right) {
    return left.left == right.left && left.top == right.top &&
           left.right == right.right && left.bottom == right.bottom;
}

std::filesystem::path assets_directory() {
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    require(length && length < 32768, "Could not find the sprite test executable");
    return std::filesystem::path(executable).parent_path() / L"assets";
}

struct Canvas {
    static constexpr int width = 216;
    static constexpr int height = 206;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ previous = nullptr;
    std::uint32_t* pixels = nullptr;

    Canvas() {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        require(dc != nullptr, "Could not create a sprite test DC");
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS,
                                  reinterpret_cast<void**>(&pixels), nullptr, 0);
        require(bitmap && pixels, "Could not create a sprite test bitmap");
        previous = SelectObject(dc, bitmap);
    }
    ~Canvas() {
        if (previous) SelectObject(dc, previous);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    std::uint64_t render(const knock::Sprite& sprite) {
        std::fill(pixels, pixels + width * height, 0x00090909u);
        sprite.draw(dc, RECT{0, 0, width, height});
        GdiFlush();
        std::uint64_t hash = 1469598103934665603ull;
        bool painted = false;
        for (int i = 0; i < width * height; ++i) {
            const auto color = pixels[i] & 0x00FFFFFFu;
            painted = painted || color != 0x00090909u;
            hash ^= color;
            hash *= 1099511628211ull;
        }
        require(painted, "Sprite did not draw visible pixels");
        return hash;
    }
};

void check_animation(const std::filesystem::path& assets, const wchar_t* name,
                     unsigned int frames, RECT bounds) {
    knock::Sprite sprite;
    Canvas canvas;
    require(sprite.load(assets, name), "Bundled GIF did not decode");
    require(sprite.animated(), "Bundled GIF was not recognized as animated");
    require(same(sprite.content_bounds(), bounds), "Animated crop differs from the bundled GIF's visible bounds");
    const auto first = canvas.render(sprite);
    require(!sprite.advance(100000), "Sprite advanced before being shown");

    sprite.resume(1000);
    sprite.resume(1050);
    require(!sprite.advance(1099), "First 100 ms GIF frame advanced too early");
    require(sprite.advance(1100), "Repeated resume reset the first frame deadline");
    const auto second = canvas.render(sprite);
    sprite.pause();
    require(!sprite.advance(90000), "Hidden sprite continued animating");
    require(canvas.render(sprite) == second, "Hidden sprite changed its pixels");
    sprite.resume(100000);
    require(!sprite.advance(100199), "Resume did not preserve the current frame's duration");
    require(sprite.advance(100200), "Resume failed to continue the GIF");

    require(sprite.load(assets, name), "Could not reload bundled GIF");
    sprite.resume(1000);
    std::set<std::uint64_t> appearances{canvas.render(sprite)};
    std::uint64_t deadline = 1000;
    for (unsigned int i = 0; i < frames; ++i) {
        deadline += i == 0 ? 100 : 200;
        require(sprite.advance(deadline), "GIF missed a frame deadline");
        appearances.insert(canvas.render(sprite));
        require(same(sprite.content_bounds(), bounds), "Frame changed the fixed crop");
    }
    require(appearances.size() >= 2, "Decoded GIF frames never changed visually");
    require(canvas.render(sprite) == first, "GIF did not loop back to its first frame");

    const auto cycle = 100 + static_cast<std::uint64_t>(frames - 1) * 200;
    require(sprite.advance(deadline + cycle * 10000 + 100), "Late timer did not advance");
    require(canvas.render(sprite) == second, "Late timer did not preserve GIF timing");
    require(!sprite.load(assets / L"missing-overrides", name), "Missing override unexpectedly loaded");
    require(canvas.render(sprite) == second, "Missing override discarded the shipped GIF");
}

void check_png_fallback(const std::filesystem::path& assets) {
    wchar_t temporary[32768]{};
    require(GetTempPathW(32768, temporary) != 0, "Could not find a temporary test directory");
    const auto directory = std::filesystem::path(temporary) /
        (L"knock-sprite-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
    } cleanup{directory};
    std::ofstream(directory / L"request.gif", std::ios::binary) << "invalid image";
    std::filesystem::copy_file(assets / L"request.png", directory / L"request.png",
                                std::filesystem::copy_options::overwrite_existing);
    knock::Sprite sprite;
    Canvas canvas;
    require(sprite.load(assets, L"request") && sprite.animated(), "Initial GIF did not load");
    require(sprite.load(directory, L"request"), "Invalid GIF did not fall back to valid PNG");
    require(!sprite.animated(), "PNG override retained the old animation frames");
    require(same(sprite.content_bounds(), RECT{1, 5, 80, 57}), "PNG bounds were not recalculated");
    canvas.render(sprite);
    sprite.resume(0);
    require(!sprite.advance(100000), "Static PNG attempted to animate");
}

} // namespace

int main() {
    Gdiplus::GdiplusStartupInput options;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &options, nullptr) != Gdiplus::Ok) return 1;
    int result = 0;
    try {
        const auto assets = assets_directory();
        check_animation(assets, L"request", 4, RECT{40, 146, 445, 380});
        check_animation(assets, L"complete", 13, RECT{79, 45, 383, 428});
        check_animation(assets, L"note", 8, RECT{81, 83, 480, 340});
        check_png_fallback(assets);
        std::cout << "PASS bundled GIF decoding, fixed crops, pixel changes, exact timing, pause/resume, loops, and PNG fallback\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        result = 1;
    }
    Gdiplus::GdiplusShutdown(token);
    return result;
}
