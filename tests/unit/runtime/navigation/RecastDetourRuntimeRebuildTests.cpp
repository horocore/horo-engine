#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationRuntimeTileRebuild.h"
#include "Horo/Navigation/NavigationWorldLifecycle.h"
#include "navigation/NavigationDataQualificationCorpus.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Id;
        using TestSupport::RequireError;

        /** @brief Builds the initial complete portable closure through the native tile builder. */
        [[nodiscard]] std::vector<std::shared_ptr<const NavigationCookedTile>> CookBaseTiles(
            const INavigationMeshBuilder &builder, const NavigationBakeInputSnapshot &input,
            const std::span<const NavigationBakeTile> tiles, const NavigationTileBakeCompatibility &compatibility) {
            std::vector<std::shared_ptr<const NavigationCookedTile>> cooked;
            cooked.reserve(tiles.size());
            for (const auto &tile : tiles) {
                const auto prepared = PrepareNavigationBakeTile(input, tile, compatibility);
                REQUIRE(prepared.HasValue());
                const auto &selected = prepared.Value();
                auto built = builder.BuildTile({.key = tile.key.tile,
                                                .bounds = tile.bounds,
                                                .tileSizeMeters = tile.tileSizeMeters,
                                                .buildGeometry = selected.geometry,
                                                .triangles = selected.triangles,
                                                .modifiers = selected.modifiers,
                                                .borderSizeCells = selected.borderSizeCells},
                                               {});
                REQUIRE(built.HasValue());
                REQUIRE_FALSE(built.Value().IsEmpty());
                auto output = NavigationCookedTile::Create(selected, std::move(built).Value());
                REQUIRE(output.HasValue());
                cooked.push_back(std::move(output).Value());
            }
            return cooked;
        }

        /** @brief Composes real Detour preparation, optionally under an intentionally insufficient provider budget. */
        class NativeFactory final : public INavigationRuntimeRebuildBackendFactory {
        public:
            explicit NativeFactory(const NavigationDefinition &definition, std::size_t providerBudget = 1024U * 1024U)
                : definition_(definition), providerBudget_(providerBudget) {}

            [[nodiscard]] Result<std::unique_ptr<INavigationQueryBackend>> Prepare(
                const NavigationWorldActivationDescriptor &descriptor, std::span<const std::shared_ptr<const NavigationCookedTile>> tiles,
                std::size_t maximumOwnedBytes, const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<std::unique_ptr<INavigationQueryBackend>>::Failure(MakeError(NavigationErrors::BakeInputCancelled));
                const auto &key = tiles.front()->Key();
                return CreateRecastDetourCookedNavigationQueryBackend({.world = descriptor.world,
                                                                       .topology = descriptor.topology,
                                                                       .surface = {key.surface, key.scope},
                                                                       .profile = key.profile,
                                                                       .definition = definition_,
                                                                       .tiles = tiles,
                                                                       .nearestPointHalfExtents = {2, 4, 2},
                                                                       .queryLimits = {128, 32, 100},
                                                                       .maximumConcurrentQueries = 1,
                                                                       .maximumOwnedBytes = std::min(providerBudget_, maximumOwnedBytes),
                                                                       .capabilityRevision = 12});
            }

        private:
            const NavigationDefinition &definition_;
            std::size_t providerBudget_;
        };

        /** @brief Uses the exact acquired generation and its cooperative retirement token. */
        [[nodiscard]] NavigationPathStatus PathStatus(const NavigationWorldReadLease &lease, const NavigationFilterId filter,
                                                      const float startX, const float destinationX) {
            const auto path =
                lease.Backend().FindPath({.world = lease.Descriptor().world,
                                          .topology = lease.Descriptor().topology,
                                          .start = {startX, 0, 4},
                                          .destination = {destinationX, 0, 4},
                                          .filter = filter,
                                          .requirement = {NavigationQueryKind::Path, NavigationQualityLevel::Balanced, {64, 16, 80}}},
                                         lease.Cancellation());
            REQUIRE(path.HasValue());
            return path.Value().status;
        }

        /** @brief Owns an actual Recast/Detour world and the canonical source for four neighboring tiles. */
        struct RuntimeRebuildFixture final {
            using Tiles = std::vector<std::shared_ptr<const NavigationCookedTile>>;

            TestSupport::NavigationDataQualificationCorpus corpus;
            const NavigationDefinition definition{corpus.Definition()};
            const std::vector<NavigationBakeTile> layout{corpus.collision.Tiles()};
            const std::unique_ptr<INavigationMeshBuilder> builder{CreateBuilder()};
            const NativeFactory native{definition};
            const NavigationWorldActivationDescriptor original{Id<NavigationSceneRuntimeId>(1), Id<NavigationSceneGeneration>(1),
                                                               Id<NavigationWorldId>(2), Id<NavigationGeneration>(3)};
            const Tiles base{CookBaseTiles(*builder, *corpus.collision.Input(), layout, corpus.collision.compatibility)};
            const NavigationFilterId filter{definition.Registry().Filters().front().id};
            NavigationWorldLifecycle world{CreateWorld()};

            /** @brief Advances a canonical source modifier that cuts a corridor at the requested tile border. */
            [[nodiscard]] std::shared_ptr<const NavigationBakeInputSnapshot> ExcludeBorder(const float x = 8) {
                corpus.collision.ExcludeBorder(x);
                return corpus.collision.Input();
            }

            /** @brief Binds detached work to the currently published generation and the owned post-change snapshot. */
            [[nodiscard]] NavigationRuntimeTileRebuildRequest Request(
                const NavigationBakeInputSnapshot &input, const std::span<const std::shared_ptr<const NavigationCookedTile>> activeTiles,
                const std::uint64_t next, const std::uint64_t revision = 1) const {
                return {.expected = {.activation = world.ActiveDescriptor().Value(),
                                     .sources = input.Revisions(),
                                     .overlay = revision,
                                     .origin = 1,
                                     .worldChanges = revision},
                        .nextTopology = Id<NavigationGeneration>(next),
                        .input = input,
                        .layout = layout,
                        .activeTiles = activeTiles,
                        .compatibility = corpus.collision.compatibility};
            }

            /** @brief Pins whichever complete native provider currently serves this world. */
            [[nodiscard]] NavigationWorldReadLease Acquire() const {
                auto lease = world.Acquire(original.world);
                REQUIRE(lease.HasValue());
                return std::move(lease).Value();
            }

            /** @brief Prepares a candidate through the fixture's native builder and provider factory. */
            [[nodiscard]] Result<NavigationRuntimeTileRebuildCandidate> Prepare(const NavigationRuntimeTileRebuildRequest &request) const {
                return PrepareRuntimeNavigationTileRebuild(request, *builder, native);
            }

        private:
            /** @brief Creates the actual optional Recast build provider. */
            [[nodiscard]] static std::unique_ptr<INavigationMeshBuilder> CreateBuilder() {
                auto builder = CreateRecastDetourNavigationMeshBuilder();
                REQUIRE(builder.HasValue());
                return std::move(builder).Value();
            }

            /** @brief Publishes the initial reachable corridor with exactly one retired-world slot. */
            [[nodiscard]] NavigationWorldLifecycle CreateWorld() const {
                REQUIRE(base.size() == 4);
                auto provider = native.Prepare(original, base, 1024U * 1024U, {});
                REQUIRE(provider.HasValue());
                auto created = NavigationWorldLifecycle::Create(1);
                REQUIRE(created.HasValue());
                auto active = std::move(created).Value();
                REQUIRE(active.Stage(original, std::move(provider).Value()).HasValue());
                REQUIRE(active.CommitAtSafePoint(original.scene, original.sceneGeneration).HasValue());
                const auto lease = active.Acquire(original.world).Value();
                REQUIRE(PathStatus(lease, filter, 4, 12) == NavigationPathStatus::Reachable);
                return active;
            }
        };
    }  // namespace

    TEST_CASE("Native local tile rebuild preparation failure preserves the prior generation",
              "[unit][navigation][provider][tile_rebuild]") {
        RuntimeRebuildFixture fixture;
        const auto predecessor = fixture.Acquire();
        const auto edited = fixture.ExcludeBorder();
        const auto request = fixture.Request(*edited, fixture.base, 4);

        const NativeFactory exhausted{fixture.definition, 1};
        RequireError(PrepareRuntimeNavigationTileRebuild(request, *fixture.builder, exhausted), NavigationErrors::CapacityExceeded);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        RequireError(PrepareRuntimeNavigationTileRebuild(request, *fixture.builder, fixture.native, cancelled.Token()),
                     NavigationErrors::BakeInputCancelled);
        CHECK(fixture.world.ActiveDescriptor().Value() == fixture.original);
        CHECK_FALSE(fixture.world.HasStagedCandidate());
        CHECK_FALSE(predecessor.IsRevoked());
        CHECK(PathStatus(predecessor, fixture.filter, 4, 12) == NavigationPathStatus::Reachable);

        auto stale = fixture.Prepare(request);
        REQUIRE(stale.HasValue());
        auto staleFence = request.expected;
        ++staleFence.worldChanges;
        RequireError(std::move(stale).Value().PublishAtSafePoint(fixture.world, staleFence), NavigationErrors::StaleSnapshot);
        CHECK(fixture.world.ActiveDescriptor().Value() == fixture.original);
        CHECK_FALSE(fixture.world.HasStagedCandidate());
    }

    TEST_CASE("Native local tile rebuild swaps the complete generation and retains its predecessor for held readers",
              "[unit][navigation][provider][tile_rebuild]") {
        RuntimeRebuildFixture fixture;
        auto predecessor = fixture.Acquire();
        const auto edited = fixture.ExcludeBorder();
        const auto request = fixture.Request(*edited, fixture.base, 4);

        auto prepared = fixture.Prepare(request);
        REQUIRE(prepared.HasValue());
        const auto &candidate = prepared.Value();
        REQUIRE(candidate.Tiles().size() == fixture.base.size());
        CHECK(candidate.RebuiltTileCount() == 2);
        CHECK(candidate.Tiles()[0] != fixture.base[0]);
        CHECK(candidate.Tiles()[1] != fixture.base[1]);
        CHECK(candidate.Tiles()[2] == fixture.base[2]);
        CHECK(candidate.Tiles()[3] == fixture.base[3]);
        CHECK(fixture.world.ActiveDescriptor().Value() == fixture.original);
        CHECK(PathStatus(predecessor, fixture.filter, 4, 12) == NavigationPathStatus::Reachable);

        const auto published = std::move(prepared).Value().PublishAtSafePoint(fixture.world, request.expected);
        REQUIRE(published.HasValue());
        REQUIRE(published.Value().size() == fixture.base.size());
        CHECK(published.Value()[2] == fixture.base[2]);
        CHECK(published.Value()[3] == fixture.base[3]);
        const auto replacement = fixture.Acquire();
        REQUIRE(replacement.Descriptor().topology == request.nextTopology);
        CHECK(PathStatus(replacement, fixture.filter, 4, 12) == NavigationPathStatus::Unreachable);
        CHECK(PathStatus(replacement, fixture.filter, 20, 28) == NavigationPathStatus::Reachable);
        CHECK(predecessor.Revocation() == NavigationWorldRevocation::TopologyReplaced);
        CHECK(predecessor.Cancellation().IsCancellationRequested());
        CHECK(fixture.world.CollectRetired() == NavigationWorldLifecycleState::Active);
        REQUIRE(fixture.world.RetiredCount() == 1);
        predecessor = {};
        CHECK(fixture.world.CollectRetired() == NavigationWorldLifecycleState::Active);
        CHECK(fixture.world.RetiredCount() == 0);
    }

    TEST_CASE("Native local tile rebuild rolls back a full retired slot and permits a fresh retry",
              "[unit][navigation][provider][tile_rebuild]") {
        RuntimeRebuildFixture fixture;
        auto predecessor = fixture.Acquire();
        const auto edited = fixture.ExcludeBorder();
        const auto request = fixture.Request(*edited, fixture.base, 4);
        auto prepared = fixture.Prepare(request);
        REQUIRE(prepared.HasValue());
        const auto published = std::move(prepared).Value().PublishAtSafePoint(fixture.world, request.expected);
        REQUIRE(published.HasValue());
        REQUIRE(fixture.world.RetiredCount() == 1);

        auto replacement = fixture.Acquire();
        const auto moved = fixture.ExcludeBorder(16);
        auto retry = fixture.Request(*moved, published.Value(), 5, 2);
        auto rejected = fixture.Prepare(retry);
        REQUIRE(rejected.HasValue());
        RequireError(std::move(rejected).Value().PublishAtSafePoint(fixture.world, retry.expected), NavigationErrors::CapacityExceeded);
        CHECK_FALSE(fixture.world.HasStagedCandidate());
        CHECK(fixture.world.ActiveDescriptor().Value() == replacement.Descriptor());
        CHECK(fixture.world.RetiredCount() == 1);
        CHECK_FALSE(replacement.IsRevoked());
        CHECK(PathStatus(replacement, fixture.filter, 4, 12) == NavigationPathStatus::Unreachable);

        predecessor = {};
        REQUIRE(fixture.world.CollectRetired() == NavigationWorldLifecycleState::Active);
        REQUIRE(fixture.world.RetiredCount() == 0);
        retry.nextTopology = Id<NavigationGeneration>(6);
        auto allowed = fixture.Prepare(retry);
        REQUIRE(allowed.HasValue());
        REQUIRE(std::move(allowed).Value().PublishAtSafePoint(fixture.world, retry.expected).HasValue());
        CHECK_FALSE(fixture.world.HasStagedCandidate());
        const auto current = fixture.Acquire();
        CHECK(current.Descriptor().topology == retry.nextTopology);
        CHECK(PathStatus(current, fixture.filter, 4, 12) == NavigationPathStatus::Reachable);
        CHECK(PathStatus(current, fixture.filter, 12, 20) == NavigationPathStatus::Unreachable);
        CHECK(replacement.Revocation() == NavigationWorldRevocation::TopologyReplaced);
        CHECK(fixture.world.RetiredCount() == 1);
        replacement = {};
        CHECK(fixture.world.CollectRetired() == NavigationWorldLifecycleState::Active);
        CHECK(fixture.world.RetiredCount() == 0);
    }

    TEST_CASE("Native local tile rebuild can publish explicit loss of all walkable coverage",
              "[unit][navigation][provider][tile_rebuild]") {
        RuntimeRebuildFixture fixture;
        const auto predecessor = fixture.Acquire();
        fixture.corpus.collision.ExcludeBorder();
        fixture.corpus.collision.modifiers.front().localBounds = {{-4, -1, -4}, {36, 3, 12}};
        const auto edited = fixture.corpus.collision.Input();
        const auto request = fixture.Request(*edited, fixture.base, 4);
        const NativeFactory exhausted{fixture.definition, 1};
        auto prepared = PrepareRuntimeNavigationTileRebuild(request, *fixture.builder, exhausted);
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().RebuiltTileCount() == fixture.base.size());
        CHECK(PathStatus(predecessor, fixture.filter, 4, 12) == NavigationPathStatus::Reachable);
        const auto published = std::move(prepared).Value().PublishAtSafePoint(fixture.world, request.expected);
        REQUIRE(published.HasValue());
        CHECK(published.Value().size() == fixture.base.size());
        CHECK(std::ranges::all_of(published.Value(), [](const auto &tile) {
            return tile->Topology().IsEmpty();
        }));
        CHECK(fixture.world.ActiveDescriptor().Value().topology == request.nextTopology);
        RequireError(fixture.world.Acquire(fixture.original.world), NavigationErrors::NoNavigationData);
        CHECK(predecessor.Revocation() == NavigationWorldRevocation::TopologyReplaced);
    }
}  // namespace Horo::Navigation
