#include "UiTextShapingInternal.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    /** @copydoc UiTextShape::UiTextShape(std::shared_ptr<const Storage>) */
    UiTextShape::UiTextShape(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiTextShape::~UiTextShape */
    UiTextShape::~UiTextShape() {
        Release();
    }

    /** @copydoc UiTextShape::UiTextShape(const UiTextShape&) */
    UiTextShape::UiTextShape(const UiTextShape &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiTextShape::operator=(const UiTextShape&) */
    UiTextShape &UiTextShape::operator=(const UiTextShape &other) noexcept {
        if (this != &other) {
            UiTextShape replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiTextShape::UiTextShape(UiTextShape&&) */
    UiTextShape::UiTextShape(UiTextShape &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiTextShape::operator=(UiTextShape&&) */
    UiTextShape &UiTextShape::operator=(UiTextShape &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiTextShape::Retain */
    void UiTextShape::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load();
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (storage_->leases.compare_exchange_weak(current, current + 1))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiTextShape::Release */
    void UiTextShape::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1);
        storage_.reset();
    }

    /** @copydoc UiTextShape::Descriptor */
    const UiTextShapeDescriptor &UiTextShape::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiTextShape::Text */
    std::string_view UiTextShape::Text() const noexcept {
        return storage_->text;
    }

    /** @copydoc UiTextShape::Features */
    std::span<const UiTextFeature> UiTextShape::Features() const noexcept {
        return storage_->features;
    }

    /** @copydoc UiTextShape::Metrics */
    const UiTextMetrics &UiTextShape::Metrics() const noexcept {
        return storage_->metrics;
    }

    /** @copydoc UiTextShape::Runs */
    std::span<const UiTextGlyphRun> UiTextShape::Runs() const noexcept {
        return storage_->runs;
    }

    /** @copydoc UiTextShape::Glyphs */
    std::span<const UiTextGlyph> UiTextShape::Glyphs() const noexcept {
        return storage_->glyphs;
    }

    /** @copydoc UiTextShape::Clusters */
    std::span<const UiTextCluster> UiTextShape::Clusters() const noexcept {
        return storage_->clusters;
    }

    /** @copydoc UiTextShape::IsValid */
    bool UiTextShape::IsValid() const noexcept {
        if (!storage_ || !storage_->descriptor.IsValid() || !storage_->metrics.IsValid())
            return false;
        return std::all_of(storage_->runs.begin(), storage_->runs.end(), [](const auto &run) {
            return run.IsValid();
        }) && std::all_of(storage_->glyphs.begin(), storage_->glyphs.end(), [](const auto &glyph) {
            return glyph.IsValid();
        }) && std::all_of(storage_->clusters.begin(), storage_->clusters.end(), [](const auto &cluster) {
            return cluster.IsValid();
        });
    }
}  // namespace Horo::Runtime::Ui
