#include "knock/sprite.hpp"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <system_error>

namespace knock {
namespace {

constexpr UINT maximum_dimension = 2048;
constexpr UINT maximum_frames = 256;
constexpr std::uint64_t maximum_decoded_pixels = 64 * 1024 * 1024;

bool visible_bounds(Gdiplus::Bitmap& image, RECT& bounds) {
    Gdiplus::Rect area(0, 0, static_cast<INT>(image.GetWidth()), static_cast<INT>(image.GetHeight()));
    Gdiplus::BitmapData pixels{};
    if (image.LockBits(&area, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &pixels) != Gdiplus::Ok)
        return false;

    for (INT y = 0; y < area.Height; ++y) {
        const auto* row = static_cast<const unsigned char*>(pixels.Scan0) +
                          static_cast<std::ptrdiff_t>(y) * pixels.Stride;
        for (INT x = 0; x < area.Width; ++x) {
            if (row[x * 4 + 3] == 0) continue;
            bounds.left = std::min(bounds.left, static_cast<LONG>(x));
            bounds.top = std::min(bounds.top, static_cast<LONG>(y));
            bounds.right = std::max(bounds.right, static_cast<LONG>(x + 1));
            bounds.bottom = std::max(bounds.bottom, static_cast<LONG>(y + 1));
        }
    }
    return image.UnlockBits(&pixels) == Gdiplus::Ok;
}

} // namespace

Sprite::Sprite() = default;
Sprite::~Sprite() = default;

bool Sprite::load(const std::filesystem::path& directory, const std::wstring& name) {
    for (const auto* extension : {L".gif", L".png"}) {
        const auto path = directory / (name + extension);
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) continue;

        auto candidate = std::make_unique<Gdiplus::Bitmap>(path.c_str());
        if (candidate->GetLastStatus() != Gdiplus::Ok) continue;
        const UINT width = candidate->GetWidth();
        const UINT height = candidate->GetHeight();
        if (!width || !height || width > maximum_dimension || height > maximum_dimension) continue;

        GUID dimension{};
        UINT count = 1;
        const UINT dimension_count = candidate->GetFrameDimensionsCount();
        if (dimension_count > 8) continue;
        if (dimension_count) {
            std::vector<GUID> dimensions(dimension_count);
            if (candidate->GetFrameDimensionsList(dimensions.data(), dimension_count) != Gdiplus::Ok) continue;
            dimension = dimensions.front();
            for (const auto& id : dimensions) {
                if (IsEqualGUID(id, Gdiplus::FrameDimensionTime)) { dimension = id; break; }
            }
            count = candidate->GetFrameCount(&dimension);
        }
        if (!count || count > maximum_frames ||
            static_cast<std::uint64_t>(width) * height * count > maximum_decoded_pixels) continue;

        std::vector<unsigned int> delays(count, 100);
        const UINT size = candidate->GetPropertyItemSize(PropertyTagFrameDelay);
        if (size && size <= 65536) {
            std::vector<unsigned char> storage(size);
            auto* property = reinterpret_cast<Gdiplus::PropertyItem*>(storage.data());
            if (candidate->GetPropertyItem(PropertyTagFrameDelay, size, property) == Gdiplus::Ok &&
                property->type == PropertyTagTypeLong && property->value) {
                for (UINT i = 0; i < count && (i + 1) * sizeof(UINT) <= property->length; ++i) {
                    UINT hundredths = 0;
                    std::memcpy(&hundredths, static_cast<unsigned char*>(property->value) + i * sizeof(UINT), sizeof(UINT));
                    delays[i] = static_cast<unsigned int>(std::clamp<std::uint64_t>(
                        static_cast<std::uint64_t>(hundredths) * 10, 20, 10000));
                }
            }
        }

        RECT bounds{static_cast<LONG>(width), static_cast<LONG>(height), 0, 0};
        bool valid = true;
        for (UINT frame = 0; frame < count; ++frame) {
            if (count > 1 && candidate->SelectActiveFrame(&dimension, frame) != Gdiplus::Ok) {
                valid = false;
                break;
            }
            if (!visible_bounds(*candidate, bounds)) { valid = false; break; }
        }
        if (!valid || (count > 1 && candidate->SelectActiveFrame(&dimension, 0) != Gdiplus::Ok)) continue;
        if (bounds.left >= bounds.right || bounds.top >= bounds.bottom)
            bounds = RECT{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

        image_ = std::move(candidate);
        delays_ = std::move(delays);
        dimension_ = dimension;
        content_ = bounds;
        frame_ = 0;
        cycle_ = 0;
        for (const auto delay : delays_) cycle_ += delay;
        next_ = 0;
        running_ = false;
        return true;
    }
    return false;
}

bool Sprite::animated() const noexcept { return image_ && delays_.size() > 1; }

void Sprite::resume(std::uint64_t now) {
    if (running_) return;
    running_ = animated();
    if (running_) next_ = now + delays_[frame_];
}

void Sprite::pause() noexcept { running_ = false; }

bool Sprite::advance(std::uint64_t now) {
    if (!running_ || !animated() || now < next_) return false;

    // Discard whole missed cycles in constant time. Ordinary timer jitter does
    // not accumulate and gradually slow an animation down.
    if (now - next_ >= cycle_) next_ += ((now - next_) / cycle_) * cycle_;
    do {
        frame_ = (frame_ + 1) % static_cast<unsigned int>(delays_.size());
        next_ += delays_[frame_];
    } while (now >= next_);
    if (image_->SelectActiveFrame(&dimension_, frame_) != Gdiplus::Ok) {
        running_ = false;
        return false;
    }
    return true;
}

void Sprite::draw(HDC dc, RECT destination) const {
    if (!image_ || destination.right <= destination.left || destination.bottom <= destination.top) return;
    const auto source_width = content_.right - content_.left;
    const auto source_height = content_.bottom - content_.top;
    const float ratio = std::min(static_cast<float>(destination.right - destination.left) / source_width,
                                 static_cast<float>(destination.bottom - destination.top) / source_height);
    const int width = std::max(1, static_cast<int>(std::round(source_width * ratio)));
    const int height = std::max(1, static_cast<int>(std::round(source_height * ratio)));
    Gdiplus::Rect target(destination.left + (destination.right - destination.left - width) / 2,
                         destination.top + (destination.bottom - destination.top - height) / 2, width, height);
    Gdiplus::Graphics graphics(dc);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.DrawImage(image_.get(), target, content_.left, content_.top,
                       source_width, source_height, Gdiplus::UnitPixel);
}

} // namespace knock
