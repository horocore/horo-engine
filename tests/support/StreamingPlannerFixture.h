#pragma once
#include <Horo/WorldStreaming/StreamingSourcePlanner.h>
#include <array>
#include <stdexcept>
#include <vector>

namespace Horo::WorldStreaming::PlannerFixture {
    template <typename T> T Id(const std::uint64_t value) {
        return T::Create(value).Value();
    }

    inline StreamingLayerId Layer(const std::uint16_t value = 1) {
        return StreamingLayerId::Create(value).Value();
    }

    inline WorldPartitionId World() {
        SerializedWorldPartitionId bytes{};
        bytes.back() = 1;
        return WorldPartitionId::Create(bytes).Value();
    }

    inline Math::WorldCoordinate64 Point(const std::int64_t x, const std::int64_t y = 0, const std::int64_t z = 0) {
        return Math::WorldCoordinate64::FromMillimeters(x, y, z);
    }

    inline WorldPartitionRegistrySnapshot Partition(const std::int32_t side = 16, const std::int32_t spacing = 1,
                                                    const std::uint64_t epoch = 1) {
        const auto edge = side * spacing;
        const auto grid = WorldCellQuantizationPolicy::Create(Point(0), 100, {0, edge, 0, edge, 0, 0}, 1).Value();
        const std::array layers{WorldLayerDescriptor{Layer(), "runtime", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::None, 1},
                                WorldLayerDescriptor{Layer(2), "optional", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::Optional,
                                                     1}};
        std::vector<WorldPartitionCellDescriptor> cells;
        for (std::int32_t y = 0; y < side; ++y)
            for (std::int32_t x = 0; x < side; ++x) {
                std::array<std::uint8_t, 16> bytes{};
                bytes[0] = 1;
                const auto index = static_cast<std::uint32_t>(y * side + x + 1);
                for (unsigned b = 0; b < 4; ++b)
                    bytes[12 + b] = static_cast<std::uint8_t>(index >> (b * 8));
                cells.push_back({{x * spacing, y * spacing, 0, 0, Layer()}, {Assets::AssetId::FromBytes(bytes)}});
            }
        auto descriptor = WorldPartitionDescriptor::Create({}, World(), {Point(0), Point(edge * 100 - 1, edge * 100 - 1, 99)}, grid, layers,
                                                           cells, {2, static_cast<std::uint32_t>(cells.size()), 32})
                              .Value();
        const StreamingRuntimeOwnerToken owner{World(), Id<PartitionEpoch>(epoch), Id<StreamingRuntimeOwnerId>(1)};
        auto registry = WorldPartitionRegistry::Create(Id<WorldPartitionRegistryId>(1), owner,
                                                       {static_cast<std::uint32_t>(cells.size()), static_cast<std::uint32_t>(cells.size())})
                            .Value();
        auto published = registry->Publish(std::move(descriptor), Id<WorldPartitionRegistryRevision>(1));
        if (published.HasError())
            throw std::runtime_error("fixture partition publication failed");
        return registry->Snapshot().Value();
    }

    inline StreamingSourcePlannerConfig Config(const std::uint32_t cells = 256) {
        StreamingPriorityPolicyRequest priority;
        priority.id = Id<StreamingPriorityPolicyId>(1);
        priority.revision = Id<StreamingPriorityPolicyRevision>(1);
        StreamingCellStabilityPolicyRequest stability;
        stability.id = Id<StreamingCellStabilityPolicyId>(1);
        stability.revision = Id<StreamingCellStabilityPolicyRevision>(1);
        stability.maximumTrackedCells = cells;
        stability.lingerMilliseconds = 100;
        stability.pressureLingerMilliseconds = 20;
        StreamingPrefetchPolicyRequest prefetch;
        prefetch.id = Id<StreamingPrefetchPolicyId>(1);
        prefetch.revision = Id<StreamingPrefetchPolicyRevision>(1);
        prefetch.pathHalfExtentMillimeters = 20;
        return {{8, cells, std::min(65'536U, cells * 8), 2, 8, 8, std::min(cells, 8U), 2'000},
                {true, true, true, true},
                StreamingPriorityPolicy::Create(priority).Value(),
                StreamingCellStabilityPolicy::Create(stability).Value(),
                StreamingPrefetchPolicy::Create(prefetch).Value()};
    }

    inline StreamingSourceEnvironment Environment(const std::uint64_t revision = 1) {
        static const std::array enabled{Layer()};
        return {revision, {1}, 1, 3, enabled};
    }

    inline std::unique_ptr<StreamingSourcePlanner> Planner(const WorldPartitionRegistrySnapshot &partition,
                                                           const std::uint32_t cells = 256) {
        return StreamingSourcePlanner::Create(Config(cells), partition, Environment()).Value();
    }

    inline StreamingSourceRequest Source(const std::uint64_t id = 1, const StreamingSourceIntent intent = StreamingSourceIntent::Gameplay,
                                         const std::int64_t x = 0, const std::int64_t extent = 199) {
        return {.id = Id<StreamingSourceId>(id),
                .revision = Id<StreamingSourceRevision>(1),
                .intent = intent,
                .priority = StreamingSourcePriority::Create(1).Value(),
                .shape = StreamingSourceBox{Point(x), Point(x + extent, 99, 99)}};
    }

    inline StreamingSourcePlanView Finish(StreamingSourcePlanner &planner, const std::uint64_t time = 1'000) {
        for (unsigned step = 0; step < 1'000'000; ++step) {
            const auto result = planner.Service(time);
            if (result.HasError())
                throw std::runtime_error(result.ErrorValue().message);
            if (result.Value().published)
                return planner.Plan().Value();
        }
        throw std::runtime_error("planner made no bounded progress");
    }
}  // namespace Horo::WorldStreaming::PlannerFixture
