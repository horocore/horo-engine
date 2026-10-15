#include "Horo/WorldCook/WorldCookPublication.h"

#include <array>
#include <chrono>

namespace {
    using namespace Horo;
    namespace W = WorldStreaming;
    namespace C = WorldCook;

    /** @brief Owns only this consumer's uniquely created output directory through the real durable call. */
    struct Output final {
        std::filesystem::path root;
        bool created{};

        ~Output() {
            if (created) {
                std::error_code ignored;
                std::filesystem::remove_all(root, ignored);
            }
        }
    };

    /** @brief Independent deterministic persistent asset identities used by the public consumer. */
    Assets::AssetId Asset(const std::uint8_t value) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.front() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    /** @brief Exercises the full public publication facade against actual native files, not a header-only declaration. */
    int Qualify(const W::WorldPartitionDescriptor &partition, const W::WorldSpatialAssignment &assignments,
                const W::WorldSpanningObjectPlan &placements, const W::WorldDependencyPlan &dependencies,
                const W::WorldDependencyEndpoint endpoint, const W::StreamingCellId cell) {
        auto chunk = Assets::AssetChunkId::Parse("base");
        auto indexType = Assets::AssetTypeId::Parse("world.index");
        auto depsType = Assets::AssetTypeId::Parse("world.dependencies");
        auto cellType = Assets::AssetTypeId::Parse("world.cell");
        auto target = Assets::AssetCookTargetId::Parse("headless-null");
        auto owner = W::WorldPackageAssignmentId::Create(1);
        auto revision = W::WorldPackageAssignmentRevision::Create(1);
        auto epoch = W::PartitionEpoch::Create(1);
        if (chunk.HasError() || indexType.HasError() || depsType.HasError() || cellType.HasError() || target.HasError() ||
            owner.HasError() || revision.HasError() || epoch.HasError())
            return 4;
        const std::array definitions{
            Assets::AssetChunkDefinition{chunk.Value(), Assets::AssetChunkKind::Base, {Asset(2), Asset(3), Asset(4)}}};
        auto packages = Assets::AssetChunkPlan::Create(definitions);
        if (packages.HasError())
            return 5;
        const std::array entities{Runtime::RuntimeEntityDefinition{.object = {7}}};
        const Runtime::SceneCellPayloadIdentity identity{partition.Partition(), cell, {5}, {1}};
        const std::array inputs{Runtime::SceneCellCookInput{{identity, entities}, identity}};
        const std::array<C::ObjectBinding, 1> objects{C::SpatialObjectBinding{endpoint, cell, {7}}};
        const auto settings = ComputeSha256(std::as_bytes(std::span{"public-world-cook", 17}));
        const C::SourceCapture source{partition, assignments, placements, dependencies,   inputs, {}, objects, {},
                                      settings,  1,           0,          "Public world", {},     {}};
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        Output output{std::filesystem::temp_directory_path() / ("horo world cook consumer " + std::to_string(nonce))};
        output.created = std::filesystem::create_directory(output.root);
        if (!output.created)
            return 6;
        NativeDurableFileSystem files;
        auto lock = files.TryAcquireExclusive(output.root / ".cook-writer.lock", "public consumer");
        if (lock.HasError())
            return 7;
        const C::ReleaseCapture release{packages.Value(),
                                        {owner.Value(), revision.Value(), epoch.Value()},
                                        settings,
                                        {},
                                        Asset(3),
                                        Asset(4),
                                        {indexType.Value(), depsType.Value(), cellType.Value()},
                                        output.root,
                                        target.Value()};
        const C::PublicationLimits limits{{65536, 65536, {8, 8, 65536}},
                                          {1, 65536, 65536},
                                          {65536, 64, 1, {1, 1, 4}, {1, 1, 1, 65536, 65536}},
                                          {65536, 65536, 1, 1, 1, 1, 2, 1, 1, 8},
                                          {1, 1, 3, 3},
                                          {1, 1, 1, 4, 1024, 65536, 65536},
                                          {65536, 131072, 3, 1},
                                          1048576,
                                          1};
        Runtime::IncrementalSceneCellCook cache{{1, 1, 1, 1, 65536, 65536}};
        Assets::AssetCookPublicationPolicy policy{.files = &files, .beforeCommit = [] {
            return Result<void>::Success();
        }, .operationId = "10000000-0000-0000-0000-000000000001", .writerLease = &lock.Value()};
        auto published = C::PublishWorldCook(cache, source, release, limits, policy);
        return published.HasValue() && published.Value().validation->CanPublish() && published.Value().publication &&
                       published.Value().scene->cells.size() == 1 && cache.Revision() == 1
                   ? 0
                   : 8;
    }
}  // namespace

int main() {
    const auto world = W::WorldPartitionId::Create(Asset(1).Bytes());
    const auto layer = W::StreamingLayerId::Create(0);
    const auto authored = W::WorldAuthoringRevision::Create(1);
    auto grid = W::WorldCellQuantizationPolicy::Create({}, 100, {-1, -1, 0, 0, 0, 0}, 1);
    if (world.HasError() || layer.HasError() || authored.HasError() || grid.HasError())
        return 1;
    const W::StreamingCellId cell{-1, 0, 0, 0, layer.Value()};
    const W::WorldPartitionBounds bounds{Horo::Math::WorldCoordinate64::FromMillimeters(-100, 0, 0),
                                         Horo::Math::WorldCoordinate64::FromMillimeters(-1, 99, 99)};
    const std::array layers{
        W::WorldLayerDescriptor{layer.Value(), "base", W::WorldLayerOwnership::WorldStreaming, W::WorldLayerFlags::Persistent, 1}};
    const std::array cells{W::WorldPartitionCellDescriptor{cell, {Asset(2)}}};
    auto partition = W::WorldPartitionDescriptor::Create({}, world.Value(), bounds, grid.Value(), layers, cells, {1, 1, 4});
    if (partition.HasError())
        return 2;
    const W::WorldDependencyEndpoint endpoint{{Asset(9), 1}, authored.Value()};
    const std::array candidates{W::WorldSpatialAssignmentCandidate{endpoint.address, endpoint.revision, bounds, layer.Value(), 0}};
    auto assignment = W::WorldSpatialAssignment::Create(partition.Value(), candidates, {1, 1, 1});
    if (assignment.HasError())
        return 3;
    auto placement = W::WorldSpanningObjectPlan::Create(assignment.Value(), {}, {1, 1, 1});
    auto dependencies = W::WorldDependencyPlan::Create(assignment.Value(), {}, {1, 1, 2, 1});
    if (placement.HasError() || dependencies.HasError())
        return 3;
    return Qualify(partition.Value(), assignment.Value(), placement.Value(), dependencies.Value(), endpoint, cell);
}
