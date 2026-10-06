#include "Horo/Runtime/Scene/SavedSceneBootstrap.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"
#include "SaveContentInternal.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool HasDigestEvidence(const Sha256Digest &digest) noexcept {
            return std::ranges::any_of(digest.bytes, [](const std::uint8_t byte) {
                return byte != 0;
            });
        }
    }  // namespace

    /** @copydoc SavedSceneTransitionMetadata::IsValid */
    bool SavedSceneTransitionMetadata::IsValid() const noexcept {
        return slot.IsValid() && generation.IsValid() && (!priorWorld || priorWorld->IsValid());
    }

    /** @copydoc SavedSceneBootstrapDescriptor::IsValid */
    bool SavedSceneBootstrapDescriptor::IsValid() const noexcept {
        return world.IsValid() && baseScene.IsValid() && !expectedAssetType.Value().empty() && definition.IsValid() &&
               revision.value != 0 && HasDigestEvidence(contentDigest) && (!spawnAnchor || spawnAnchor->IsValid()) && transition.IsValid();
    }

    namespace {
        /** @brief Retains exact reconciliation ownership through deferred Scene publication and cancellation. */
        class ContentPublicationCheck final : public ScenePublicationCheck {
        public:
            explicit ContentPublicationCheck(ReconciledSaveContent content) noexcept : content_(std::move(content)) {}

            Result<void> ValidatePublication() const override {
                return content_.ValidateAdmission();
            }

            Result<void> ValidatePreparedComposition(const SceneCanonicalDatasetProjection projection) const override {
                return projection == SceneCanonicalDatasetProjection::Absent
                           ? Result<void>::Success()
                           : Result<void>::Failure(MakeError(SceneErrors::SaveBootstrapDatasetUnsupported));
            }

        private:
            ReconciledSaveContent content_;
        };
    }  // namespace

    /** @copydoc PreparedSavedSceneBootstrap::PreparedSavedSceneBootstrap */
    PreparedSavedSceneBootstrap::PreparedSavedSceneBootstrap(SavedSceneBootstrapDescriptor descriptor, Assets::AssetId baseSceneAsset,
                                                             RuntimeSceneDefinition definition, ReconciledSaveContent content) noexcept
        : descriptor_(std::move(descriptor)), baseSceneAsset_(baseSceneAsset), definition_(std::move(definition)),
          content_(std::move(content)) {}

    /** @copydoc PreparedSavedSceneBootstrap::PreparedSavedSceneBootstrap */
    PreparedSavedSceneBootstrap::PreparedSavedSceneBootstrap(PreparedSavedSceneBootstrap &&other) noexcept
        : descriptor_(std::move(other.descriptor_)), baseSceneAsset_(other.baseSceneAsset_), definition_(std::move(other.definition_)),
          content_(std::move(other.content_)), consumed_(other.consumed_) {
        other.consumed_ = true;
    }

    /** @copydoc PreparedSavedSceneBootstrap::operator= */
    PreparedSavedSceneBootstrap &PreparedSavedSceneBootstrap::operator=(PreparedSavedSceneBootstrap &&other) noexcept {
        if (this == &other)
            return *this;
        descriptor_ = std::move(other.descriptor_);
        baseSceneAsset_ = other.baseSceneAsset_;
        definition_ = std::move(other.definition_);
        content_ = std::move(other.content_);
        consumed_ = other.consumed_;
        other.consumed_ = true;
        return *this;
    }

    /** @copydoc PreparedSavedSceneBootstrap::Descriptor */
    const SavedSceneBootstrapDescriptor &PreparedSavedSceneBootstrap::Descriptor() const noexcept {
        return descriptor_;
    }

    /** @copydoc PreparedSavedSceneBootstrap::BaseSceneAsset */
    Assets::AssetId PreparedSavedSceneBootstrap::BaseSceneAsset() const noexcept {
        return baseSceneAsset_;
    }

    /** @copydoc PreparedSavedSceneBootstrap::InstalledGeneration */
    std::uint64_t PreparedSavedSceneBootstrap::InstalledGeneration() const noexcept {
        return content_.state_ ? content_.state_->installed->generation : 0;
    }

    /** @copydoc PreparedSavedSceneBootstrap::Definition */
    const RuntimeSceneDefinition &PreparedSavedSceneBootstrap::Definition() const noexcept {
        return definition_;
    }

    /** @copydoc PreparedSavedSceneBootstrap::Queue */
    Result<QueuedSavedSceneBootstrap> PreparedSavedSceneBootstrap::Queue(std::shared_ptr<RuntimeSceneService> service) && {
        if (consumed_ || !service)
            return Failure<QueuedSavedSceneBootstrap>(SceneErrors::SaveBootstrapInvalid);
        consumed_ = true;
        if (auto admitted = content_.ValidateAdmission(); admitted.HasError())
            return Result<QueuedSavedSceneBootstrap>::Failure(admitted.ErrorValue());
        try {
            auto state = std::make_shared<SaveContentDetail::WorldState>(service, descriptor_, content_.state_);
            auto check = std::make_unique<ContentPublicationCheck>(std::move(content_));
            auto queued = service->QueuePreparationWithPublicationCheck(std::move(definition_), std::move(check));
            if (queued.HasError())
                return Result<QueuedSavedSceneBootstrap>::Failure(queued.ErrorValue());
            state->publication.emplace(std::move(queued).Value());
            return Result<QueuedSavedSceneBootstrap>::Success(QueuedSavedSceneBootstrap{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<QueuedSavedSceneBootstrap>(SaveErrors::RestoreAllocationFailed);
        }
    }

    /** @copydoc QueuedSavedSceneBootstrap::BindPublishedWorld */
    Result<SaveContentWorld> QueuedSavedSceneBootstrap::BindPublishedWorld() {
        if (!state_ || consumed_ || !state_->publication)
            return Failure<SaveContentWorld>(SceneErrors::SaveBootstrapInvalid);
        ReconciledSaveContent content{state_->content};
        if (auto admission = content.ValidateAdmission(); admission.HasError())
            return Result<SaveContentWorld>::Failure(admission.ErrorValue());
        auto receipt = state_->publication->Snapshot();
        if (receipt.HasError())
            return Result<SaveContentWorld>::Failure(receipt.ErrorValue());
        if (receipt.Value().status == ScenePublicationStatus::Pending)
            return Failure<SaveContentWorld>(SceneErrors::OperationInProgress);
        if (receipt.Value().status != ScenePublicationStatus::Published)
            return Failure<SaveContentWorld>(SaveErrors::RestoreActivationStale);
        if (receipt.Value().datasets != SceneCanonicalDatasetProjection::Absent)
            return Failure<SaveContentWorld>(SceneErrors::SaveBootstrapDatasetUnsupported);
        const auto current = state_->service->ActiveScene();
        if (!current || current->RuntimeId() != receipt.Value().scene ||
            current->StructuralRevision() != receipt.Value().structuralRevision ||
            current->DefinitionId() != state_->descriptor.definition || current->DefinitionRevision() != state_->descriptor.revision)
            return Failure<SaveContentWorld>(SaveErrors::RestoreActivationStale);
        state_->boundScene = receipt.Value().scene;
        consumed_ = true;
        return Result<SaveContentWorld>::Success(SaveContentWorld{state_});
    }

    /** @copydoc PrepareSavedSceneBootstrap */
    Result<PreparedSavedSceneBootstrap> PrepareSavedSceneBootstrap(SavedSceneBootstrapDescriptor descriptor,
                                                                   const Assets::AssetTypeId &requiredSceneAssetType,
                                                                   ReconciledSaveContent content, ISavedSceneBaselineDecoder *decoder) {
        if (!descriptor.IsValid() || requiredSceneAssetType.Value().empty() || descriptor.expectedAssetType != requiredSceneAssetType)
            return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapInvalid);
        if (auto admission = content.ValidateAdmission(); admission.HasError())
            return Result<PreparedSavedSceneBootstrap>::Failure(admission.ErrorValue());
        if (!decoder)
            return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapDecoderUnavailable);
        auto &state = *content.state_;
        const auto &header = state.sourceReader.Header();
        if (header.world != descriptor.world || header.baseScene != descriptor.baseScene || header.slot != descriptor.transition.slot ||
            header.slotGeneration != descriptor.transition.generation)
            return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapIncompatible);
        const auto baseline = Assets::AssetId::FromBytes(descriptor.baseScene.Bytes());
        try {
            if (state.legacy &&
                std::ranges::find(state.assets, baseline, &SaveContentDetail::ResolvedAsset::original) == state.assets.end()) {
                auto resolved =
                    SaveContentDetail::ResolveLegacyBaseline(state, {baseline, requiredSceneAssetType, descriptor.contentDigest});
                if (resolved.HasError())
                    return Result<PreparedSavedSceneBootstrap>::Failure(resolved.ErrorValue());
            }
            if (!state.legacy || !state.requirements.empty()) {
                const bool declared = std::ranges::any_of(state.requirements, [&](const SaveContentRequirement &requirement) {
                    const auto *asset = std::get_if<SaveAssetContentRequirement>(&requirement.content);
                    return requirement.necessity == SaveContentNecessity::Required && asset && asset->asset == baseline &&
                           asset->type == requiredSceneAssetType && asset->envelopeDigest == descriptor.contentDigest;
                });
                if (!declared)
                    return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapIncompatible);
            }
            const auto resolved = std::ranges::find(state.assets, baseline, &SaveContentDetail::ResolvedAsset::original);
            if (resolved == state.assets.end() || resolved->type != requiredSceneAssetType)
                return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapAssetUnavailable);
            // A substitution is admitted only by the sealed project mapping; the original durable digest remains in the descriptor.
            if (resolved->installed == baseline && resolved->envelopeDigest != descriptor.contentDigest)
                return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapIncompatible);
            const SavedSceneBaselineDecodeInput input{descriptor, resolved->artifact};
            auto decoded = decoder->DecodeBaseline(input);
            if (decoded.HasError())
                return Result<PreparedSavedSceneBootstrap>::Failure(decoded.ErrorValue());
            if (auto admission = content.ValidateAdmission(); admission.HasError())
                return Result<PreparedSavedSceneBootstrap>::Failure(admission.ErrorValue());
            auto definition = std::move(decoded).Value();
            if (definition.Id() != descriptor.definition || definition.Revision() != descriptor.revision)
                return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapIncompatible);
            if (descriptor.spawnAnchor && std::ranges::find(definition.Entities(), *descriptor.spawnAnchor,
                                                            &RuntimeEntityDefinition::object) == definition.Entities().end())
                return Failure<PreparedSavedSceneBootstrap>(SceneErrors::SaveBootstrapSpawnMissing);
            return Result<PreparedSavedSceneBootstrap>::Success(
                PreparedSavedSceneBootstrap{std::move(descriptor), resolved->installed, std::move(definition), std::move(content)});
        } catch (const std::bad_alloc &) {
            return Failure<PreparedSavedSceneBootstrap>(SaveErrors::RestoreAllocationFailed);
        } catch (...) {
            // The admitted host decoder is a foreign boundary; no exception may prepare or publish a partial world.
            return Failure<PreparedSavedSceneBootstrap>(SaveErrors::RestoreAdapterContractInvalid);
        }
    }
}  // namespace Horo::Runtime
