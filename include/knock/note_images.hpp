#pragma once

#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace Gdiplus { class Bitmap; }

namespace knock {

enum class ClipboardImageStatus { no_image, imported, error };

struct ClipboardImageResult {
    ClipboardImageStatus status = ClipboardImageStatus::no_image;
    std::string filename;
    std::wstring error;
};

// Call only in response to an explicit paste. The clipboard is never changed.
// GDI+ must already be started. Images are normalized to PNG in note-images.
ClipboardImageResult paste_note_image(HWND owner, const std::filesystem::path& data_directory);

// These helpers accept only bare PNG names, never arbitrary paths. Remove only
// an unsubmitted attachment; committed messages may still refer to its file.
bool valid_note_image_name(std::string_view filename) noexcept;
bool remove_note_image(const std::filesystem::path& data_directory, std::string_view filename) noexcept;

// A bounded decoded preview. Keep it in the UI cache rather than loading during
// paint. width/height retain the original dimensions for layout and labels.
class NoteImage {
public:
    NoteImage();
    ~NoteImage();
    NoteImage(const NoteImage&) = delete;
    NoteImage& operator=(const NoteImage&) = delete;

    bool load(const std::filesystem::path& data_directory, std::string_view filename);
    unsigned int width() const noexcept { return width_; }
    unsigned int height() const noexcept { return height_; }
    void draw(HDC dc, RECT destination) const;

private:
    std::unique_ptr<Gdiplus::Bitmap> image_;
    unsigned int width_ = 0;
    unsigned int height_ = 0;
};

} // namespace knock
