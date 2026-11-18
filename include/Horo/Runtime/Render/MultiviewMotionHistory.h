#pragma once
/** @file MultiviewMotionHistory.h
 * @brief Exact per-view and per-object render history for genuine projected frames.
 */
#include "Horo/Runtime/Render/MotionHistory.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"

namespace Horo::Render {
    /** @brief One immutable render presentation and its compositor prediction; display time is in the owner's clock domain. */
    struct MultiviewMotionFrame final {
        MultiviewPublication publication;
        std::uint64_t projectionSequence{};
        std::int64_t predictedDisplayTimeNanoseconds{};
        auto operator<=>(const MultiviewMotionFrame &) const noexcept = default;
    };

    /** @brief Stable predecessors for a current visible object's unjittered projection. */
    struct MultiviewMotionPredecessor final {
        Math::Mat4 worldToClip;
        Math::Mat4 localToWorld;
    };

    /** @brief Exact per-eye clip matrices for one admitted current draw. Culled eyes are not invoked. */
    struct MultiviewMotionDraw final {
        std::array<Math::Mat4, MultiviewExtractionLimits::HardMaximumViews> currentLocalToClip{};
        std::array<Math::Mat4, MultiviewExtractionLimits::HardMaximumViews> previousLocalToClip{};
        std::size_t viewCount{};
    };

    /** @brief One owner-lane history, without native resource or submission authority.
     * @details Stage copies the actual render extraction after candidate validation. Publish is called only for the
     * matching successful native projection completion. No heap allocation or pixel generation is performed here.
     */
    class MultiviewMotionHistory final {
    public:
        MultiviewMotionHistory() = default;
        MultiviewMotionHistory(const MultiviewMotionHistory &) = delete;
        MultiviewMotionHistory &operator=(const MultiviewMotionHistory &) = delete;

        /** @brief Reserves a real candidate over the exact renderer snapshot and canonical object IDs.
         * @param frame Exact native prediction and renderer publication.
         * @param snapshot Immutable actual selected scene with ZeroToOne projection conventions.
         * @param objects Stable object IDs aligned to every extracted instance, not draw or eye indices.
         * @return True for an admitted candidate. Failure clears the attempted candidate and leaves committed history intact.
         */
        [[nodiscard]] bool Stage(const MultiviewMotionFrame &frame, const MultiviewSceneSnapshot &snapshot,
                                 std::span<const RenderMotionObjectId> objects) noexcept;

        /** @brief Observes the exact compatible predecessor while the supplied current publication is staged.
         * @param frame Exact
         * live candidate. @return Prior completed projection identity, or absence on any history break.
         */
        [[nodiscard]] std::optional<MultiviewMotionFrame> PreviousFrame(const MultiviewMotionFrame &frame) const noexcept;
        /** @brief Returns a predecessor only for an exact live candidate and a visible object in the same eye in both frames.
         * @param frame Exact staged frame. @param object Canonical current draw identity. @param view Dense renderer view index.
         * @return Copied predecessor, absent for skipped, stale, changed, missing or culled history.
         */
        [[nodiscard]] std::optional<MultiviewMotionPredecessor> Previous(const MultiviewMotionFrame &frame, RenderMotionObjectId object,
                                                                         std::uint32_t view) const noexcept;

        /** @brief Reads every visible eye's exact current and previous geometry for the canonical instance index.
         * @param frame Exact staged frame. @param instanceIndex Index of the corresponding staged scene instance.
         * @return Copied transforms or absence if any visible eye lacks a genuine predecessor.
         */
        [[nodiscard]] std::optional<MultiviewMotionDraw> ReadDraw(const MultiviewMotionFrame &frame,
                                                                  std::uint32_t instanceIndex) const noexcept;

        /** @brief Advances history after actual successful compositor projection.
         * @param frame Exact staged and submitted candidate. @return True once for a matching staged completion.
         * @pre Caller observed successful native submission of that exact frame; no GPU receipts are manufactured here.
         */
        [[nodiscard]] bool Publish(const MultiviewMotionFrame &frame) noexcept;

        /** @brief Clears candidate and predecessor after zero-layer end, loss, retirement or incompatible evidence. */
        void Reset() noexcept;

    private:
        struct Object final {
            RenderMotionObjectId id;
            RenderMeshHandle mesh;
            Math::Mat4 localToWorld;
            std::uint32_t visibleViews{};
        };

        struct Projection final {
            MultiviewMotionFrame frame;
            std::array<Math::Mat4, MultiviewExtractionLimits::HardMaximumViews> views{};
            std::array<Object, MultiviewExtractionLimits::HardMaximumInstances> objects{};
            std::array<RenderMotionObjectId, MultiviewExtractionLimits::HardMaximumInstances> instanceIds{};
            std::size_t viewCount{};
            std::size_t objectCount{};
        };

        /** @brief Returns an exact identity from a sorted fixed table. */
        [[nodiscard]] static const Object *Find(const Projection &projection, RenderMotionObjectId object) noexcept;
        /** @brief Checks view, selected-device and origin continuity across genuine ordered frame completions. */
        [[nodiscard]] bool Adjacent() const noexcept;

        Projection slots_[2];
        std::size_t writable_{};
        bool staged_{};
        bool published_{};
    };
}  // namespace Horo::Render
