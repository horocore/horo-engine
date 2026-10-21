#pragma once

/** @file UiLayoutInspector.h
 * @brief Optional copied committed tree/layout/clip/profile inspection, independent of input authority.
 */
#include "Horo/Runtime/Ui/UiLayoutClipping.h"
#include "Horo/Runtime/Ui/UiPresentationProfiles.h"

namespace Horo::Runtime::Ui {
    /** @brief Fixed independent observer storage reserved before ordinary frame execution. */
    struct UiLayoutInspectorLimits final {
        std::uint32_t elements{};
        std::uint32_t clips{};
        std::uint32_t scrolls{};
        std::uint32_t snapshots{}; /**< At least two; never consumes an ordinary layout/frame slot. */
        /** @brief Checks positive finite compiled ceilings. @return Whether all capacities are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Closed observation outcome that must never alter ordinary publication or input. */
    enum class UiLayoutInspectionStatus : std::uint8_t {
        Captured,
        Disabled,
        DroppedStorage,
        DroppedCapacity,
        RejectedSource,
        Stopped
    };
    /** @brief Closed source rejection reason, preserving diagnostics without a dynamic string. */
    enum class UiLayoutInspectionRejection : std::uint8_t {
        None,
        WrongThread,
        Tree,
        LayoutLineage,
        LayoutMembership,
        ClipLineage,
        ClipMembership,
        SelectionLineage
    };

    /** @brief Non-authoritative observation outcome; only Captured replaces Latest. */
    struct UiLayoutInspectionResult final {
        UiLayoutInspectionStatus status{UiLayoutInspectionStatus::Disabled};
        UiLayoutInspectionRejection rejection{UiLayoutInspectionRejection::None};
        [[nodiscard]] auto operator<=>(const UiLayoutInspectionResult &) const noexcept = default;
    };

    /** @brief Copied hierarchy and evaluated geometry of one exact committed element. */
    struct UiLayoutInspectionRecord final {
        UiElementId id;
        UiElementHandle parent; /**< Exact parent at capture; invalid only for the root. */
        UiLayoutRecord layout;
    };

    /** @brief Exact committed inspection lineage, distinct from successfully presented input authority. */
    struct UiLayoutInspectionDescriptor final {
        UiCanvasId canvas;
        UiLayoutSnapshotDescriptor layout;
        bool clipping{};
        std::optional<UiPresentationProfileSelection> selection;
    };

    class UiLayoutInspector;

    /** @brief Independent immutable copied lease; contains no owner, mutable tree, callback or native object. */
    class UiLayoutInspectionSnapshot final {
    public:
        /** @brief Borrows exact copied publication lineage. @return Descriptor valid for this lease lifetime. */
        [[nodiscard]] const UiLayoutInspectionDescriptor &Descriptor() const noexcept;
        /** @brief Borrows authored-preorder hierarchy and layout. @return Immutable copied rows. */
        [[nodiscard]] std::span<const UiLayoutInspectionRecord> Records() const noexcept;
        /** @brief Borrows source-aligned copied clipping. @return Empty when clipping was absent. */
        [[nodiscard]] std::span<const UiLayoutClipRecord> Clipping() const noexcept;
        /** @brief Borrows copied ancestor-order clip nodes. @return Immutable nodes. */
        [[nodiscard]] std::span<const UiLayoutClipNode> Clips() const noexcept;
        /** @brief Borrows copied scroll bounds/offsets. @return Immutable scroll rows. */
        [[nodiscard]] std::span<const UiLayoutScrollRecord> Scrolls() const noexcept;

    private:
        struct Storage;
        friend class UiLayoutInspector;
        /** @brief Retains only an independent immutable observer slot. */
        explicit UiLayoutInspectionSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /**
     * @brief Optional owner-thread copied observer of actual committed publications.
     * @details Create/Rebind/Enable/Capture/Latest/Shutdown and destruction are serialized on the creating thread.
     * Retained immutable snapshot leases may be read after reload, shutdown or observer destruction. Their contents
     * describe committed evidence, never last-presented input eligibility or a mutable command capability.
     * Capture allocates no fallback, invokes no callback, and reports observation-only drops/rejections. Its outcome
     * must never fail layout, input or whole-frame publication. Source arguments are synchronous borrows only.
     */
    class UiLayoutInspector final {
    public:
        /** @brief Preallocates independent bounded storage, initially disabled.
         * @param generation Actual admitted source. @param canvas Exact authored canvas. @param limits Finite observer capacities.
         * @return Disabled observer or typed source/capacity failure.
         */
        [[nodiscard]] static Result<UiLayoutInspector> Create(const UiReloadGeneration &generation, UiCanvasId canvas,
                                                              UiLayoutInspectorLimits limits);
        /** @brief Closes observation and releases mutable storage; independent immutable leases survive. */
        ~UiLayoutInspector();
        /** @brief Transfers observer ownership on its creating thread. @param other Observer to invalidate and transfer. */
        UiLayoutInspector(UiLayoutInspector &&other) noexcept;
        /** @brief Closes this observer and transfers another on their creating thread.
         * @param other Observer to invalidate and transfer. @return This observer.
         */
        UiLayoutInspector &operator=(UiLayoutInspector &&other) noexcept;
        UiLayoutInspector(const UiLayoutInspector &) = delete;
        UiLayoutInspector &operator=(const UiLayoutInspector &) = delete;
        /** @brief Enables or disables observation without clearing retained evidence. @param enabled Capture choice. */
        void Enable(bool enabled) noexcept;
        /** @brief Rebinds to an actual replacement in the same static document/instance scope.
         * @param generation Admitted replacement. @param canvas Same authored canvas.
         * @return Admission or typed failure preserving previous binding and all leases; document rollback rejects.
         */
        [[nodiscard]] Result<void> Rebind(const UiReloadGeneration &generation, UiCanvasId canvas);
        /** @brief Copies a coherent committed publication only after complete source/capacity validation.
         * @param tree Current admitted tree. @param layout Matching live immutable layout lease (not moved from).
         * @param clipping Optional exact matching projection. @param selection Optional actual owner-selected profile evidence.
         * @return Explicit observation outcome; disabled/dropped/rejected captures leave Latest unchanged.
         */
        [[nodiscard]] UiLayoutInspectionResult Capture(const UiElementTree &tree, const UiLayoutSnapshot &layout,
                                                       const UiLayoutClipSnapshot *clipping = nullptr,
                                                       const UiPresentationProfileSelection *selection = nullptr) noexcept;
        /** @brief Retains the latest successful copied observation. @return Empty before first capture or after shutdown. */
        [[nodiscard]] std::optional<UiLayoutInspectionSnapshot> Latest() const noexcept;
        /** @brief Closes capture admission before releasing mutable observer ownership; external copied leases survive. */
        void Shutdown() noexcept;

    private:
        struct Storage;
        /** @brief Validates all source lineage and capacities before any candidate slot mutation; Captured means admitted. */
        [[nodiscard]] UiLayoutInspectionResult ValidateCaptureSources(const UiElementTree &tree, const UiLayoutSnapshot &layout,
                                                                      const UiLayoutClipSnapshot *clipping,
                                                                      const UiPresentationProfileSelection *selection) const noexcept;
        /** @brief Validates exact preorder membership into preallocated private scratch without changing a snapshot slot. */
        [[nodiscard]] bool PrepareCaptureHierarchy(const UiElementTree &tree, std::span<const UiLayoutRecord> records) noexcept;
        /** @brief Adopts fully allocated isolated observer state. */
        explicit UiLayoutInspector(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
