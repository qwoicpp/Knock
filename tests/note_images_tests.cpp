#include "knock/note_images.hpp"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<unsigned char>;
constexpr CLSID png_encoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Fixture {
    HWND window = nullptr;
    std::filesystem::path directory;
    Fixture() {
        require(!FindWindowW(L"Knock.Tray.v1", nullptr), "Close Knock before running the clipboard test.");
        window = CreateWindowExW(0, L"STATIC", L"Knock image tests", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Could not create clipboard owner");
        wchar_t temporary[MAX_PATH]{};
        require(GetTempPathW(MAX_PATH, temporary) != 0, "Could not find temporary directory");
        directory = std::filesystem::path(temporary) / (L"knock-image-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        std::filesystem::create_directories(directory);
    }
    ~Fixture() {
        if (OpenClipboard(window)) { EmptyClipboard(); CloseClipboard(); }
        if (window) DestroyWindow(window);
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void publish(UINT format, const Bytes& bytes) {
        require(OpenClipboard(window) != 0, "Could not open test clipboard");
        struct Close { ~Close() { CloseClipboard(); } } close;
        require(EmptyClipboard() != 0, "Could not empty test clipboard");
        HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
        require(handle != nullptr, "Could not allocate test clipboard");
        void* data = GlobalLock(handle);
        require(data != nullptr, "Could not lock test clipboard");
        std::memcpy(data, bytes.data(), bytes.size());
        GlobalUnlock(handle);
        if (!SetClipboardData(format, handle)) { GlobalFree(handle); throw std::runtime_error("Could not publish test clipboard"); }
    }
    knock::ClipboardImageResult paste() { return knock::paste_note_image(window, directory); }
};

Bytes read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "Could not read test file");
    return Bytes(std::istreambuf_iterator<char>(input), {});
}

std::filesystem::path saved_path(const Fixture& fixture, const knock::ClipboardImageResult& result) {
    require(result.status == knock::ClipboardImageStatus::imported, "Clipboard image was not imported");
    require(result.filename.size() == 36 && result.filename.substr(32) == ".png", "Generated image name is invalid");
    for (char value : result.filename.substr(0, 32))
        require((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'), "Image name is not hexadecimal");
    const auto path = fixture.directory / L"note-images" / std::wstring(result.filename.begin(), result.filename.end());
    require(std::filesystem::is_regular_file(path), "Imported image is missing");
    return path;
}

void require_pixel(const std::filesystem::path& path, unsigned int x, unsigned int y,
                   unsigned int red, unsigned int green, unsigned int blue, unsigned int alpha = 255) {
    Gdiplus::Bitmap bitmap(path.c_str());
    require(bitmap.GetLastStatus() == Gdiplus::Ok, "Saved PNG could not be decoded");
    Gdiplus::Color pixel;
    require(bitmap.GetPixel(x, y, &pixel) == Gdiplus::Ok, "Could not read saved pixel");
    auto close_colour = [](unsigned int actual, unsigned int expected) {
        return actual > expected ? actual - expected <= 1 : expected - actual <= 1;
    };
    if (!(close_colour(pixel.GetR(), red) && close_colour(pixel.GetG(), green) && close_colour(pixel.GetB(), blue) && close_colour(pixel.GetA(), alpha))) {
        std::cerr << "Pixel " << x << "," << y << " = " << unsigned(pixel.GetR()) << "," << unsigned(pixel.GetG()) << "," << unsigned(pixel.GetB()) << "," << unsigned(pixel.GetA()) << '\n';
        throw std::runtime_error("Saved PNG pixel changed");
    }
}

Bytes dib24() {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header); header.biWidth = 2; header.biHeight = 2;
    header.biPlanes = 1; header.biBitCount = 24; header.biCompression = BI_RGB;
    Bytes bytes(sizeof(header) + 16);
    std::memcpy(bytes.data(), &header, sizeof(header));
    const std::array<unsigned char, 16> pixels{
        255, 0, 0, 255, 255, 255, 0, 0, // Bottom: blue, white.
        0, 0, 255, 0, 255, 0, 0, 0    // Top: red, green, with row padding.
    };
    std::copy(pixels.begin(), pixels.end(), bytes.begin() + sizeof(header));
    return bytes;
}

void check_dib(Fixture& fixture) {
    fixture.publish(CF_DIB, dib24());
    const DWORD sequence = GetClipboardSequenceNumber();
    auto result = fixture.paste();
    const auto path = saved_path(fixture, result);
    require(GetClipboardSequenceNumber() == sequence, "Import changed the clipboard contents");
    require_pixel(path, 0, 0, 255, 0, 0);
    require_pixel(path, 1, 0, 0, 255, 0);
    require_pixel(path, 0, 1, 0, 0, 255);
    require_pixel(path, 1, 1, 255, 255, 255);
    knock::NoteImage preview;
    require(preview.load(fixture.directory, result.filename), "Saved image did not reload");
    require(preview.width() == 2 && preview.height() == 2, "Reloaded dimensions changed");
    require(knock::remove_note_image(fixture.directory, result.filename), "Could not remove pending image");
    require(!std::filesystem::exists(path), "Pending image was not removed");
    require(!preview.load(fixture.directory, result.filename), "Removed image still loaded");
    require(preview.width() == 0 && preview.height() == 0, "Failed load kept stale dimensions");
}

void check_dibv5(Fixture& fixture) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header); header.bV5Width = 2; header.bV5Height = -1;
    header.bV5Planes = 1; header.bV5BitCount = 32; header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0xff0000; header.bV5GreenMask = 0xff00; header.bV5BlueMask = 0xff;
    header.bV5AlphaMask = 0xff000000; header.bV5CSType = 0x73524742; // LCS_sRGB.
    Bytes bytes(sizeof(header) + 8);
    std::memcpy(bytes.data(), &header, sizeof(header));
    const std::array<unsigned char, 8> pixels{0, 0, 255, 128, 0, 255, 0, 255};
    std::copy(pixels.begin(), pixels.end(), bytes.begin() + sizeof(header));
    fixture.publish(CF_DIBV5, bytes);
    auto result = fixture.paste();
    const auto path = saved_path(fixture, result);
    require_pixel(path, 0, 0, 255, 0, 0, 128);
    require_pixel(path, 1, 0, 0, 255, 0, 255);
}

void check_png(Fixture& fixture) {
    const auto input = fixture.directory / L"source.png";
    {
        Gdiplus::Bitmap bitmap(4, 2, PixelFormat32bppARGB);
        require(bitmap.SetPixel(0, 0, Gdiplus::Color(128, 255, 0, 0)) == Gdiplus::Ok, "Could not create PNG fixture");
        require(bitmap.SetPixel(1, 0, Gdiplus::Color(255, 0, 255, 0)) == Gdiplus::Ok, "Could not create PNG fixture");
        require(bitmap.Save(input.c_str(), &png_encoder, nullptr) == Gdiplus::Ok, "Could not save PNG fixture");
    }
    fixture.publish(RegisterClipboardFormatW(L"PNG"), read_file(input));
    const auto result = fixture.paste();
    const auto path = saved_path(fixture, result);
    require_pixel(path, 0, 0, 255, 0, 0, 128);
    require_pixel(path, 1, 0, 0, 255, 0);
    // Load into a fresh object after all clipboard bytes have been replaced.
    const std::wstring text = L"Normal text paste";
    Bytes text_bytes((text.size() + 1) * sizeof(wchar_t));
    std::memcpy(text_bytes.data(), text.c_str(), text_bytes.size());
    fixture.publish(CF_UNICODETEXT, text_bytes);
    require(fixture.paste().status == knock::ClipboardImageStatus::no_image, "Text paste was intercepted");
    knock::NoteImage reloaded;
    require(reloaded.load(fixture.directory, result.filename), "Persistent PNG depends on the clipboard");
    require(reloaded.width() == 4 && reloaded.height() == 2, "PNG dimensions changed");

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 60; info.bmiHeader.biHeight = -60;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    std::uint32_t* pixels = nullptr;
    HBITMAP target = CreateDIBSection(dc, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(dc && target && pixels, "Could not create thumbnail target");
    const auto old = SelectObject(dc, target);
    std::fill(pixels, pixels + 60 * 60, 0x00090909u);
    reloaded.draw(dc, RECT{0, 0, 60, 60});
    GdiFlush();
    require(std::all_of(pixels, pixels + 60 * 10, [](std::uint32_t pixel) { return (pixel & 0xffffff) == 0x090909; }),
            "Thumbnail ignored aspect ratio");
    require(std::any_of(pixels + 60 * 15, pixels + 60 * 45, [](std::uint32_t pixel) { return (pixel & 0xffffff) != 0x090909; }),
            "Thumbnail drew no pixels");
    SelectObject(dc, old); DeleteObject(target); DeleteDC(dc);
}

void check_bitmap(Fixture& fixture) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 2; info.bmiHeader.biHeight = -1;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    std::uint32_t* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, reinterpret_cast<void**>(&pixels), nullptr, 0);
    require(bitmap && pixels, "Could not create CF_BITMAP fixture");
    pixels[0] = 0x00ff0000; pixels[1] = 0x0000ff00;
    require(OpenClipboard(fixture.window) != 0, "Could not open bitmap clipboard");
    require(EmptyClipboard() != 0, "Could not empty bitmap clipboard");
    require(SetClipboardData(CF_BITMAP, bitmap) != nullptr, "Could not publish CF_BITMAP");
    CloseClipboard(); // Clipboard owns the HBITMAP.
    const auto result = fixture.paste();
    require_pixel(saved_path(fixture, result), 0, 0, 255, 0, 0);
}

void check_rejections(Fixture& fixture) {
    fixture.publish(RegisterClipboardFormatW(L"PNG"), Bytes{1, 2, 3, 4, 5});
    auto result = fixture.paste();
    require(result.status == knock::ClipboardImageStatus::error && !result.error.empty(), "Corrupt PNG was accepted");

    auto truncated = dib24(); truncated.resize(40);
    fixture.publish(CF_DIB, truncated);
    require(fixture.paste().status == knock::ClipboardImageStatus::error, "Truncated DIB was accepted");
    auto oversized = dib24();
    LONG width = 9000;
    std::memcpy(oversized.data() + 4, &width, sizeof(width));
    fixture.publish(CF_DIB, oversized);
    require(fixture.paste().status == knock::ClipboardImageStatus::error, "Oversized DIB was accepted");

    for (const auto* name : {"../escape.png", "..\\escape.png", "/escape.png", "C:\\escape.png", "a.png:stream", ".png", "a/b.png", "a.jpg"}) {
        require(!knock::valid_note_image_name(name), "Unsafe image name was accepted");
        require(!knock::remove_note_image(fixture.directory, name), "Unsafe image deletion was accepted");
        knock::NoteImage image;
        require(!image.load(fixture.directory, name), "Unsafe image load was accepted");
    }
    require(knock::valid_note_image_name("0123-a_B.png"), "Safe image name was rejected");
    const auto missing_dir = fixture.directory / L"not-a-directory";
    { std::ofstream file(missing_dir); file << "file"; }
    fixture.publish(CF_DIB, dib24());
    result = knock::paste_note_image(fixture.window, missing_dir);
    require(result.status == knock::ClipboardImageStatus::error && result.filename.empty(), "Failed disk save reported success");
}

} // namespace

int main() {
    Gdiplus::GdiplusStartupInput options;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &options, nullptr) != Gdiplus::Ok) return 1;
    int code = 0;
    try {
        Fixture fixture;
        check_dib(fixture); std::cout << "PASS DIB row order, padding, opaque colour, reload, removal\n";
        check_dibv5(fixture); std::cout << "PASS DIBV5 transparency and top-down pixels\n";
        check_png(fixture); std::cout << "PASS PNG transparency, clipboard independence, text fallback, thumbnail\n";
        check_bitmap(fixture); std::cout << "PASS CF_BITMAP compatibility\n";
        check_rejections(fixture); std::cout << "PASS corruption, size, path and save failure checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; code = 1;
    }
    Gdiplus::GdiplusShutdown(token);
    return code;
}
