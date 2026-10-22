#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace Gdiplus { class Bitmap; }

namespace knock {

// A fixed crop across all animation frames keeps transparent canvas padding
// from shrinking the character, without making the character jump as it moves.
class Sprite {
public:
    Sprite();
    ~Sprite();
    Sprite(const Sprite&) = delete;
    Sprite& operator=(const Sprite&) = delete;

    // Prefer GIF, then PNG. A missing or invalid override keeps the old sprite.
    bool load(const std::filesystem::path& directory, const std::wstring& name);
    bool animated() const noexcept;
    RECT content_bounds() const noexcept { return content_; }

    void resume(std::uint64_t now = GetTickCount64());
    void pause() noexcept;
    bool advance(std::uint64_t now = GetTickCount64());
    void draw(HDC dc, RECT destination) const;

private:
    std::unique_ptr<Gdiplus::Bitmap> image_;
    std::vector<unsigned int> delays_;
    GUID dimension_{};
    RECT content_{};
    unsigned int frame_ = 0;
    std::uint64_t next_ = 0;
    std::uint64_t cycle_ = 0;
    bool running_ = false;
};

} // namespace knock
