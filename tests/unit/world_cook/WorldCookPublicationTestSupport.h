#pragma once

/** @file WorldCookPublicationTestSupport.h
 * @brief Shared typed committed world publication for integration tests.
 */
#include "../runtime/world_streaming/WorldArtifactCodecTestSupport.h"
#include "Horo/WorldCook/WorldCookPublication.h"
#include "assets/AssetCookOutputFixture.h"

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::WorldCook::PublicationTestSupport {
    namespace W = WorldStreaming;
    namespace S = Runtime;
    namespace A = Assets;
    namespace T = A::OutputTestSupport;
    using W::TestSupport::Asset;
    using W::TestSupport::IdentityFrom;

    /** @brief Independent finite host ceilings for the complete actual two-cell publication. */
    inline PublicationLimits Limits() {
        return {{65536, 65536, {8, 8, 65536}},
                {1, 65536, 65536},
                {65536, 128, 4, {2, 8, 32}, {8, 8, 32, 65536, 65536}},
                {65536, 65536, 8, 8, 16, 16, 16, 16, 32, 512},
                {8, 8, 32, 64},
                {8, 8, 16, 16, 4096, 65536, 131072},
                {65536, 131072, 32, 1},
                1048576,
                16};
    }

    /** @brief Exact page/object publication independent from cell-local Scene identity. */
    inline W::WorldDependencyEndpoint Endpoint(const std::uint64_t object) {
        return {{Asset(1), object}, IdentityFrom<W::WorldAuthoringRevision>(1)};
    }

    /** @brief Actual typed source, owning release, native writer lease and observable durable files. */
    struct Fixture final {
        W::ArtifactCodecTestSupport::Fixture world;
        W::WorldSpatialAssignment assignments{Assignments(world.manifest.Descriptor())};
        W::WorldSpanningObjectPlan placements{Placements(assignments)};
        W::WorldDependencyPlan dependencies{Dependencies(assignments)};
        std::array<S::RuntimeEntityDefinition, 2> entities{{{.object = {11}}, {.object = {22}}}};
        std::array<S::SceneCellCookInput, 2> inputs{Inputs(entities)};
        std::vector<S::SceneCellCookDependency> dependencyArtifacts;
        std::vector<ObjectBinding> objects{SpatialObjectBinding{Endpoint(1), W::CandidateTestSupport::Cell(-1), {11}},
                                           SpatialObjectBinding{Endpoint(2), W::CandidateTestSupport::Cell(), {22}}};
        A::AssetChunkPlan packages{Packages()};
        S::IncrementalSceneCellCook cache{{8, 8, 16, 8, 65536, 131072}};
        T::TempDir directory;
        T::PublicationFiles files;
        std::vector<std::vector<std::uint8_t>> baseArtifacts;
        std::vector<CellNavigationBinding> navigationCells;
        std::uint64_t attempt{};

        explicit Fixture(const bool nonspatial = false, const bool hardDependency = true,
                         const std::optional<W::WorldSpatialAssignmentCandidate> additionalObject = std::nullopt)
            : assignments(Assignments(world.manifest.Descriptor(), nonspatial, additionalObject)),
              placements(Placements(assignments, nonspatial)), dependencies(Dependencies(assignments, hardDependency)),
              packages(Packages(nonspatial)) {
            files.root = directory.path;
            if (nonspatial) {
                baseArtifacts.push_back(T::MakePayload(Asset(8), 16));
                objects.push_back(BaseObjectBinding{Endpoint(3), Asset(8), T::DigestOf(baseArtifacts.front())});
            }
        }

        static W::WorldSpatialAssignment Assignments(
            const W::WorldPartitionDescriptor &partition, const bool nonspatial = false,
            const std::optional<W::WorldSpatialAssignmentCandidate> additionalObject = std::nullopt) {
            const auto bounds = [](const std::int64_t x) {
                const auto point = Math::WorldCoordinate64::FromMillimeters(x, 0, 0);
                return W::WorldPartitionBounds{point, point};
            };
            std::vector candidates{W::WorldSpatialAssignmentCandidate{Endpoint(2).address, Endpoint(2).revision, bounds(0),
                                                                      W::TestSupport::Layer(), 0},
                                   W::WorldSpatialAssignmentCandidate{Endpoint(1).address, Endpoint(1).revision, bounds(-1000),
                                                                      W::TestSupport::Layer(), 0}};
            if (nonspatial)
                candidates.push_back({Endpoint(3).address, Endpoint(3).revision, partition.Bounds(), W::TestSupport::Layer(), 0});
            if (additionalObject)
                candidates.push_back(*additionalObject);
            auto result = W::WorldSpatialAssignment::Create(partition, candidates, {8, 8, 16});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        static W::WorldSpanningObjectPlan Placements(const W::WorldSpatialAssignment &assignments, bool nonspatial = false) {
            std::vector<W::WorldSpanningObjectDirective> directives;
            if (nonspatial)
                directives.push_back({Endpoint(3).address, Endpoint(3).revision, W::WorldSpanningObjectPolicy::NonSpatial, {}});
            auto result = W::WorldSpanningObjectPlan::Create(assignments, directives, {8, 1, 16});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        static W::WorldDependencyPlan Dependencies(const W::WorldSpatialAssignment &assignments, const bool hardDependency = true) {
            const std::array edges{W::WorldDependencyCandidate{Endpoint(1), Endpoint(2), W::WorldDependencyKind::Hard},
                                   W::WorldDependencyCandidate{Endpoint(2), Endpoint(9), W::WorldDependencyKind::Soft}};
            auto selected = std::span<const W::WorldDependencyCandidate>{edges};
            if (!hardDependency)
                selected = selected.last(1);
            auto result = W::WorldDependencyPlan::Create(assignments, selected, {8, 8, 8, 8});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        static std::array<S::SceneCellCookInput, 2> Inputs(const std::array<S::RuntimeEntityDefinition, 2> &entities) {
            const S::SceneCellPayloadIdentity left{W::TestSupport::World(), W::CandidateTestSupport::Cell(-1), {71}, {1}};
            const S::SceneCellPayloadIdentity right{W::TestSupport::World(), W::CandidateTestSupport::Cell(), {72}, {1}};
            return {S::SceneCellCookInput{{left, std::span{entities}.first(1)}, left},
                    S::SceneCellCookInput{{right, std::span{entities}.last(1)}, right}};
        }

        static A::AssetChunkPlan Packages(const bool nonspatial = false) {
            auto id = A::AssetChunkId::Parse("base");
            REQUIRE(id.HasValue());
            std::array definitions{
                A::AssetChunkDefinition{std::move(id).Value(), A::AssetChunkKind::Base, {Asset(4), Asset(5), Asset(6), Asset(7)}}};
            if (nonspatial)
                definitions.front().assets.push_back(Asset(8));
            auto result = A::AssetChunkPlan::Create(definitions);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        SourceCapture Source() const {
            return {world.manifest.Descriptor(),
                    assignments,
                    placements,
                    dependencies,
                    inputs,
                    dependencyArtifacts,
                    objects,
                    {},
                    W::CandidateTestSupport::Hash(),
                    1,
                    cache.Revision(),
                    "Canonical world",
                    {},
                    {},
                    navigationCells};
        }

        ReleaseCapture Release() const {
            return {packages,
                    {IdentityFrom<W::WorldPackageAssignmentId>(1), IdentityFrom<W::WorldPackageAssignmentRevision>(1),
                     IdentityFrom<W::PartitionEpoch>(1)},
                    W::CandidateTestSupport::Hash(),
                    baseArtifacts,
                    Asset(6),
                    Asset(7),
                    {T::Type("world.index"), T::Type("world.dependencies"), T::Type("world.cell")},
                    directory.path,
                    T::Target("headless-null")};
        }

        Result<PublicationReport> Publish(const PublicationLimits &limits = Limits(), const std::function<Result<void>()> &freshness = [] {
            return Result<void>::Success();
        }, const CancellationToken &cancellation = {}) {
            auto lease = files.native.TryAcquireExclusive(directory.path / ".cook-writer.lock", "world publication fixture");
            REQUIRE(lease.HasValue());
            auto policy = T::Policy(files, "10000000-0000-0000-0000-000000000001");
            policy.newOperationId = [&] {
                return Result<A::AssetId>::Success(Asset(++attempt + 100));
            };
            policy.operationId.clear();
            policy.writerLease = &lease.Value();
            policy.beforeCommit = freshness;
            return PublishWorldCook(cache, Source(), Release(), limits, policy, cancellation);
        }
    };

}  // namespace Horo::WorldCook::PublicationTestSupport
