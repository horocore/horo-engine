#pragma once

/**
 * @file UiLayoutClipping.h
 * @brief Immutable Runtime UI clip chains, scroll extents, and bring-into-view projection.
 */

#include "Horo/Runtime/Ui/UiFocusGraph.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t NoUiLayoutClip = std::numeric_limits<std::uint32_t>::max();
    inline constexpr std::uint32_t NoUiLayoutScroll = std::numeric_limits<std::uint32_t>::max();
    inline constexpr std::uint32_t MaximumUiLayoutClipNodes = MaximumUiTreeElements;
    inline constexpr std::uint32_t MaximumUiLayoutScrollContainers = MaximumUiTreeElements;
    inline constexpr std::uint32_t MaximumUiLayoutClipSnapshotsInFlight = 64;

    /** @brief Closed descendant overflow behavior owned by Runtime UI layout. */
    enum class UiLayoutOverflowPolicy : std::uint8_t {
        Visible,
        Clip,
        Scroll,
        Count,
    };

    /** @brief Typed overflow and current scroll offset for one arranged element. */
    struct UiLayoutClipDescriptor final {
        UiElementHandle element;                                          /**< Exact current retained-tree element. */
        UiLayoutOverflowPolicy overflow{UiLayoutOverflowPolicy::Visible}; /**< Descendant overflow policy. */
        UiLogicalPoint scrollOffset; /**< Current signed logical scroll translation; meaningful only for Scroll. */

        /** @brief Validates the element and policy-specific offset representation. @return Whether the descriptor is admissible. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutClipDescriptor &) const noexcept = default;
    };

    /** @brief One immutable logical clip node in ancestor order. */
    struct UiLayoutClipNode final {
        UiElementHandle element;              /**< Element whose descendant content establishes this clip. */
        UiLogicalRect rect;                   /**< Canvas-space clip rectangle after outer scroll translation. */
        std::uint32_t parent{NoUiLayoutClip}; /**< Earlier ancestor clip, or the absent sentinel. */

        /** @brief Validates the node representation without consulting a snapshot table. @return Whether the node is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutClipNode &) const noexcept = default;
    };

    /** @brief Resolved content bounds and clamped offset for one scroll container. */
    struct UiLayoutScrollRecord final {
        UiElementHandle element;      /**< Exact scroll-container element. */
        UiLogicalRect viewport;       /**< Canvas-space viewport after outer scroll translation. */
        UiLogicalRect content;        /**< Unscrolled canvas-space descendant bounds including the viewport. */
        UiLogicalPoint offset;        /**< Applied translation is the negation of this offset. */
        UiLogicalPoint minimumOffset; /**< Inclusive lower offset bound, possibly negative for leading overflow. */
        UiLogicalPoint maximumOffset; /**< Inclusive upper offset bound. */

        /** @brief Validates bounds and the clamped current offset. @return Whether the record is publishable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutScrollRecord &) const noexcept = default;
    };

    /** @brief Per-element projection linking arranged content to clip and scroll tables. */
    struct UiLayoutClipRecord final {
        UiElementHandle element;                /**< Exact element in the source layout snapshot. */
        UiLogicalPoint scrollTranslation;       /**< Accumulated negated offsets from scroll ancestors. */
        std::uint32_t clip{NoUiLayoutClip};     /**< Clip chain applying to this element's own paint/interaction. */
        std::uint32_t ownClip{NoUiLayoutClip};  /**< Clip node established for this element's descendants. */
        std::uint32_t scroll{NoUiLayoutScroll}; /**< Scroll table entry owned by this element, when applicable. */

        /** @brief Validates element and scalar representation before table-reference checks. @return Whether the record is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutClipRecord &) const noexcept = default;
    };

    /** @brief Exact immutable lineage for one derived clip/scroll publication. */
    struct UiLayoutClipSnapshotDescriptor final {
        RuntimeUiInstanceId instance;      /**< Exact runtime UI instance. */
        UiCanvasInstanceId canvas;         /**< Exact canvas incarnation. */
        UiDocumentId document;             /**< Stable source document. */
        UiLayoutSourceRevisions sources;   /**< Complete source revisions consumed by layout. */
        UiInteractionRevision interaction; /**< Exact layout/interaction generation. */

        /** @brief Validates owner identity and complete source lineage. @return Whether the descriptor is publishable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLayoutClipSnapshotDescriptor &) const noexcept = default;
    };

    /** @brief Source-aligned clip policy span and an optional generation-fenced reveal request. */
    struct UiLayoutClipUpdateRequest final {
        std::span<const UiLayoutClipDescriptor> elements;         /**< One descriptor per layout record in identical preorder. */
        std::optional<UiFocusBringIntoViewRequest> bringIntoView; /**< Optional focus-graph scroll transaction. */
    };

    /** @brief Fixed capacities for one owner-thread clip and scroll projector. */
    struct UiLayoutClipEngineDescriptor final {
        RuntimeUiInstanceId instance;        /**< Exact runtime UI instance owner. */
        UiCanvasInstanceId canvas;           /**< Exact canvas owner. */
        UiDocumentId document;               /**< Stable source document. */
        std::uint32_t elementCapacity{};     /**< Maximum source layout records. */
        std::uint32_t clipCapacity{};        /**< Maximum clip nodes retained per publication. */
        std::uint32_t scrollCapacity{};      /**< Maximum scroll records retained per publication. */
        std::uint32_t concurrentSnapshots{}; /**< Preallocated immutable publication slots. */

        /** @brief Validates owner identities and every bounded storage capacity. @return Whether creation is safe. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    enum class UiLayoutClipEngineState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    class UiLayoutClipEngine;

    /** @brief Immutable leased clip-chain and scroll projection derived from one layout generation. */
    class UiLayoutClipSnapshot final {
    public:
        /** @brief Releases this immutable publication lease. */
        ~UiLayoutClipSnapshot();
        /** @brief Retains one immutable publication. @param other Live snapshot to retain. */
        UiLayoutClipSnapshot(const UiLayoutClipSnapshot &other) noexcept;
        /** @brief Replaces this lease with a retained publication. @param other Live snapshot. @return This snapshot. */
        UiLayoutClipSnapshot &operator=(const UiLayoutClipSnapshot &other) noexcept;
        /** @brief Transfers one immutable publication lease. @param other Snapshot to transfer. */
        UiLayoutClipSnapshot(UiLayoutClipSnapshot &&other) noexcept;
        /** @brief Replaces this lease by transfer. @param other Snapshot to transfer. @return This snapshot. */
        UiLayoutClipSnapshot &operator=(UiLayoutClipSnapshot &&other) noexcept;

        /** @brief Returns exact source and interaction lineage. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiLayoutClipSnapshotDescriptor &Descriptor() const noexcept;
        /** @brief Returns one record per source layout element in preorder. @return Borrowed immutable records. */
        [[nodiscard]] std::span<const UiLayoutClipRecord> Records() const noexcept;
        /** @brief Returns clip nodes in ancestor-before-descendant order. @return Borrowed immutable clip nodes. */
        [[nodiscard]] std::span<const UiLayoutClipNode> Clips() const noexcept;
        /** @brief Returns resolved scroll records in source preorder. @return Borrowed immutable scroll records. */
        [[nodiscard]] std::span<const UiLayoutScrollRecord> Scrolls() const noexcept;
        /** @brief Finds one source element's clip projection. @param element Exact current handle. @return Record or stale failure. */
        [[nodiscard]] Result<UiLayoutClipRecord> Get(UiElementHandle element) const;

    private:
        struct Storage;
        friend class UiLayoutClipEngine;
        explicit UiLayoutClipSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Owner-thread transactional projector for immutable clipping, scrolling, and bring-into-view state. */
    class UiLayoutClipEngine final {
    public:
        /**
         * @brief Creates a projector with all frame-hot tables and snapshot slots reserved.
         * @param descriptor Exact owner identities and bounded capacities.
         * @return Active projector or a typed identity/capacity/allocation failure.
         */
        [[nodiscard]] static Result<UiLayoutClipEngine> Create(const UiLayoutClipEngineDescriptor &descriptor);
        /** @brief Closes admission and releases the engine-owned current publication. */
        ~UiLayoutClipEngine();
        /** @brief Transfers unique projector ownership. @param other Projector whose ownership is transferred. */
        UiLayoutClipEngine(UiLayoutClipEngine &&other) noexcept;
        /** @brief Replaces this projector by transfer. @param other Projector to transfer. @return This projector. */
        UiLayoutClipEngine &operator=(UiLayoutClipEngine &&other) noexcept;
        UiLayoutClipEngine(const UiLayoutClipEngine &) = delete;
        UiLayoutClipEngine &operator=(const UiLayoutClipEngine &) = delete;

        /**
         * @brief Publishes one complete clip/scroll generation from an immutable layout snapshot.
         * @param tree Exact active retained tree used for ancestor relationships.
         * @param layout Exact immutable arranged generation; it is never mutated.
         * @param request Source-aligned policies and optional focused-target reveal request.
         * @return Complete immutable projection or a typed stale, malformed, capacity, or lifecycle failure; prior publication survives
         * failure.
         * @pre Calls for one projector are serialized on the Runtime UI owner thread.
         */
        [[nodiscard]] Result<UiLayoutClipSnapshot> Update(const UiElementTree &tree, const UiLayoutSnapshot &layout,
                                                          const UiLayoutClipUpdateRequest &request);
        /** @brief Closes new updates while preserving external immutable snapshot leases. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the projector and releases mutable storage. */
        void Shutdown() noexcept;
        /** @brief Returns the explicit admission lifecycle. @return Active, Retiring, or Stopped. */
        [[nodiscard]] UiLayoutClipEngineState State() const noexcept;
        /** @brief Reports whether all external immutable snapshot leases have drained. @return True when drained. */
        [[nodiscard]] bool IsDrained() const noexcept;

    private:
        struct Storage;
        explicit UiLayoutClipEngine(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
