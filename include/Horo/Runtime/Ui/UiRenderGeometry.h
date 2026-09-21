#pragma once

/**
 * @file UiRenderGeometry.h
 * @brief Bounded backend-neutral Runtime UI geometry generation and paint batching.
 */

#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Runtime::Ui {
    /** @brief Absolute per-plan vertex ceiling for Runtime UI geometry. */
    inline constexpr std::uint32_t MaximumUiRenderGeometryVertices = 262'144;
    /** @brief Absolute per-plan index ceiling for Runtime UI geometry. */
    inline constexpr std::uint32_t MaximumUiRenderGeometryIndices = 393'216;
    /** @brief Absolute per-plan contiguous paint-batch ceiling. */
    inline constexpr std::uint32_t MaximumUiRenderGeometryBatches = 16'384;
    /** @brief Absolute number of immutable geometry plans that may remain leased. */
    inline constexpr std::uint32_t MaximumUiRenderGeometryPlansInFlight = 64;

    /** @brief Backend-neutral primitive family emitted by one Runtime UI draw payload. */
    enum class UiRenderGeometryPrimitive : std::uint8_t {
        SolidRectangle,
        BorderRectangle,
        ImageRectangle,
        SpriteRectangle,
        TextGlyphs,
    };

    /** @brief One logical vertex in 1/64-DIP coordinates and normalized source coordinates. */
    struct UiRenderVertex final {
        float x{};                              /**< Logical horizontal position in 1/64-DIP units. */
        float y{};                              /**< Logical vertical position in 1/64-DIP units. */
        float u{};                              /**< Normalized source horizontal coordinate. */
        float v{};                              /**< Normalized source vertical coordinate. */
        UiLinearColor color;                    /**< Per-vertex linear RGBA color after command opacity. */

        /** @brief Checks finite position, source coordinates, and color representation. @return Whether the vertex is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiRenderVertex &) const noexcept = default;
    };

    /** @brief Paint and projection state that must match for two adjacent commands to share a batch. */
    struct UiRenderGeometryBatchKey final {
        UiRenderGeometryPrimitive primitive{UiRenderGeometryPrimitive::SolidRectangle}; /**< Primitive pipeline family. */
        std::uint32_t resource{NoUiRenderIndex}; /**< Snapshot resource index, or sentinel for untextured paint. */
        std::uint32_t transform{};               /**< Snapshot transform table index. */
        std::uint32_t clip{NoUiRenderIndex};     /**< Snapshot clip table index, or sentinel. */
        std::uint32_t mask{NoUiRenderIndex};     /**< Snapshot mask table index, or sentinel. */

        /** @brief Checks only the closed primitive and sentinel representation. @return Whether the key is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiRenderGeometryBatchKey &) const noexcept = default;
    };

    /** @brief One contiguous authored-order range of generated indexed geometry. */
    struct UiRenderGeometryBatch final {
        UiRenderGeometryBatchKey key; /**< Compatible paint/projection state shared by this batch. */
        std::uint32_t firstVertex{};  /**< First vertex owned by the batch. */
        std::uint32_t vertexCount{};  /**< Number of vertices owned by the batch. */
        std::uint32_t firstIndex{};   /**< First index owned by the batch. */
        std::uint32_t indexCount{};   /**< Number of indices owned by the batch. */
        std::uint32_t firstCommand{}; /**< First authored command represented by the batch. */
        std::uint32_t commandCount{}; /**< Contiguous authored command count represented by the batch. */

        /** @brief Validates ranges against one complete generated plan. @return Whether the batch is representable. */
        [[nodiscard]] bool IsValid(std::size_t vertexCount, std::size_t indexCount, std::size_t commandCount) const noexcept;
        [[nodiscard]] auto operator<=>(const UiRenderGeometryBatch &) const noexcept = default;
    };

    /** @brief Finite capacities reserved for every immutable geometry-plan slot. */
    struct UiRenderGeometryLimits final {
        std::uint32_t vertices{}; /**< Maximum generated vertices per plan. */
        std::uint32_t indices{};  /**< Maximum generated indices per plan. */
        std::uint32_t batches{};  /**< Maximum contiguous batches per plan. */

        /** @brief Validates non-zero capacities against repository hard ceilings. @return Whether the limits are supported. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiRenderGeometryLimits &) const noexcept = default;
    };

    /** @brief Exact view and capacity admission for one owner-thread geometry arena. */
    struct UiRenderGeometryArenaDescriptor final {
        UiRenderViewId view;                 /**< Exact Horo-owned view incarnation. */
        UiRenderGeometryLimits limits;       /**< Per-plan geometry capacities reserved at creation. */
        std::uint32_t concurrentPlans{};     /**< Number of immutable plan slots reserved at creation. */

        /** @brief Validates view identity, capacities, and the bounded slot count. @return Whether the arena can be created. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact immutable source and generated-count evidence for one geometry plan. */
    struct UiRenderGeometryPlanDescriptor final {
        UiRenderViewId view;                       /**< Exact view that owns the plan. */
        UiRenderSnapshotRevision snapshotRevision; /**< Exact source snapshot generation. */
        std::uint32_t commandCount{};              /**< Authored draw-command count represented by the plan. */
        std::uint32_t vertexCount{};               /**< Generated vertex count. */
        std::uint32_t indexCount{};                /**< Generated index count. */
        std::uint32_t batchCount{};                /**< Generated contiguous batch count. */
    };

    /** @brief Allocation-free geometry capacity and pressure evidence for one arena. */
    struct UiRenderGeometryStatistics final {
        std::uint32_t capacityVertices{}; /**< Configured vertex capacity per plan. */
        std::uint32_t capacityIndices{};  /**< Configured index capacity per plan. */
        std::uint32_t capacityBatches{}; /**< Configured batch capacity per plan. */
        std::uint32_t usedVertices{};     /**< Vertices held by currently leased plans. */
        std::uint32_t usedIndices{};      /**< Indices held by currently leased plans. */
        std::uint32_t usedBatches{};      /**< Batches held by currently leased plans. */
        std::uint32_t peakVertices{};     /**< Lifetime high-water vertex count. */
        std::uint32_t peakIndices{};      /**< Lifetime high-water index count. */
        std::uint32_t peakBatches{};      /**< Lifetime high-water batch count. */
        std::uint64_t failedBuilds{};     /**< Lifetime count of rejected or exhausted build attempts. */
        std::uint32_t activeLeases{};     /**< Immutable plan copies currently pinning slots. */
        bool accepting{};                 /**< False after Close(). */

        [[nodiscard]] constexpr auto operator<=>(const UiRenderGeometryStatistics &) const noexcept = default;
    };

    /** @brief Explicit admission lifecycle for one view-owned geometry arena. */
    enum class UiRenderGeometryArenaState : std::uint8_t {
        Active,
        Closed,
    };

    class UiRenderGeometryArena;

    /**
     * @brief Immutable indexed Runtime UI geometry with authored-order batch provenance.
     * @details The plan retains its source snapshot lease, so resource, clip, mask, and transform table indexes remain valid
     *          for the complete plan lifetime. Adjacent compatible commands are coalesced; non-adjacent commands never move
     *          across one another to obtain a larger batch.
     */
    class UiRenderGeometryPlan final {
    public:
        /** @brief Releases this plan's immutable geometry-slot lease. */
        ~UiRenderGeometryPlan();
        /** @brief Copies one immutable plan and retains its exact slot. @param other Live plan to retain. */
        UiRenderGeometryPlan(const UiRenderGeometryPlan &other) noexcept;
        /** @brief Replaces this plan by a retained copy. @param other Live plan. @return This plan. */
        UiRenderGeometryPlan &operator=(const UiRenderGeometryPlan &other) noexcept;
        /** @brief Transfers one immutable plan lease. @param other Plan whose lease is transferred. */
        UiRenderGeometryPlan(UiRenderGeometryPlan &&other) noexcept;
        /** @brief Replaces this plan by transfer. @param other Plan to transfer. @return This plan. */
        UiRenderGeometryPlan &operator=(UiRenderGeometryPlan &&other) noexcept;

        /** @brief Returns exact view, source revision, and generated-count evidence. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiRenderGeometryPlanDescriptor &Descriptor() const noexcept;
        /** @brief Returns the immutable source snapshot retained by this plan. @return Borrowed source snapshot. */
        [[nodiscard]] const UiRenderSnapshot &SourceSnapshot() const noexcept;
        /** @brief Returns generated vertices in plan order. @return Borrowed immutable vertices. */
        [[nodiscard]] std::span<const UiRenderVertex> Vertices() const noexcept;
        /** @brief Returns generated indices in plan order. @return Borrowed immutable indices. */
        [[nodiscard]] std::span<const std::uint32_t> Indices() const noexcept;
        /** @brief Returns contiguous batches in authored paint order. @return Borrowed immutable batches. */
        [[nodiscard]] std::span<const UiRenderGeometryBatch> Batches() const noexcept;

    private:
        struct Storage;
        friend class UiRenderGeometryArena;
        /** @brief Adopts one already leased immutable geometry slot. */
        explicit UiRenderGeometryPlan(std::shared_ptr<const Storage> storage) noexcept;
        /** @brief Retains the current immutable geometry slot. */
        void Retain() const noexcept;
        /** @brief Releases the current slot and makes it reusable after the final lease. */
        void Release() noexcept;

        std::shared_ptr<const Storage> storage_;
    };

    /**
     * @brief Owner-thread preallocated frame arena for Runtime UI geometry generation and batching.
     * @details Creation reserves every vertex, index, batch, and plan slot. Build performs a bounded command scan without I/O,
     *          blocking, fallback allocation, or reordering. Close stops admission while outstanding plans retain their source
     *          snapshot and geometry storage until the final immutable lease retires.
     */
    class UiRenderGeometryArena final {
    public:
        /**
         * @brief Creates a view-owned arena with all frame-hot storage reserved.
         * @param descriptor Exact view, capacities, and concurrent immutable-plan bound.
         * @return Active arena or a typed identity/capacity/allocation failure.
         */
        [[nodiscard]] static Result<UiRenderGeometryArena> Create(const UiRenderGeometryArenaDescriptor &descriptor);
        /** @brief Closes admission while preserving leased plans. */
        ~UiRenderGeometryArena();
        /** @brief Transfers unique arena ownership. @param other Arena whose ownership is transferred. */
        UiRenderGeometryArena(UiRenderGeometryArena &&other) noexcept;
        /** @brief Replaces this arena by transfer. @param other Arena to transfer. @return This arena. */
        UiRenderGeometryArena &operator=(UiRenderGeometryArena &&other) noexcept;
        UiRenderGeometryArena(const UiRenderGeometryArena &) = delete;
        UiRenderGeometryArena &operator=(const UiRenderGeometryArena &) = delete;

        /**
         * @brief Generates indexed geometry and contiguous compatible batches from one immutable snapshot.
         * @param snapshot Exact per-view source snapshot; the returned plan retains its own immutable lease.
         * @return Complete plan or a typed view, lifecycle, geometry, capacity, or slot-exhaustion failure.
         * @pre Calls for one arena are serialized on its owner thread.
         */
        [[nodiscard]] Result<UiRenderGeometryPlan> Build(const UiRenderSnapshot &snapshot);
        /** @brief Stops new plans without invalidating existing plan or source-snapshot leases. */
        void Close() noexcept;
        /** @brief Returns whether every geometry plan slot and retained source lease has drained. @return True when drained. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns current arena admission state. @return Active or Closed. */
        [[nodiscard]] UiRenderGeometryArenaState State() const noexcept;
        /** @brief Returns bounded capacity, usage, peak, and failure evidence without allocating. @return Immutable statistics. */
        [[nodiscard]] UiRenderGeometryStatistics Statistics() const noexcept;

    private:
        struct Storage;
        explicit UiRenderGeometryArena(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
