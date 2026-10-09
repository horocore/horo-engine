#include "Horo/Runtime/Ui/UiGlyphAtlas.h"
#include "UiGlyphAtlasStorage.h"

#include <tuple>
#include <utility>

namespace Horo::Runtime::Ui {
    /** @copydoc UiGlyphAtlas::PageLease::PageLease */
    UiGlyphAtlas::PageLease::PageLease(std::shared_ptr<const Storage> storage, const UiGlyphAtlasPageId page,
                                       const UiGlyphAtlasRevision revision) noexcept
        : storage_(std::move(storage)), page_(page), revision_(revision) {}

    /** @copydoc UiGlyphAtlas::PageLease::Page */
    UiGlyphAtlasPageId UiGlyphAtlas::PageLease::Page() const noexcept {
        return page_;
    }

    /** @copydoc UiGlyphAtlas::PageLease::Revision */
    UiGlyphAtlasRevision UiGlyphAtlas::PageLease::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc UiGlyphAtlas::PageLease::IsCurrent */
    bool UiGlyphAtlas::PageLease::IsCurrent() const noexcept {
        return storage_ && storage_->IsActive() && storage_->revision == revision_ &&
               std::ranges::find(storage_->pages, page_) != storage_->pages.end();
    }

    /** @copydoc UiGlyphAtlas::AcquirePage */
    Result<UiGlyphAtlas::PageLease> UiGlyphAtlas::AcquirePage(const UiGlyphAtlasPageId page) const {
        if (!storage_ || !storage_->IsActive())
            return UiGlyphAtlasStorageDetail::Failure<PageLease>(UiErrors::GlyphAtlasLifecycleUnavailable);
        if (std::ranges::find(storage_->pages, page) == storage_->pages.end())
            return UiGlyphAtlasStorageDetail::Failure<PageLease>(UiErrors::GlyphAtlasFrameInvalid);
        return Result<PageLease>::Success(PageLease{storage_, page, storage_->revision});
    }

    /** @copydoc UiGlyphAtlas::FrameLease::FrameLease(std::shared_ptr<Storage>, UiGlyphAtlasFrameId) */
    UiGlyphAtlas::FrameLease::FrameLease(std::shared_ptr<Storage> storage, const UiGlyphAtlasFrameId frame) noexcept
        : storage_(std::move(storage)), frame_(frame) {}

    /** @copydoc UiGlyphAtlas::FrameLease::~FrameLease */
    UiGlyphAtlas::FrameLease::~FrameLease() {
        Release();
    }

    /** @copydoc UiGlyphAtlas::FrameLease::FrameLease(FrameLease &&) */
    UiGlyphAtlas::FrameLease::FrameLease(FrameLease &&other) noexcept
        : storage_(std::move(other.storage_)), frame_(std::exchange(other.frame_, {})) {}

    /** @copydoc UiGlyphAtlas::FrameLease::operator= */
    UiGlyphAtlas::FrameLease &UiGlyphAtlas::FrameLease::operator=(FrameLease &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
            frame_ = std::exchange(other.frame_, {});
        }
        return *this;
    }

    /** @copydoc UiGlyphAtlas::FrameLease::Frame */
    UiGlyphAtlasFrameId UiGlyphAtlas::FrameLease::Frame() const noexcept {
        return frame_;
    }

    /** @copydoc UiGlyphAtlas::FrameLease::Revision */
    UiGlyphAtlasRevision UiGlyphAtlas::FrameLease::Revision() const noexcept {
        return storage_ ? storage_->revision : UiGlyphAtlasRevision{};
    }

    /** @copydoc UiGlyphAtlas::FrameLease::OwnsPage */
    bool UiGlyphAtlas::FrameLease::OwnsPage(const UiGlyphAtlasPageId page, const UiGlyphAtlasRevision revision) const noexcept {
        return storage_ && frame_.ownership == page.ownership && storage_->revision == revision &&
               std::ranges::find(storage_->pages, page) != storage_->pages.end();
    }

    /** @copydoc UiGlyphAtlas::FrameLease::OwnsPage(const PageLease &) */
    bool UiGlyphAtlas::FrameLease::OwnsPage(const PageLease &page) const noexcept {
        return storage_ && storage_ == page.storage_ && OwnsPage(page.page_, page.revision_);
    }

    /** @copydoc UiGlyphAtlas::FrameLease::PinsGlyph */
    bool UiGlyphAtlas::FrameLease::PinsGlyph(const UiFontFace &face, const std::uint32_t glyph, const UiGlyphAtlasPageId page,
                                             const std::array<float, 4> &uv) const noexcept {
        if (!storage_ || !OwnsPage(page, storage_->revision) || !face.IsValid() || !face.Revision().IsValid())
            return false;
        const auto index = storage_->FrameIndex(frame_);
        if (index.HasError())
            return false;
        const auto &pins = storage_->frames[index.Value()].entries;
        const auto key = std::tuple{UiTextFaceId::Create(face.Id().Bytes()).Value(), face.Revision(), glyph, page, uv};
        const auto position = std::ranges::lower_bound(pins, key, {}, [&](const std::uint32_t entryIndex) {
            return storage_->PinIdentity(entryIndex);
        });
        if (position == pins.end() || storage_->PinIdentity(*position) != key)
            return false;
        const auto &source = storage_->entries[*position].sourceFace;
        return source && source->SharesGeneration(face);
    }

    /** @copydoc UiGlyphAtlas::FrameLease::Release */
    void UiGlyphAtlas::FrameLease::Release() noexcept {
        if (storage_)
            storage_->ReleaseFrame(frame_);
        storage_.reset();
        frame_ = {};
    }
}  // namespace Horo::Runtime::Ui
