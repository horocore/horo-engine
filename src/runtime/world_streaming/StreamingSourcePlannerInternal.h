#pragma once
#include "Horo/WorldStreaming/StreamingSourcePlanner.h"

#include <thread>
#include <vector>

namespace Horo::WorldStreaming {
    /** @brief Preallocated explicit owner-lane storage retained only by the planner and its RAII registrations. */
    struct StreamingSourcePlannerState final {
        struct Source final {
            std::optional<StreamingSourceRequest> request{};
            std::uint32_t generation{};
            std::optional<StreamingSourceMotionSample> motion;
            std::optional<StreamingSourceRequest> captured;
            std::uint64_t capturedTime{};
            std::uint64_t capturedServiceTime{};
        };

        StreamingSourcePlannerConfig config;
        WorldPartitionRegistrySnapshot partition;
        StreamingSourceEnvironment environment{};
        std::vector<StreamingLayerId> enabledLayers;
        std::vector<Source> sources;
        std::vector<std::uint32_t> sourceOrder;
        std::vector<StreamingPlannedCell> staged;
        std::vector<StreamingPlannedCell> published;
        std::vector<StreamingPlannedCell> nextPublished;

        struct Lookup final {
            std::uint64_t cycle{};
            std::uint32_t row{};
        };

        std::vector<Lookup> lookup;
        std::vector<std::uint32_t> readyHeap;
        std::uint64_t cycleSequence{};
        StreamingCellStabilityPressure cyclePressure{};
        std::vector<StreamingPlannedSourceCell> stagedContributions;
        std::vector<StreamingPlannedSourceCell> publishedContributions;
        std::vector<StreamingPlannedSourceCell> nextPublishedContributions;
        std::thread::id lane{std::this_thread::get_id()};
        std::uint64_t revision{1};
        std::uint64_t publishedRevision{};
        std::uint64_t planSequence{};
        std::uint64_t lastServiceTime{};
        std::uint64_t cycleTime{};
        std::uint64_t publishedTime{};
        std::uint64_t oldestCycleSampleTime{};
        std::size_t sourceSlot{};
        std::size_t historySlot{};
        bool closed{};
        bool cycleActive{};
        bool predicting{};
        std::optional<WorldPartitionSpatialCursor> cursor;
        std::optional<StreamingSourceShape> queryShape;

        StreamingSourcePlannerState(StreamingSourcePlannerConfig configValue, WorldPartitionRegistrySnapshot partitionValue)
            : config(std::move(configValue)), partition(std::move(partitionValue)) {}

        [[nodiscard]] bool OnLane() const noexcept {
            return lane == std::this_thread::get_id();
        }

        [[nodiscard]] StreamingSourceOwnerToken Token(std::size_t slot) const noexcept;
        [[nodiscard]] StreamingSourceDescriptor Descriptor(std::size_t slot) const noexcept;
        [[nodiscard]] Result<void> ValidateEnvironment(const StreamingSourceEnvironment &value,
                                                       const WorldPartitionRegistrySnapshot &snapshot) const;
        [[nodiscard]] Result<void> ValidateRequest(const StreamingSourceRequest &request, const StreamingSourceOwnerToken &owner,
                                                   std::optional<StreamingSourceRevision> previous = {}) const;
        [[nodiscard]] Result<void> ValidateOwner(const StreamingSourceOwnerToken &owner) const;
        void Invalidate() noexcept;
        [[nodiscard]] bool Selected(const StreamingSourceRequest &request) const noexcept;
        [[nodiscard]] Result<void> Accumulate(std::size_t slot, const WorldPartitionCellHandle &cell, const StreamingSourceShape &shape,
                                              bool prediction);
        [[nodiscard]] Result<void> CaptureSource(std::size_t slot);
        [[nodiscard]] Result<void> AdmitContribution(std::size_t slot, const WorldPartitionCellHandle &cell,
                                                     const StreamingSourceDescriptor &descriptor, bool prediction);
        [[nodiscard]] Result<void> RankRow(std::size_t slot, StreamingPlannedCell &row, const StreamingSourceDescriptor &descriptor);
        void CompleteSourceTraversal();
        [[nodiscard]] Result<void> BeginSource(std::size_t slot, bool prediction);
        [[nodiscard]] Result<std::size_t> AdmitRow(const StreamingPlannedCell &row);
        [[nodiscard]] bool HeapLess(std::uint32_t left, std::uint32_t right) const noexcept;
        void BeginCycle(std::uint64_t time, StreamingCellStabilityPressure pressure) noexcept;
        [[nodiscard]] Result<void> StepSources(StreamingSourcePlannerProgress &progress);
        [[nodiscard]] Result<void> StepHistory(StreamingSourcePlannerProgress &progress);
        [[nodiscard]] Result<void> StepOutput(StreamingSourcePlannerProgress &progress);
        [[nodiscard]] Result<void> Stabilize(StreamingPlannedCell &row, StreamingCellStabilityPressure pressure);
    };

    namespace SourcePlanning {
        [[nodiscard]] Result<StreamingSourceShape> Reposition(const StreamingSourceShape &shape, const Math::WorldCoordinate64 &anchor);

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }
    }  // namespace SourcePlanning
}  // namespace Horo::WorldStreaming
