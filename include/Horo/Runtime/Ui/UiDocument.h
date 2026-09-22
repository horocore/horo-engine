#pragma once

/** @file UiDocument.h
 * @brief Authored, cooked, scene-reference, and mutable-instance Runtime UI contracts.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Runtime/Ui/UiAssetDependency.h"
#include "Horo/Runtime/Ui/UiCanvasSpace.h"
#include "Horo/Runtime/Ui/UiLocalization.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
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
    /** @brief Maximum elements retained by one authored document. */
    inline constexpr std::size_t MaximumUiDocumentElements = 4096;
    /** @brief Maximum typed properties attached to one authored element. */
    inline constexpr std::size_t MaximumUiDocumentProperties = 64;
    /** @brief Maximum explicit references attached to one authored element. */
    inline constexpr std::size_t MaximumUiDocumentReferences = 64;
    /** @brief Maximum route definitions retained by one authored document. */
    inline constexpr std::size_t MaximumUiDocumentRoutes = 128;
    /** @brief Maximum UTF-8 bytes in one authored UI property key or text value. */
    inline constexpr std::size_t MaximumUiDocumentTextBytes = 4096;
    /** @brief Maximum byte count admitted by one cooked document payload. */
    inline constexpr std::size_t MaximumCookedUiDocumentBytes = 64ULL * 1024ULL * 1024ULL;

    /** @brief Version of the backend-neutral binary Runtime UI cooked payload. */
    inline constexpr std::uint32_t CurrentCookedUiDocumentFormatVersion = 1;

    /** @brief Bounded limits applied while producing or decoding one cooked Runtime UI document. */
    struct UiDocumentCookLimits final {
        std::size_t maximumPayloadBytes{MaximumCookedUiDocumentBytes};
        std::size_t maximumCanvases{MaximumUiDocumentCanvases};
        std::size_t maximumElements{MaximumUiDocumentElements};
        std::size_t maximumDependencies{MaximumUiDocumentDependencies};
        std::size_t maximumPropertiesPerElement{MaximumUiDocumentProperties};
        std::size_t maximumReferencesPerElement{MaximumUiDocumentReferences};
        std::size_t maximumRoutes{MaximumUiDocumentRoutes};
        std::size_t maximumTextBytes{MaximumUiDocumentTextBytes};

        /** @brief Checks that caller limits are positive and cannot weaken compiled safety ceilings. @return Whether usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumPayloadBytes > 0 && maximumPayloadBytes <= MaximumCookedUiDocumentBytes && maximumCanvases > 0 &&
                   maximumCanvases <= MaximumUiDocumentCanvases && maximumElements <= MaximumUiDocumentElements &&
                   maximumDependencies <= MaximumUiDocumentDependencies && maximumPropertiesPerElement > 0 &&
                   maximumPropertiesPerElement <= MaximumUiDocumentProperties && maximumReferencesPerElement > 0 &&
                   maximumReferencesPerElement <= MaximumUiDocumentReferences && maximumRoutes <= MaximumUiDocumentRoutes &&
                   maximumTextBytes > 0 && maximumTextBytes <= MaximumUiDocumentTextBytes;
        }
    };

    /** @brief Version of the durable Runtime UI document source schema. */
    struct UiDocumentSchemaVersion final {
        std::uint16_t major{1};
        std::uint16_t minor{1};

        [[nodiscard]] constexpr auto operator<=>(const UiDocumentSchemaVersion &) const noexcept = default;
    };

    /** @brief Current schema emitted by the Runtime UI document serializer. */
    inline constexpr UiDocumentSchemaVersion CurrentUiDocumentSchemaVersion{1, 1};

    /** @brief Closed semantic presentation band persisted by a route definition. */
    enum class UiPresentationBand : std::uint8_t {
        World,
        Hud,
        Screen,
        Overlay,
        Modal,
        Loading,
        Debug,
        Count,
    };

    /** @brief Closed typed identity category for one durable UI reference. */
    enum class UiReferenceKind : std::uint8_t {
        Element,
        Canvas,
        Document,
        Asset,
        Count,
    };

    /** @brief A path-independent reference to another authored UI or asset identity. */
    struct UiReference final {
        UiReferenceKind kind{UiReferenceKind::Element};
        UiElementId element;
        UiCanvasId canvas;
        UiDocumentId document;
        Assets::AssetId asset;
        Assets::AssetTypeId expectedAssetType;

        [[nodiscard]] auto operator<=>(const UiReference &) const noexcept = default;
    };

    /** @brief Closed typed property value; runtime pointers, callbacks, and string paths are not representable. */
    using UiPropertyValue = std::variant<bool, std::int64_t, double, std::string, UiReference>;

    /** @brief One typed authored property identified by a stable schema key. */
    struct UiTypedProperty final {
        std::string key;
        UiPropertyValue value;

        [[nodiscard]] auto operator<=>(const UiTypedProperty &) const noexcept = default;
    };

    /** @brief One authored element with stable identity, hierarchy, typed properties, and references. */
    struct UiDocumentElement final {
        UiElementId id;
        UiElementId parent;
        Assets::AssetTypeId type;
        std::vector<UiTypedProperty> properties;
        std::vector<UiReference> references;

        [[nodiscard]] auto operator<=>(const UiDocumentElement &) const noexcept = default;
    };

    /** @brief Durable route definition metadata; live route instances and transition cursors are excluded. */
    struct UiRouteMetadata final {
        UiRouteId id;
        UiPresentationBand band{UiPresentationBand::Screen};
        std::uint32_t order{};
        bool modal{};

        [[nodiscard]] auto operator<=>(const UiRouteMetadata &) const noexcept = default;
    };

    /** @brief One immutable cooked dependency payload retained by a prepared runtime UI instance. */
    struct UiRuntimeAsset final {
        UiAssetDependency dependency;                             /**< Stable dependency identity and expected type. */
        std::shared_ptr<const std::vector<std::uint8_t>> payload; /**< Owned immutable cooked payload lease. */

        /** @brief Compares dependency identity only; payload ownership is intentionally not semantic data. */
        [[nodiscard]] bool operator==(const UiRuntimeAsset &other) const noexcept {
            return dependency == other.dependency;
        }
    };

    /** @brief Immutable validated authoring model stored by a `.uicanvas` document owner. */
    class UiDocument final {
    public:
        /** @brief Returns the exact source schema represented by this immutable snapshot. @return Durable schema version. */
        [[nodiscard]] UiDocumentSchemaVersion SchemaVersion() const noexcept;
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
        /** @brief Returns authored elements in semantic parent/sibling order. @return Borrowed immutable element records. */
        [[nodiscard]] std::span<const UiDocumentElement> Elements() const noexcept;
        /** @brief Returns dependencies in canonical AssetId order. @return Borrowed immutable dependencies. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;
        /** @brief Returns route definitions in canonical route identity order. @return Borrowed immutable route metadata. */
        [[nodiscard]] std::span<const UiRouteMetadata> Routes() const noexcept;

    private:
        friend class UiDocumentBuilder;
        struct State;
        /** @brief Adopts validated authored state from UiDocumentBuilder. */
        explicit UiDocument(State state) noexcept;
        UiDocumentSchemaVersion schemaVersion_;                  /**< Durable source schema represented by this snapshot. */
        UiDocumentId id_;                                        /**< Stable authored identity. */
        UiDocumentRevision revision_;                            /**< Authored revision represented by this snapshot. */
        std::vector<UiCanvasDescriptor> canvases_;               /**< Validated authored-order canvases. */
        std::vector<UiDocumentElement> elements_;                /**< Validated authored hierarchy and typed values. */
        std::vector<UiLocalizedText> localizedTexts_;            /**< Authored stable message references. */
        std::vector<UiLocalizedAssetReference> localizedAssets_; /**< Authored localized asset variant sets. */
        std::vector<UiAssetDependency> dependencies_;            /**< Canonical dependency enumeration. */
        std::vector<UiRouteMetadata> routes_;                    /**< Canonical route definitions. */
    };

    /** @brief Load-time authoring builder that publishes only complete validated documents. */
    class UiDocumentBuilder final {
    public:
        /** @brief Starts a bounded authoring transaction. @param id Stable document identity.
         * @param revision Non-zero authored revision.
         */
        UiDocumentBuilder(UiDocumentId id, UiDocumentRevision revision,
                          UiDocumentSchemaVersion schemaVersion = CurrentUiDocumentSchemaVersion) noexcept;
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
        /** @brief Adds one authored element in semantic hierarchy order. @param element Element candidate.
         * @return Success or a bounded/typed validation error; the builder remains unchanged on failure.
         */
        [[nodiscard]] Result<void> AddElement(UiDocumentElement element);
        /** @brief Adds or strengthens one dependency. @param dependency Asset requirement.
         * @return Success, or a typed invalid/conflicting/capacity error.
         */
        [[nodiscard]] Result<void> RequireAsset(UiAssetDependency dependency);
        /** @brief Adds one durable route definition. @param route Route metadata candidate.
         * @return Success or a duplicate/capacity/typed validation error.
         */
        [[nodiscard]] Result<void> AddRoute(UiRouteMetadata route);
        /** @brief Validates and consumes the builder. @return Immutable authored document or typed failure. */
        [[nodiscard]] Result<UiDocument> Build() &&;

    private:
        UiDocumentSchemaVersion schemaVersion_;                  /**< Candidate source schema. */
        UiDocumentId id_;                                        /**< Candidate authored identity. */
        UiDocumentRevision revision_;                            /**< Candidate authored revision. */
        std::vector<UiCanvasDescriptor> canvases_;               /**< Bounded authored-order canvases. */
        std::vector<UiDocumentElement> elements_;                /**< Candidate hierarchy and typed values. */
        std::vector<UiLocalizedText> localizedTexts_;            /**< Bounded authored message references. */
        std::vector<UiLocalizedAssetReference> localizedAssets_; /**< Bounded authored asset references. */
        std::vector<UiAssetDependency> dependencies_;            /**< Bounded canonicalizable dependencies. */
        std::vector<UiRouteMetadata> routes_;                    /**< Candidate route definitions. */
    };

    /** @brief Versioned cooked payload, separate from authoring state and mutable runtime state. */
    class CookedUiDocument final {
    public:
        /**
         * @brief Cooks one validated authored document into deterministic runtime-oriented bytes.
         * @param document Validated immutable authored document.
         * @param limits Bounded output and content limits.
         * @return Owned cooked representation or a typed validation/capacity failure.
         */
        [[nodiscard]] static Result<CookedUiDocument> Cook(const UiDocument &document, const UiDocumentCookLimits &limits = {});

        /**
         * @brief Decodes one bounded deterministic Runtime UI cooked payload.
         * @param payload Untrusted cooked bytes; the input remains borrowed and unchanged.
         * @param limits Bounded decoder and content limits.
         * @return Owned validated cooked representation or a typed malformed/version/capacity failure.
         */
        [[nodiscard]] static Result<CookedUiDocument> Decode(std::span<const std::uint8_t> payload,
                                                             const UiDocumentCookLimits &limits = {});

        /** @brief Creates cooked state from one validated authored revision. @param document Source document.
         * @param payload Non-empty deterministic cooked bytes within the public bound.
         * @return Owned cooked document or typed payload error.
         */
        [[nodiscard]] static Result<CookedUiDocument> Create(const UiDocument &document, std::vector<std::uint8_t> payload);
        /** @brief Returns the cooked document identity. @return Stable authored identity. */
        [[nodiscard]] UiDocumentId Id() const noexcept;
        /** @brief Returns the authored revision used for cooking. @return Source revision. */
        [[nodiscard]] UiDocumentRevision SourceRevision() const noexcept;
        /** @brief Returns the source schema represented by the cooked payload. @return Durable schema version. */
        [[nodiscard]] UiDocumentSchemaVersion SchemaVersion() const noexcept;
        /** @brief Returns immutable canvas descriptors retained for runtime preparation. @return Borrowed canvas data. */
        [[nodiscard]] std::span<const UiCanvasDescriptor> Canvases() const noexcept;
        /** @brief Returns immutable element descriptors retained for runtime preparation. @return Borrowed element data. */
        [[nodiscard]] std::span<const UiDocumentElement> Elements() const noexcept;
        /** @brief Returns the cooked dependency manifest. @return Borrowed immutable dependencies. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;
        /** @brief Returns immutable route descriptors retained for runtime preparation. @return Borrowed route data. */
        [[nodiscard]] std::span<const UiRouteMetadata> Routes() const noexcept;
        /** @brief Returns deterministic cooked bytes. @return Borrowed immutable payload. */
        [[nodiscard]] std::span<const std::uint8_t> Payload() const noexcept;

    private:
        friend class UiRuntimeInstance;
        /** @brief Adopts validated cooked state. */
        CookedUiDocument(UiDocumentSchemaVersion schemaVersion, UiDocumentId id, UiDocumentRevision revision,
                         std::vector<UiCanvasDescriptor> canvases, std::vector<UiDocumentElement> elements,
                         std::vector<UiAssetDependency> dependencies, std::vector<UiRouteMetadata> routes,
                         std::vector<std::uint8_t> payload) noexcept;
        UiDocumentSchemaVersion schemaVersion_;       /**< Source schema represented by this cooked snapshot. */
        UiDocumentId id_;                             /**< Stable authored identity. */
        UiDocumentRevision revision_;                 /**< Source authored revision. */
        std::vector<UiCanvasDescriptor> canvases_;    /**< Runtime-ready immutable canvas descriptors. */
        std::vector<UiDocumentElement> elements_;     /**< Runtime-ready immutable element descriptors. */
        std::vector<UiAssetDependency> dependencies_; /**< Canonical cooked dependency manifest. */
        std::vector<UiRouteMetadata> routes_;         /**< Runtime-ready immutable route descriptors. */
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
        /**
         * @brief Creates a prepared instance while transferring validated dependency payload leases.
         * @param document Owned cooked document transferred into runtime storage.
         * @param instance Valid transient identity issued by the owning service.
         * @param assets Owned immutable dependency leases; optional dependencies may be omitted.
         * @return Prepared instance or a typed dependency/payload failure.
         */
        [[nodiscard]] static Result<UiRuntimeInstance> Create(CookedUiDocument document, RuntimeUiInstanceId instance,
                                                              std::vector<UiRuntimeAsset> assets);
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
        /** @brief Returns immutable cooked canvas descriptors. @return Borrowed canvas data. */
        [[nodiscard]] std::span<const UiCanvasDescriptor> Canvases() const noexcept;
        /** @brief Returns immutable cooked element descriptors. @return Borrowed element data. */
        [[nodiscard]] std::span<const UiDocumentElement> Elements() const noexcept;
        /** @brief Returns the explicit lifecycle state. @return Current owner-thread state. */
        [[nodiscard]] UiRuntimeInstanceState State() const noexcept;
        /** @brief Returns dependencies retained until shutdown. @return Borrowed immutable dependencies. */
        [[nodiscard]] std::span<const UiAssetDependency> Dependencies() const noexcept;
        /** @brief Returns loaded immutable dependency leases. @return Borrowed runtime asset payloads. */
        [[nodiscard]] std::span<const UiRuntimeAsset> ResolvedAssets() const noexcept;
        /** @brief Resolves one loaded dependency without allocating. @param id Stable dependency identity.
         * @return Borrowed payload lease, or null when absent.
         */
        [[nodiscard]] const UiRuntimeAsset *FindAsset(Assets::AssetId id) const noexcept;
        /** @brief Returns immutable cooked route descriptors. @return Borrowed route data. */
        [[nodiscard]] std::span<const UiRouteMetadata> Routes() const noexcept;
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
        struct InitialState final {
            UiDocumentSchemaVersion schemaVersion;
            UiDocumentId document;
            UiDocumentRevision revision;
            std::vector<UiCanvasDescriptor> canvases;
            std::vector<UiDocumentElement> elements;
            std::vector<UiAssetDependency> dependencies;
            std::vector<UiRouteMetadata> routes;
            std::vector<std::uint8_t> payload;
            std::vector<UiRuntimeAsset> assets;
            RuntimeUiInstanceId instance;
        };

        explicit UiRuntimeInstance(InitialState initialState) noexcept;
        UiDocumentSchemaVersion schemaVersion_;                          /**< Source schema represented by this instance. */
        UiDocumentId document_;                                          /**< Stable source document identity. */
        UiDocumentRevision revision_;                                    /**< Exact cooked source revision. */
        std::vector<UiCanvasDescriptor> canvases_;                       /**< Runtime-ready immutable canvases. */
        std::vector<UiDocumentElement> elements_;                        /**< Runtime-ready immutable elements. */
        std::vector<UiAssetDependency> dependencies_;                    /**< Retained dependency manifest. */
        std::vector<UiRouteMetadata> routes_;                            /**< Runtime-ready immutable routes. */
        std::vector<std::uint8_t> payload_;                              /**< Retained cooked representation. */
        std::vector<UiRuntimeAsset> assets_;                             /**< Retained dependency payload leases. */
        RuntimeUiInstanceId instance_;                                   /**< Owner-issued transient identity. */
        UiRuntimeInstanceState state_{UiRuntimeInstanceState::Prepared}; /**< Owner-thread lifecycle state. */
    };
}  // namespace Horo::Runtime::Ui
