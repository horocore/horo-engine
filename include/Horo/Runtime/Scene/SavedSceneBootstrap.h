#pragma once

/**
 * @file SavedSceneBootstrap.h
 * @brief Path-independent saved-scene baseline admission and preparation.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Save/SaveArchiveMetadata.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Runtime/Scene/SaveContentWorld.h"

#include <optional>

namespace Horo::Runtime {
    class RuntimeSceneService;

    /** @brief Durable provenance for the save generation that requested a scene transition. */
    struct SavedSceneTransitionMetadata final {
        SaveGameSlotId slot;                   /**< Logical slot selected by the restore owner. */
        SlotGenerationId generation;           /**< Exact immutable publication being restored. */
        std::optional<SaveWorldId> priorWorld; /**< Previous logical world, when this crosses worlds. */

        /** @brief Checks required identities and optional prior-world evidence. @return Whether the metadata is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SavedSceneTransitionMetadata &) const noexcept = default;
    };

    /** @brief Exact durable requirements for recreating a saved scene from cooked content. */
    struct SavedSceneBootstrapDescriptor final {
        SaveWorldId world;                        /**< Logical world being reconstructed. */
        SaveBaseSceneId baseScene;                /**< Path-independent cooked scene asset identity. */
        Assets::AssetTypeId expectedAssetType;    /**< Persisted type evidence; host policy remains authoritative. */
        SceneDefinitionId definition;             /**< Stable logical definition identity. */
        SceneDefinitionRevision revision;         /**< Exact compatible authored revision. */
        Sha256Digest contentDigest;               /**< Exact compatible cooked-content digest. */
        std::optional<SceneObjectId> spawnAnchor; /**< Stable authored spawn location, when required. */
        SavedSceneTransitionMetadata transition;  /**< Restore operation provenance. */

        /** @brief Checks complete typed identity, revision, digest, spawn, and transition evidence. @return Validity. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const SavedSceneBootstrapDescriptor &) const noexcept = default;
    };

    /** @brief Borrowed verified provider envelope, available only during the admitted host decoder call.
     * @details The host defines the existing cooked scene payload format. RuntimeScene does not invent or parse an editor format.
     */
    class SavedSceneBaselineDecodeInput final {
    public:
        /** @brief Returns the durable requirements. @return Borrow valid only throughout DecodeBaseline. */
        [[nodiscard]] const SavedSceneBootstrapDescriptor &Descriptor() const noexcept {
            return descriptor_;
        }

        /** @brief Returns the actual provider-verified cooked envelope. @return Borrow valid only throughout DecodeBaseline. */
        [[nodiscard]] const Assets::AssetCookArtifact &Artifact() const noexcept {
            return artifact_;
        }

    private:
        friend Result<PreparedSavedSceneBootstrap> PrepareSavedSceneBootstrap(SavedSceneBootstrapDescriptor, const Assets::AssetTypeId &,
                                                                              ReconciledSaveContent, ISavedSceneBaselineDecoder *);

        SavedSceneBaselineDecodeInput(const SavedSceneBootstrapDescriptor &descriptor, const Assets::AssetCookArtifact &artifact) noexcept
            : descriptor_(descriptor), artifact_(artifact) {}

        const SavedSceneBootstrapDescriptor &descriptor_;
        const Assets::AssetCookArtifact &artifact_;
    };

    /** @brief Explicit trusted host decoder for its existing cooked scene format; invoked only after content preflight. */
    class ISavedSceneBaselineDecoder {
    public:
        virtual ~ISavedSceneBaselineDecoder() = default;
        /** @brief Produces an owned definition from the admitted provider envelope.
         * @param input Exact immutable envelope and saved requirements; all borrows expire on return.
         * @return Owned definition or contextual unsupported/invalid payload error. Must not prepare or publish a runtime world.
         */
        [[nodiscard]] virtual Result<RuntimeSceneDefinition> DecodeBaseline(const SavedSceneBaselineDecodeInput &input) = 0;
    };

    /** @brief Owned exact queue receipt and real Scene service pin; cannot bind an ordinary scene by matching definitions. */
    class QueuedSavedSceneBootstrap final {
    public:
        QueuedSavedSceneBootstrap(const QueuedSavedSceneBootstrap &) = delete;
        QueuedSavedSceneBootstrap &operator=(const QueuedSavedSceneBootstrap &) = delete;
        QueuedSavedSceneBootstrap(QueuedSavedSceneBootstrap &&) noexcept = default;
        QueuedSavedSceneBootstrap &operator=(QueuedSavedSceneBootstrap &&) noexcept = default;
        /** @brief Consumes the exact actual published Scene receipt and constructs its content-aware world owner.
         * @return Bound world or typed pending/rejected/stale/moved error; active Scene is reacquired during this call.
         * @details A pending receipt can be polled again. Successful binding consumes this queued value exactly once.
         */
        [[nodiscard]] Result<SaveContentWorld> BindPublishedWorld();

    private:
        friend class PreparedSavedSceneBootstrap;

        explicit QueuedSavedSceneBootstrap(std::shared_ptr<SaveContentDetail::WorldState> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<SaveContentDetail::WorldState> state_;
        bool consumed_{};
    };

    /** @brief Owned preflight proof whose definition can enter the existing scene preparation path. */
    class PreparedSavedSceneBootstrap final {
    public:
        PreparedSavedSceneBootstrap(const PreparedSavedSceneBootstrap &) = delete;
        PreparedSavedSceneBootstrap &operator=(const PreparedSavedSceneBootstrap &) = delete;
        /** @brief Transfers the unique preparation proof and invalidates reuse of the source. */
        PreparedSavedSceneBootstrap(PreparedSavedSceneBootstrap &&other) noexcept;
        /** @brief Transfers the unique preparation proof and invalidates reuse of the source. */
        PreparedSavedSceneBootstrap &operator=(PreparedSavedSceneBootstrap &&other) noexcept;

        /** @brief Returns the validated durable requirements. @return Borrow valid for this prepared value's lifetime. */
        [[nodiscard]] const SavedSceneBootstrapDescriptor &Descriptor() const noexcept;
        /** @brief Returns the logical cooked asset resolved from the saved identity. @return Stable base scene AssetId. */
        [[nodiscard]] Assets::AssetId BaseSceneAsset() const noexcept;
        /** @brief Returns the exact installed-content generation used for preflight. @return Non-zero pinned installation generation. */
        [[nodiscard]] std::uint64_t InstalledGeneration() const noexcept;
        /** @brief Returns the owned immutable default scene to receive later saved overrides. @return Borrowed definition. */
        [[nodiscard]] const RuntimeSceneDefinition &Definition() const noexcept;
        /** @brief Consumes and queues the validated default scene through normal lifecycle admission. @param service Actual shared Scene
         * owner pin retained through preparation and world binding.
         * @return Queue admission result; active publication still occurs only at its lifecycle commit boundary. The proof is consumed
         * even when admission fails, and subsequent use returns SaveBootstrapInvalid. */
        [[nodiscard]] Result<QueuedSavedSceneBootstrap> Queue(std::shared_ptr<RuntimeSceneService> service) &&;

    private:
        friend Result<PreparedSavedSceneBootstrap> PrepareSavedSceneBootstrap(SavedSceneBootstrapDescriptor, const Assets::AssetTypeId &,
                                                                              ReconciledSaveContent, ISavedSceneBaselineDecoder *);

        /** @brief Owns exact source/install authority and its admitted host-decoded definition. */
        PreparedSavedSceneBootstrap(SavedSceneBootstrapDescriptor descriptor, Assets::AssetId baseSceneAsset,
                                    RuntimeSceneDefinition definition, ReconciledSaveContent content) noexcept;

        SavedSceneBootstrapDescriptor descriptor_;
        Assets::AssetId baseSceneAsset_;
        RuntimeSceneDefinition definition_;
        ReconciledSaveContent content_;
        bool consumed_{};
    };

    /**
     * @brief Decodes the actual installed baseline only after complete content reconciliation, without preparing a world.
     * @param descriptor Exact saved baseline and world requirements matching the owned source archive.
     * @param requiredSceneAssetType Trusted host type; persisted metadata cannot weaken it.
     * @param content Consumed owned source/install reconciliation proof; no authoring registry or asserted digest substitutes for it.
     * @param decoder Explicit host decoder for its cooked format; null returns a typed unsupported decoder failure before callbacks.
     * @return Owned preparation proof, or contextual stale/unavailable/incompatible/decoder/spawn failure.
     */
    [[nodiscard]] Result<PreparedSavedSceneBootstrap> PrepareSavedSceneBootstrap(SavedSceneBootstrapDescriptor descriptor,
                                                                                 const Assets::AssetTypeId &requiredSceneAssetType,
                                                                                 ReconciledSaveContent content,
                                                                                 ISavedSceneBaselineDecoder *decoder);
}  // namespace Horo::Runtime
