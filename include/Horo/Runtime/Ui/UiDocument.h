#pragma once

/** @file UiDocument.h
 * @brief Authored, cooked, scene-reference, and mutable-instance Runtime UI contracts.
 */

#include "Horo/Runtime/Ui/UiAssetDependency.h"
#include "Horo/Runtime/Ui/UiCanvasSpace.h"
#include "Horo/Runtime/Ui/UiLocalization.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    /** @brief Maximum canvas descriptors admitted by one authored document. */
    inline constexpr std::size_t MaximumUiDocumentCanvases = 64;
    /** @brief Maximum canonical asset dependencies admitted by one authored document. */
    inline constexpr std::size_t MaximumUiDocumentDependencies = 1024;
    /** @brief Maximum localized message references admitted by one authored document. */
    inline constexpr std::size_t MaximumUiDocumentLocalizedTexts = 4'096;
    /** @brief Maximum localized asset references admitted by one authored document. */
    inline constexpr std::size_t MaximumUiDocumentLocalizedAssets = 1'024;
    /** @brief Maximum byte count admitted by one cooked document payload. */
    inline constexpr std::size_t MaximumCookedUiDocumentBytes = 64ULL * 1024ULL * 1024ULL;

    /** @brief Immutable validated authoring model stored by a `.uicanvas` document owner. */
    class UiDocument final {
    public:
        /** @brief Returns the stable authored document identity. @return Document identity. */
        [[nodiscard]] UiDocumentId Id() const noexcept;
        /** @brief Returns the authored document revision. @return Non-zero monotonic revision. */
        [[nodiscard]] UiDocumentRevision Revision() const noexcept;
        /** @brief Returns canvases in stable authored order. @return Borrowed immutable canvas descriptors. */
        [[nodiscard]] std::span<const UiCanvasDescriptor> Canvases() const noexcept;
        /** @brief Returns localized message references in authored order. @return Borrowed immutable references. */
        [[nodiscard]] std::span<const UiLocalizedText> LocalizedTexts() const noexcept;
        /** @brief Returns localized asset references in authored order. @return Borrowed immutable references. */
        [[nodiscard]] std::span<const UiLocalizedAssetReference> LocalizedAssets() const noexcept;
        /** @brief Returns dependencies in canonical AssetId order. @return Borrowed immutable dependencies. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;

    private:
        friend class UiDocumentBuilder;
        /** @brief Adopts validated authored state from UiDocumentBuilder. */
        UiDocument(UiDocumentId id, UiDocumentRevision revision, std::vector<UiCanvasDescriptor> canvases,
                   std::vector<UiLocalizedText> localizedTexts, std::vector<UiLocalizedAssetReference> localizedAssets,
                   std::vector<UiAssetDependency> dependencies) noexcept;
        UiDocumentId id_;                                        /**< Stable authored identity. */
        UiDocumentRevision revision_;                            /**< Authored revision represented by this snapshot. */
        std::vector<UiCanvasDescriptor> canvases_;               /**< Validated authored-order canvases. */
        std::vector<UiLocalizedText> localizedTexts_;            /**< Authored stable message references. */
        std::vector<UiLocalizedAssetReference> localizedAssets_; /**< Authored localized asset variant sets. */
        std::vector<UiAssetDependency> dependencies_;            /**< Canonical dependency enumeration. */
    };

    /** @brief Load-time authoring builder that publishes only complete validated documents. */
    class UiDocumentBuilder final {
    public:
        /** @brief Starts a bounded authoring transaction. @param id Stable document identity.
         * @param revision Non-zero authored revision.
         */
        UiDocumentBuilder(UiDocumentId id, UiDocumentRevision revision) noexcept;
        /** @brief Adds one canvas in stable authored order. @param canvas Canvas identity and root.
         * @return Success or UiErrors::CapacityExceeded without modifying the builder.
         */
        [[nodiscard]] Result<void> AddCanvas(UiCanvasDescriptor canvas);
        /**
         * @brief Adds one complete localized message reference in authored order.
         * @param text Owned stable key, arguments, fallback and failure policy.
         * @return Success or a typed capacity/invalid-reference failure; failure leaves the builder unchanged.
         */
        [[nodiscard]] Result<void> AddLocalizedText(UiLocalizedText text);
        /**
         * @brief Adds one localized asset reference and atomically merges all declared variants into dependencies.
         * @param reference Owned finite localized asset mapping.
         * @return Success or a typed validation/conflict/capacity failure; failure leaves the builder unchanged.
         */
        [[nodiscard]] Result<void> AddLocalizedAsset(UiLocalizedAssetReference reference);
        /** @brief Adds or strengthens one dependency. @param dependency Asset requirement.
         * @return Success, or a typed invalid/conflicting/capacity error.
         */
        [[nodiscard]] Result<void> RequireAsset(UiAssetDependency dependency);
        /** @brief Validates and consumes the builder. @return Immutable authored document or typed failure. */
        [[nodiscard]] Result<UiDocument> Build() &&;

    private:
        UiDocumentId id_;                                        /**< Candidate authored identity. */
        UiDocumentRevision revision_;                            /**< Candidate authored revision. */
        std::vector<UiCanvasDescriptor> canvases_;               /**< Bounded authored-order canvases. */
        std::vector<UiLocalizedText> localizedTexts_;            /**< Bounded authored message references. */
        std::vector<UiLocalizedAssetReference> localizedAssets_; /**< Bounded authored asset references. */
        std::vector<UiAssetDependency> dependencies_;            /**< Bounded canonicalizable dependencies. */
    };

    /** @brief Versioned cooked payload, separate from authoring state and mutable runtime state. */
    class CookedUiDocument final {
    public:
        /** @brief Creates cooked state from one validated authored revision. @param document Source document.
         * @param payload Non-empty deterministic cooked bytes within the public bound.
         * @return Owned cooked document or typed payload error.
         */
        [[nodiscard]] static Result<CookedUiDocument> Create(const UiDocument &document, std::vector<std::uint8_t> payload);
        /** @brief Returns the cooked document identity. @return Stable authored identity. */
        [[nodiscard]] UiDocumentId Id() const noexcept;
        /** @brief Returns the authored revision used for cooking. @return Source revision. */
        [[nodiscard]] UiDocumentRevision SourceRevision() const noexcept;
        /** @brief Returns the cooked dependency manifest. @return Borrowed immutable dependencies. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;
        /** @brief Returns deterministic cooked bytes. @return Borrowed immutable payload. */
        [[nodiscard]] std::span<const std::uint8_t> Payload() const noexcept;

    private:
        friend class UiRuntimeInstance;
        /** @brief Adopts validated cooked state. */
        CookedUiDocument(UiDocumentId id, UiDocumentRevision revision, std::vector<UiAssetDependency> dependencies,
                         std::vector<std::uint8_t> payload) noexcept;
        UiDocumentId id_;                             /**< Stable authored identity. */
        UiDocumentRevision revision_;                 /**< Source authored revision. */
        std::vector<UiAssetDependency> dependencies_; /**< Canonical cooked dependency manifest. */
        std::vector<std::uint8_t> payload_;           /**< Deterministic owned cooked bytes. */
    };

    /** @brief Stable scene/component reference to one canvas asset and expected authored identity. */
    struct UiCanvasAssetReference final {
        Assets::AssetId asset;              /**< Asset containing the cooked document. */
        UiDocumentId document;              /**< Expected stable document identity. */
        UiCanvasId canvas;                  /**< Canvas selected inside the document. */
        UiDocumentRevision minimumRevision; /**< Oldest authored revision accepted by the scene. */
        /** @brief Compares serialized reference evidence. @return Structural ordering and equality. */
        [[nodiscard]] auto operator<=>(const UiCanvasAssetReference &) const noexcept = default;
    };

    /** @brief Validates a serialized scene/component canvas reference. @param reference Reference to inspect.
     * @return Success or a typed reference error.
     */
    [[nodiscard]] Result<void> ValidateUiCanvasAssetReference(const UiCanvasAssetReference &reference);

    /** @brief Explicit lifecycle of a mutable runtime instance. */
    enum class UiRuntimeInstanceState : std::uint8_t {
        Prepared, /**< Owns cooked state but admits no frame work. */
        Active,   /**< Admits owner-thread runtime work. */
        Retiring, /**< Rejects new work while shutdown drains. */
        Stopped   /**< Owns no cooked payload or dependencies. */
    };

    /** @brief Mutable runtime owner of one exact cooked document and transient instance identity. */
    class UiRuntimeInstance final {
    public:
        /** @brief Consumes one cooked snapshot into a prepared mutable instance.
         * @param document Owned cooked representation transferred into runtime storage.
         * @param instance Valid transient identity issued by the owning service.
         * @return Prepared instance or typed invalid-payload failure.
         */
        [[nodiscard]] static Result<UiRuntimeInstance> Create(CookedUiDocument document, RuntimeUiInstanceId instance);
        /** @brief Runtime instances have unique mutable ownership. */
        UiRuntimeInstance(const UiRuntimeInstance &) = delete;
        /** @brief Runtime instances cannot share mutable ownership. */
        UiRuntimeInstance &operator=(const UiRuntimeInstance &) = delete;
        /** @brief Transfers unique runtime ownership. */
        UiRuntimeInstance(UiRuntimeInstance &&) noexcept = default;
        /** @brief Replaces this instance by transferring unique runtime ownership. */
        UiRuntimeInstance &operator=(UiRuntimeInstance &&) noexcept = default;
        /** @brief Returns the transient instance identity. @return Owner-issued runtime identity. */
        [[nodiscard]] RuntimeUiInstanceId InstanceId() const noexcept;
        /** @brief Returns the stable source document identity. @return Authored document identity. */
        [[nodiscard]] UiDocumentId DocumentId() const noexcept;
        /** @brief Returns the exact cooked source revision. @return Authored source revision. */
        [[nodiscard]] UiDocumentRevision DocumentRevision() const noexcept;
        /** @brief Returns the explicit lifecycle state. @return Current owner-thread state. */
        [[nodiscard]] UiRuntimeInstanceState State() const noexcept;
        /** @brief Returns dependencies retained until shutdown. @return Borrowed immutable dependencies. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;
        /** @brief Returns cooked bytes retained until shutdown. @return Borrowed immutable payload. */
        [[nodiscard]] std::span<const std::uint8_t> Payload() const noexcept;
        /** @brief Admits runtime work from Prepared. @return Success or UiErrors::InstanceStateInvalid. */
        [[nodiscard]] Result<void> Activate();
        /** @brief Stops admission from Prepared or Active. @return Success or UiErrors::InstanceStateInvalid. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Stops the instance and releases its cooked payload. Repeated shutdown is harmless. */
        void Shutdown() noexcept;

    private:
        /** @brief Adopts validated cooked state into one Prepared instance. */
        UiRuntimeInstance(UiDocumentId document, UiDocumentRevision revision, std::vector<UiAssetDependency> dependencies,
                          std::vector<std::uint8_t> payload, RuntimeUiInstanceId instance) noexcept;
        UiDocumentId document_;                                          /**< Stable source document identity. */
        UiDocumentRevision revision_;                                    /**< Exact cooked source revision. */
        std::vector<UiAssetDependency> dependencies_;                    /**< Retained dependency manifest. */
        std::vector<std::uint8_t> payload_;                              /**< Retained cooked representation. */
        RuntimeUiInstanceId instance_;                                   /**< Owner-issued transient identity. */
        UiRuntimeInstanceState state_{UiRuntimeInstanceState::Prepared}; /**< Owner-thread lifecycle state. */
    };
}  // namespace Horo::Runtime::Ui
