#include "SourceDocumentInternal.h"

namespace Horo::Editor {
    SourceDocumentSnapshot::SourceDocumentSnapshot(std::shared_ptr<const Detail::SourceDocumentRoot> root) noexcept
        : root_(std::move(root)) {}

    /** @copydoc SourceDocumentSnapshot::Identity */
    DocumentIdentity SourceDocumentSnapshot::Identity() const {
        return root_ ? root_->identity : DocumentIdentity{};
    }

    /** @copydoc SourceDocumentSnapshot::SharesStateWith */
    bool SourceDocumentSnapshot::SharesStateWith(const SourceDocumentSnapshot &other) const noexcept {
        return root_ == other.root_;
    }

    /** @copydoc SourceDocumentSnapshot::IsCurrent */
    bool SourceDocumentSnapshot::IsCurrent() const noexcept {
        return root_ && root_->currentRevision->load() == root_->revision;
    }

    /** @copydoc SourceDocumentSnapshot::WasSavedInPlace */
    bool SourceDocumentSnapshot::WasSavedInPlace() const noexcept {
        return root_ && root_->savedInPlace.load();
    }

    /** @copydoc SourceDocumentSnapshot::Revision */
    std::uint64_t SourceDocumentSnapshot::Revision() const noexcept {
        return root_ ? root_->revision : 0;
    }

    /** @copydoc SourceDocumentSnapshot::BaseRevision */
    std::uint64_t SourceDocumentSnapshot::BaseRevision() const noexcept {
        return root_ ? root_->baseRevision : 0;
    }

    /** @copydoc SourceDocumentSnapshot::Text */
    std::string_view SourceDocumentSnapshot::Text() const noexcept {
        return root_ ? root_->text->bytes : std::string_view{};
    }

    /** @copydoc SourceDocumentSnapshot::TextLease */
    std::shared_ptr<const std::string> SourceDocumentSnapshot::TextLease() const noexcept {
        if (!root_)
            return {};
        return {root_->text, &root_->text->bytes};
    }

    /** @copydoc SourceDocumentSnapshot::DiskBase */
    std::string_view SourceDocumentSnapshot::DiskBase() const noexcept {
        return root_ ? root_->base->bytes : std::string_view{};
    }

    /** @copydoc SourceDocumentSnapshot::Dirty */
    bool SourceDocumentSnapshot::Dirty() const noexcept {
        return root_ && (root_->saveUnconfirmed || root_->text->bytes != root_->base->bytes);
    }

    /** @copydoc SourceDocumentSnapshot::Metadata */
    SourceTextMetadata SourceDocumentSnapshot::Metadata() const noexcept {
        return root_ ? root_->text->metadata : SourceTextMetadata{};
    }

    /** @copydoc SourceDocumentSnapshot::BaseMetadata */
    SourceTextMetadata SourceDocumentSnapshot::BaseMetadata() const noexcept {
        return root_ ? root_->base->metadata : SourceTextMetadata{};
    }

    /** @copydoc SourceDocumentSnapshot::ExternalState */
    SourceExternalState SourceDocumentSnapshot::ExternalState() const noexcept {
        return root_ ? root_->external : SourceExternalState::Unreadable;
    }
}  // namespace Horo::Editor
