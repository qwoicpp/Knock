#include "knock/note_images.hpp"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace knock {
namespace {

constexpr std::size_t maximum_file_bytes = 32 * 1024 * 1024;
constexpr std::size_t maximum_clipboard_bytes = 68 * 1024 * 1024;
constexpr unsigned int maximum_dimension = 8192;
constexpr std::uint64_t maximum_pixels = 16 * 1024 * 1024;
constexpr unsigned int preview_dimension = 1024;
constexpr DWORD alpha_bitfields = 6;
constexpr DWORD embedded_profile = 0x4d424544; // PROFILE_EMBEDDED (MBED).
constexpr DWORD linked_profile = 0x4c494e4b;   // PROFILE_LINKED (LINK).
constexpr CLSID png_encoder = {0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};

using Bytes = std::vector<unsigned char>;
using Bitmap = Gdiplus::Bitmap;
struct ReleaseStream { void operator()(IStream* value) const { if (value) value->Release(); } };
using Stream = std::unique_ptr<IStream, ReleaseStream>;

[[noreturn]] void invalid(const char* message) { throw std::runtime_error(message); }

void check_dimensions(unsigned int width, unsigned int height) {
    if (!width || !height || width > maximum_dimension || height > maximum_dimension ||
        static_cast<std::uint64_t>(width) * height > maximum_pixels)
        invalid("Image is too large. Use at most 8192 pixels per side and 16 megapixels.");
}

std::filesystem::path image_path(const std::filesystem::path& directory, std::string_view name) {
    if (!valid_note_image_name(name)) invalid("Invalid image filename.");
    return directory / L"note-images" / std::wstring(name.begin(), name.end());
}

template<class T> T read_at(const Bytes& bytes, std::size_t position) {
    if (position > bytes.size() || bytes.size() - position < sizeof(T)) invalid("Incomplete clipboard image.");
    T value{};
    std::memcpy(&value, bytes.data() + position, sizeof(T));
    return value;
}

Bytes clipboard_bytes(UINT format, std::size_t limit) {
    const HANDLE handle = GetClipboardData(format);
    if (!handle) invalid("Could not read the clipboard image. Copy it again and retry.");
    const SIZE_T size = GlobalSize(handle);
    if (!size || size > limit) invalid("Clipboard image is too large or incomplete.");
    const auto* pointer = static_cast<const unsigned char*>(GlobalLock(handle));
    if (!pointer) invalid("Could not lock the clipboard image.");
    struct Unlock { HANDLE value; ~Unlock() { GlobalUnlock(value); } } unlock{handle};
    return {pointer, pointer + size};
}

Stream memory_stream(const Bytes& bytes) {
    IStream* raw = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &raw) != S_OK) invalid("Could not read the image.");
    Stream stream(raw);
    ULONG written = 0;
    if (stream->Write(bytes.data(), static_cast<ULONG>(bytes.size()), &written) != S_OK || written != bytes.size())
        invalid("Could not read the image.");
    LARGE_INTEGER start{};
    if (stream->Seek(start, STREAM_SEEK_SET, nullptr) != S_OK) invalid("Could not read the image.");
    return stream;
}

std::unique_ptr<Bitmap> copy_bitmap(Bitmap& source, unsigned int width, unsigned int height) {
    auto result = std::make_unique<Bitmap>(static_cast<INT>(width), static_cast<INT>(height), PixelFormat32bppARGB);
    if (result->GetLastStatus() != Gdiplus::Ok) invalid("Could not allocate the image.");
    Gdiplus::Graphics graphics(result.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    if (graphics.DrawImage(&source, Gdiplus::Rect(0, 0, static_cast<INT>(width), static_cast<INT>(height)),
                           0, 0, static_cast<INT>(source.GetWidth()), static_cast<INT>(source.GetHeight()),
                           Gdiplus::UnitPixel) != Gdiplus::Ok)
        invalid("This image could not be decoded.");
    return result;
}

std::unique_ptr<Bitmap> decode_png(const Bytes& bytes, bool preview,
                                 unsigned int* source_width = nullptr, unsigned int* source_height = nullptr) {
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    if (bytes.size() < 33 || bytes.size() > maximum_file_bytes ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        std::memcmp(bytes.data() + 12, "IHDR", 4) != 0)
        invalid("This clipboard image is not a valid PNG.");
    auto big_endian = [&](std::size_t offset) {
        return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
               (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
               (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
    };
    if (big_endian(8) != 13) invalid("This clipboard image is not a valid PNG.");
    const auto width = big_endian(16);
    const auto height = big_endian(20);
    check_dimensions(width, height); // Check before the decoder allocates its pixels.
    auto stream = memory_stream(bytes);
    Bitmap source(stream.get(), FALSE);
    if (source.GetLastStatus() != Gdiplus::Ok || source.GetWidth() != width || source.GetHeight() != height)
        invalid("This PNG image could not be decoded.");
    const double ratio = preview ? std::min(1.0, static_cast<double>(preview_dimension) / std::max(width, height)) : 1.0;
    auto result = copy_bitmap(source, std::max(1u, static_cast<unsigned int>(std::round(width * ratio))),
                              std::max(1u, static_cast<unsigned int>(std::round(height * ratio))));
    if (source_width) *source_width = width;
    if (source_height) *source_height = height;
    return result; // Independent pixels: no stream or clipboard handle is retained.
}

bool contiguous(std::uint32_t mask) {
    if (!mask) return false;
    while (!(mask & 1)) mask >>= 1;
    return (static_cast<std::uint64_t>(mask) & (static_cast<std::uint64_t>(mask) + 1)) == 0;
}

unsigned char channel(std::uint32_t pixel, std::uint32_t mask, unsigned char fallback = 255) {
    if (!mask) return fallback;
    unsigned int shift = 0;
    while (!(mask & 1)) { mask >>= 1; ++shift; }
    return static_cast<unsigned char>((static_cast<std::uint64_t>((pixel >> shift) & mask) * 255 + mask / 2) / mask);
}

std::unique_ptr<Bitmap> decode_dib(const Bytes& bytes) {
    const auto header = read_at<BITMAPINFOHEADER>(bytes, 0);
    if (header.biSize != 40 && header.biSize != 52 && header.biSize != 56 &&
        header.biSize != 108 && header.biSize != 124) invalid("This bitmap header is not supported.");
    if (bytes.size() < header.biSize || header.biWidth <= 0 || header.biHeight == 0 ||
        header.biHeight == std::numeric_limits<LONG>::min() || header.biPlanes != 1)
        invalid("Invalid clipboard bitmap dimensions.");
    const auto width = static_cast<unsigned int>(header.biWidth);
    const auto height = static_cast<unsigned int>(header.biHeight < 0 ? -header.biHeight : header.biHeight);
    check_dimensions(width, height);
    const auto bits = header.biBitCount;
    if (bits != 1 && bits != 4 && bits != 8 && bits != 16 && bits != 24 && bits != 32)
        invalid("This clipboard bitmap depth is not supported.");
    if (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS && header.biCompression != alpha_bitfields)
        invalid("Compressed clipboard bitmaps are not supported. Copy the image as PNG.");
    const bool bitfields = header.biCompression != BI_RGB;
    if (bitfields && bits != 16 && bits != 32) invalid("Invalid bitmap colour masks.");

    std::size_t offset = header.biSize;
    std::array<std::uint32_t, 4> masks = bits == 16
        ? std::array<std::uint32_t, 4>{0x7c00, 0x03e0, 0x001f, 0}
        : std::array<std::uint32_t, 4>{0xff0000, 0x00ff00, 0x0000ff, 0};
    if (bitfields) {
        for (std::size_t i = 0; i < 3; ++i) masks[i] = read_at<std::uint32_t>(bytes, 40 + i * 4);
        if (header.biSize >= 56 || header.biCompression == alpha_bitfields)
            masks[3] = read_at<std::uint32_t>(bytes, 52);
        if (header.biSize == 40) offset += header.biCompression == alpha_bitfields ? 16 : 12;
    } else if (header.biSize >= 56 && bits == 32) {
        masks[3] = read_at<std::uint32_t>(bytes, 52);
    }
    if (bits == 16 || bits == 32) {
        std::uint32_t occupied = 0;
        for (std::size_t i = 0; i < masks.size(); ++i) {
            if (i == 3 && !masks[i]) continue;
            if (!contiguous(masks[i]) || (occupied & masks[i]) || (bits == 16 && masks[i] > 0xffff))
                invalid("Invalid bitmap colour masks.");
            occupied |= masks[i];
        }
    }
    const auto colour_count = bits <= 8 ? (header.biClrUsed ? header.biClrUsed : 1u << bits) : header.biClrUsed;
    if (colour_count > 256 || (bits <= 8 && colour_count > (1u << bits))) invalid("Invalid bitmap colour table.");
    const auto colour_offset = offset;
    offset += static_cast<std::size_t>(colour_count) * sizeof(RGBQUAD);
    const std::uint64_t stride = ((static_cast<std::uint64_t>(width) * bits + 31) / 32) * 4;
    const std::uint64_t pixel_bytes = stride * height;
    if (offset > bytes.size() || pixel_bytes > bytes.size() - offset) invalid("The clipboard bitmap is incomplete.");
    if (header.biSize == 124) {
        const auto v5 = read_at<BITMAPV5HEADER>(bytes, 0);
        if (v5.bV5CSType == embedded_profile || v5.bV5CSType == linked_profile) {
            // Packed DIB profiles follow pixels. Never open a linked profile or
            // treat profile bytes as pixels, even if the clipboard is malformed.
            if (v5.bV5ProfileData < offset + pixel_bytes || v5.bV5ProfileData > bytes.size() ||
                v5.bV5ProfileSize > bytes.size() - v5.bV5ProfileData)
                invalid("This bitmap has an invalid colour profile.");
        }
    }
    auto result = std::make_unique<Bitmap>(static_cast<INT>(width), static_cast<INT>(height), PixelFormat32bppARGB);
    Gdiplus::Rect area(0, 0, static_cast<INT>(width), static_cast<INT>(height));
    Gdiplus::BitmapData destination{};
    if (result->GetLastStatus() != Gdiplus::Ok ||
        result->LockBits(&area, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &destination) != Gdiplus::Ok)
        invalid("Could not allocate the clipboard image.");
    struct Unlock { Bitmap* image; Gdiplus::BitmapData* data; ~Unlock() { image->UnlockBits(data); } } unlock{result.get(), &destination};
    for (unsigned int y = 0; y < height; ++y) {
        const auto* source = bytes.data() + offset + static_cast<std::size_t>(header.biHeight < 0 ? y : height - 1 - y) * stride;
        auto* target = static_cast<unsigned char*>(destination.Scan0) + static_cast<std::ptrdiff_t>(y) * destination.Stride;
        for (unsigned int x = 0; x < width; ++x) {
            if (bits <= 8) {
                const unsigned int index = bits == 8 ? source[x] : bits == 4
                    ? (source[x / 2] >> (x % 2 ? 0 : 4)) & 15 : (source[x / 8] >> (7 - x % 8)) & 1;
                if (index >= colour_count) invalid("The bitmap uses an invalid palette entry.");
                const auto colour = read_at<RGBQUAD>(bytes, colour_offset + index * sizeof(RGBQUAD));
                target[x * 4] = colour.rgbBlue; target[x * 4 + 1] = colour.rgbGreen;
                target[x * 4 + 2] = colour.rgbRed; target[x * 4 + 3] = 255;
            } else if (bits == 24) {
                std::memcpy(target + x * 4, source + x * 3, 3); target[x * 4 + 3] = 255;
            } else {
                std::uint32_t pixel = 0;
                std::memcpy(&pixel, source + x * (bits / 8), bits / 8);
                target[x * 4] = channel(pixel, masks[2]); target[x * 4 + 1] = channel(pixel, masks[1]);
                target[x * 4 + 2] = channel(pixel, masks[0]); target[x * 4 + 3] = channel(pixel, masks[3]);
            }
        }
    }
    return result;
}

std::unique_ptr<Bitmap> bitmap_from_clipboard() {
    const auto handle = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
    BITMAP info{};
    if (!handle || GetObjectW(handle, sizeof(info), &info) != sizeof(info) || info.bmWidth <= 0 || info.bmHeight <= 0)
        invalid("Could not read the clipboard bitmap.");
    check_dimensions(static_cast<unsigned int>(info.bmWidth), static_cast<unsigned int>(info.bmHeight));
    BITMAPINFO requested{};
    requested.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    requested.bmiHeader.biWidth = info.bmWidth;
    requested.bmiHeader.biHeight = -info.bmHeight;
    requested.bmiHeader.biPlanes = 1;
    requested.bmiHeader.biBitCount = 32;
    requested.bmiHeader.biCompression = BI_RGB;
    Bytes bytes(sizeof(BITMAPINFOHEADER) + static_cast<std::size_t>(info.bmWidth) * info.bmHeight * 4);
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) invalid("Could not read the clipboard bitmap.");
    const int rows = GetDIBits(dc, handle, 0, static_cast<UINT>(info.bmHeight), bytes.data() + sizeof(BITMAPINFOHEADER), &requested, DIB_RGB_COLORS);
    DeleteDC(dc);
    if (rows != info.bmHeight) invalid("Could not copy the clipboard bitmap.");
    std::memcpy(bytes.data(), &requested.bmiHeader, sizeof(BITMAPINFOHEADER));
    return decode_dib(bytes);
}

Bytes png_bytes(Bitmap& bitmap) {
    IStream* raw = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &raw) != S_OK) invalid("Could not prepare the pasted image.");
    Stream stream(raw);
    if (bitmap.Save(stream.get(), &png_encoder, nullptr) != Gdiplus::Ok) invalid("Could not convert the pasted image to PNG.");
    STATSTG status{};
    if (stream->Stat(&status, STATFLAG_NONAME) != S_OK || !status.cbSize.QuadPart || status.cbSize.QuadPart > maximum_file_bytes)
        invalid("The saved image is too large. Use an image under 32 MiB.");
    LARGE_INTEGER start{};
    if (stream->Seek(start, STREAM_SEEK_SET, nullptr) != S_OK) invalid("Could not prepare the pasted image.");
    Bytes result(static_cast<std::size_t>(status.cbSize.QuadPart));
    ULONG read = 0;
    if (stream->Read(result.data(), static_cast<ULONG>(result.size()), &read) != S_OK || read != result.size())
        invalid("Could not prepare the pasted image.");
    return result;
}

std::string new_name() {
    GUID id{};
    if (CoCreateGuid(&id) != S_OK) invalid("Could not name the pasted image.");
    constexpr char hex[] = "0123456789abcdef";
    std::string name;
    for (const unsigned char byte : std::array<unsigned char, sizeof(GUID)>{
            static_cast<unsigned char>(id.Data1 >> 24), static_cast<unsigned char>(id.Data1 >> 16),
            static_cast<unsigned char>(id.Data1 >> 8), static_cast<unsigned char>(id.Data1),
            static_cast<unsigned char>(id.Data2 >> 8), static_cast<unsigned char>(id.Data2),
            static_cast<unsigned char>(id.Data3 >> 8), static_cast<unsigned char>(id.Data3),
            id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3], id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]}) {
        name += hex[byte >> 4]; name += hex[byte & 15];
    }
    return name + ".png";
}

std::string save_image(const std::filesystem::path& directory, Bitmap& bitmap) {
    const auto bytes = png_bytes(bitmap);
    std::error_code error;
    std::filesystem::create_directories(directory / L"note-images", error);
    if (error) invalid("Could not create the note image folder.");
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto name = new_name();
        const auto path = image_path(directory, name);
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            invalid("Could not save the pasted image. Check free space and folder permissions.");
        }
        DWORD written = 0;
        const bool saved = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                           written == bytes.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!saved) {
            DeleteFileW(path.c_str());
            invalid("Could not finish saving the pasted image. Check free space.");
        }
        return name;
    }
    invalid("Could not choose a unique image filename.");
}

} // namespace

bool valid_note_image_name(std::string_view name) noexcept {
    if (name.size() < 5 || name.size() > 132 || name.substr(name.size() - 4) != ".png") return false;
    for (char value : name.substr(0, name.size() - 4))
        if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
              (value >= '0' && value <= '9') || value == '-' || value == '_')) return false;
    return true;
}

ClipboardImageResult paste_note_image(HWND owner, const std::filesystem::path& directory) {
    const UINT png_format = RegisterClipboardFormatW(L"PNG");
    const std::array<UINT, 4> formats{png_format, CF_DIBV5, CF_DIB, CF_BITMAP};
    if (std::none_of(formats.begin(), formats.end(), [](UINT format) { return format && IsClipboardFormatAvailable(format); })) return {};
    ClipboardImageResult result;
    result.status = ClipboardImageStatus::error;
    if (!OpenClipboard(owner)) {
        result.error = L"The clipboard is busy. Try pasting again.";
        return result;
    }
    std::unique_ptr<Bitmap> image;
    std::string read_error;
    {
        struct Close { ~Close() { CloseClipboard(); } } close;
        UINT selected = 0;
        if (png_format && IsClipboardFormatAvailable(png_format)) selected = png_format;
        else for (UINT format = EnumClipboardFormats(0); format; format = EnumClipboardFormats(format)) {
            if (format == CF_DIBV5 || format == CF_DIB || format == CF_BITMAP) { selected = format; break; }
        }
        // Windows lists explicitly supplied formats before its synthesized
        // bitmap conversions. Validate the original bytes: falling back to a
        // synthesized format can hide a truncated source bitmap by zero-filling.
        try {
            if (selected == png_format && selected) image = decode_png(clipboard_bytes(selected, maximum_file_bytes), false);
            else if (selected == CF_BITMAP) image = bitmap_from_clipboard();
            else if (selected) image = decode_dib(clipboard_bytes(selected, maximum_clipboard_bytes));
            else invalid("This clipboard image could not be read.");
        } catch (const std::exception& error) { read_error = error.what(); }
    }
    try {
        if (!image) invalid(read_error.c_str());
        result.filename = save_image(directory, *image);
        result.status = ClipboardImageStatus::imported;
    } catch (const std::exception& error) {
        const std::string message = error.what();
        result.error.assign(message.begin(), message.end());
    }
    return result;
}

bool remove_note_image(const std::filesystem::path& directory, std::string_view name) noexcept {
    if (!valid_note_image_name(name)) return false;
    try {
        std::error_code error;
        const auto path = image_path(directory, name);
        const bool removed = std::filesystem::remove(path, error);
        return !error && (removed || !std::filesystem::exists(path, error));
    } catch (...) { return false; }
}

NoteImage::NoteImage() = default;
NoteImage::~NoteImage() = default;

bool NoteImage::load(const std::filesystem::path& directory, std::string_view name) {
    image_.reset(); width_ = 0; height_ = 0;
    try {
        const auto path = image_path(directory, name);
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || !size || size > maximum_file_bytes) return false;
        std::ifstream input(path, std::ios::binary);
        if (!input) return false;
        Bytes bytes(static_cast<std::size_t>(size));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
        image_ = decode_png(bytes, true, &width_, &height_);
        return true;
    } catch (...) { image_.reset(); width_ = 0; height_ = 0; return false; }
}

void NoteImage::draw(HDC dc, RECT destination) const {
    if (!image_ || destination.right <= destination.left || destination.bottom <= destination.top) return;
    const double ratio = std::min(static_cast<double>(destination.right - destination.left) / width_,
                                  static_cast<double>(destination.bottom - destination.top) / height_);
    const int width = std::max(1, static_cast<int>(std::round(width_ * ratio)));
    const int height = std::max(1, static_cast<int>(std::round(height_ * ratio)));
    Gdiplus::Rect target(destination.left + (destination.right - destination.left - width) / 2,
                         destination.top + (destination.bottom - destination.top - height) / 2, width, height);
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.DrawImage(image_.get(), target, 0, 0, static_cast<INT>(image_->GetWidth()),
                       static_cast<INT>(image_->GetHeight()), Gdiplus::UnitPixel);
}

} // namespace knock
