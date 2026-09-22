#include "Horo/Runtime/Ui/UiDocument.h"

#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace

    /** @copydoc CookedUiDocument::CookedUiDocument */
    CookedUiDocument::CookedUiDocument(UiDocumentId id, UiDocumentRevision revision, std::vector<UiAssetDependency> dependencies,
                                       std::vector<std::uint8_t> payload) noexcept
        : id_(id), revision_(revision), dependencies_(std::move(dependencies)), payload_(std::move(payload)) {}

    /** @copydoc CookedUiDocument::Create */
    Result<CookedUiDocument> CookedUiDocument::Create(const UiDocument &document, std::vector<std::uint8_t> payload) {
        if (payload.empty())
            return Failure<CookedUiDocument>(UiErrors::PayloadInvalid);
        if (payload.size() > MaximumCookedUiDocumentBytes)
            return Failure<CookedUiDocument>(UiErrors::CapacityExceeded);
        return Result<CookedUiDocument>::Success(CookedUiDocument{document.Id(),
                                                                  document.Revision(),
                                                                  {document.Dependencies().begin(), document.Dependencies().end()},
                                                                  std::move(payload)});
    }

    /** @copydoc CookedUiDocument::Id */
    UiDocumentId CookedUiDocument::Id() const noexcept {
        return id_;
    }

    /** @copydoc CookedUiDocument::SourceRevision */
    UiDocumentRevision CookedUiDocument::SourceRevision() const noexcept {
        return revision_;
    }

    /** @copydoc CookedUiDocument::Dependencies */
    std::span<const UiAssetDependency> CookedUiDocument::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc CookedUiDocument::Payload */
    std::span<const std::uint8_t> CookedUiDocument::Payload() const noexcept {
        return payload_;
    }
}  // namespace Horo::Runtime::Ui
