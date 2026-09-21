#include "Horo/Runtime/Ui/UiDocument.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> MergeDependency(std::vector<UiAssetDependency> &dependencies, UiAssetDependency dependency) {
            if (!dependency.asset.IsValid() || dependency.expectedType.Value().empty())
                return Failure(UiErrors::DependencyInvalid);
            const auto position = std::ranges::lower_bound(dependencies, dependency.asset, {}, &UiAssetDependency::asset);
            if (position != dependencies.end() && position->asset == dependency.asset) {
                if (position->expectedType != dependency.expectedType)
                    return Failure(UiErrors::DependencyInvalid);
                position->required = position->required || dependency.required;
                return Result<void>::Success();
            }
            if (dependencies.size() == MaximumUiDocumentDependencies)
                return Failure(UiErrors::CapacityExceeded);
            dependencies.insert(position, std::move(dependency));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiDocument::UiDocument */
    UiDocument::UiDocument(UiDocumentId id, UiDocumentRevision revision, std::vector<UiCanvasDescriptor> canvases,
                           std::vector<UiLocalizedText> localizedTexts, std::vector<UiLocalizedAssetReference> localizedAssets,
                           std::vector<UiAssetDependency> dependencies) noexcept
        : id_(id), revision_(revision), canvases_(std::move(canvases)), localizedTexts_(std::move(localizedTexts)),
          localizedAssets_(std::move(localizedAssets)), dependencies_(std::move(dependencies)) {}

    /** @copydoc UiDocument::Id */
    UiDocumentId UiDocument::Id() const noexcept {
        return id_;
    }

    /** @copydoc UiDocument::Revision */
    UiDocumentRevision UiDocument::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc UiDocument::Canvases */
    std::span<const UiCanvasDescriptor> UiDocument::Canvases() const noexcept {
        return canvases_;
    }

    /** @copydoc UiDocument::LocalizedTexts */
    std::span<const UiLocalizedText> UiDocument::LocalizedTexts() const noexcept {
        return localizedTexts_;
    }

    /** @copydoc UiDocument::LocalizedAssets */
    std::span<const UiLocalizedAssetReference> UiDocument::LocalizedAssets() const noexcept {
        return localizedAssets_;
    }

    /** @copydoc UiDocument::Dependencies */
    std::span<const UiAssetDependency> UiDocument::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc UiDocumentBuilder::UiDocumentBuilder */
    UiDocumentBuilder::UiDocumentBuilder(UiDocumentId id, UiDocumentRevision revision) noexcept : id_(id), revision_(revision) {}

    /** @copydoc UiDocumentBuilder::AddCanvas */
    Result<void> UiDocumentBuilder::AddCanvas(UiCanvasDescriptor canvas) {
        if (canvases_.size() == MaximumUiDocumentCanvases)
            return Failure(UiErrors::CapacityExceeded);
        canvases_.push_back(std::move(canvas));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::AddLocalizedText */
    Result<void> UiDocumentBuilder::AddLocalizedText(UiLocalizedText text) {
        if (!text.IsValid())
            return Failure(UiErrors::LocalizedMessageInvalid);
        if (localizedTexts_.size() == MaximumUiDocumentLocalizedTexts)
            return Failure(UiErrors::CapacityExceeded);
        localizedTexts_.push_back(std::move(text));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::AddLocalizedAsset */
    Result<void> UiDocumentBuilder::AddLocalizedAsset(UiLocalizedAssetReference reference) {
        if (!reference.IsValid())
            return Failure(UiErrors::LocalizedAssetReferenceInvalid);
        if (localizedAssets_.size() == MaximumUiDocumentLocalizedAssets)
            return Failure(UiErrors::CapacityExceeded);

        std::vector<UiAssetDependency> candidate = dependencies_;
        candidate.reserve(dependencies_.size() + reference.Dependencies().size());
        for (const UiAssetDependency &dependency : reference.Dependencies()) {
            if (const auto merged = MergeDependency(candidate, dependency); merged.HasError())
                return merged;
        }
        dependencies_ = std::move(candidate);
        localizedAssets_.push_back(std::move(reference));
        return Result<void>::Success();
    }

    /** @copydoc UiDocumentBuilder::RequireAsset */
    Result<void> UiDocumentBuilder::RequireAsset(UiAssetDependency dependency) {
        return MergeDependency(dependencies_, std::move(dependency));
    }

    /** @copydoc UiDocumentBuilder::Build */
    Result<UiDocument> UiDocumentBuilder::Build() && {
        if (!id_.IsValid() || !revision_.IsValid() || canvases_.empty())
            return Failure<UiDocument>(UiErrors::DocumentInvalid);
        for (std::size_t index = 0; index < canvases_.size(); ++index) {
            const auto &canvas = canvases_[index];
            if (!canvas.IsValid())
                return Failure<UiDocument>(UiErrors::DocumentInvalid);
            for (std::size_t previous = 0; previous < index; ++previous)
                if (canvases_[previous].id == canvas.id || canvases_[previous].rootElement == canvas.rootElement)
                    return Failure<UiDocument>(UiErrors::DocumentDuplicateIdentity);
        }
        std::ranges::sort(dependencies_, {}, &UiAssetDependency::asset);
        return Result<UiDocument>::Success(UiDocument{id_, revision_, std::move(canvases_), std::move(localizedTexts_),
                                                      std::move(localizedAssets_), std::move(dependencies_)});
    }

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

    /** @copydoc ValidateUiCanvasAssetReference */
    Result<void> ValidateUiCanvasAssetReference(const UiCanvasAssetReference &reference) {
        if (!reference.asset.IsValid() || !reference.document.IsValid() || !reference.canvas.IsValid() ||
            !reference.minimumRevision.IsValid())
            return Failure(UiErrors::CanvasReferenceInvalid);
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeInstance::UiRuntimeInstance */
    UiRuntimeInstance::UiRuntimeInstance(UiDocumentId document, UiDocumentRevision revision, std::vector<UiAssetDependency> dependencies,
                                         std::vector<std::uint8_t> payload, RuntimeUiInstanceId instance) noexcept
        : document_(document), revision_(revision), dependencies_(std::move(dependencies)), payload_(std::move(payload)),
          instance_(instance) {}

    /** @copydoc UiRuntimeInstance::Create */
    Result<UiRuntimeInstance> UiRuntimeInstance::Create(CookedUiDocument document, RuntimeUiInstanceId instance) {
        if (!instance.IsValid())
            return Failure<UiRuntimeInstance>(UiErrors::HandleMalformed);
        if (!document.Id().IsValid() || !document.SourceRevision().IsValid() || document.Payload().empty())
            return Failure<UiRuntimeInstance>(UiErrors::PayloadInvalid);
        return Result<UiRuntimeInstance>::Success(UiRuntimeInstance{document.Id(), document.SourceRevision(),
                                                                    std::move(document.dependencies_), std::move(document.payload_),
                                                                    instance});
    }

    /** @copydoc UiRuntimeInstance::InstanceId */
    RuntimeUiInstanceId UiRuntimeInstance::InstanceId() const noexcept {
        return instance_;
    }

    /** @copydoc UiRuntimeInstance::DocumentId */
    UiDocumentId UiRuntimeInstance::DocumentId() const noexcept {
        return document_;
    }

    /** @copydoc UiRuntimeInstance::DocumentRevision */
    UiDocumentRevision UiRuntimeInstance::DocumentRevision() const noexcept {
        return revision_;
    }

    /** @copydoc UiRuntimeInstance::State */
    UiRuntimeInstanceState UiRuntimeInstance::State() const noexcept {
        return state_;
    }

    /** @copydoc UiRuntimeInstance::Dependencies */
    std::span<const UiAssetDependency> UiRuntimeInstance::Dependencies() const noexcept {
        return dependencies_;
    }

    /** @copydoc UiRuntimeInstance::Payload */
    std::span<const std::uint8_t> UiRuntimeInstance::Payload() const noexcept {
        return payload_;
    }

    /** @copydoc UiRuntimeInstance::Activate */
    Result<void> UiRuntimeInstance::Activate() {
        if (state_ != UiRuntimeInstanceState::Prepared)
            return Failure(UiErrors::InstanceStateInvalid);
        state_ = UiRuntimeInstanceState::Active;
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeInstance::BeginRetirement */
    Result<void> UiRuntimeInstance::BeginRetirement() {
        using enum UiRuntimeInstanceState;
        if (state_ != Prepared && state_ != Active)
            return Failure(UiErrors::InstanceStateInvalid);
        state_ = Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiRuntimeInstance::Shutdown */
    void UiRuntimeInstance::Shutdown() noexcept {
        state_ = UiRuntimeInstanceState::Stopped;
        std::vector<UiAssetDependency>{}.swap(dependencies_);
        std::vector<std::uint8_t>{}.swap(payload_);
    }
}  // namespace Horo::Runtime::Ui
