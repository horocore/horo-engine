#include "Horo/Navigation/NavigationDefinition.h"
#include "ProjectSceneNavigationComposition.h"
#include "navigation/NavMeshAssetTestFixtures.h"
#include "navigation/NavigationContentPolicyFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace Horo::WorldCook {
    namespace {
        namespace Nav = Navigation;
        namespace AssetsFixture = Nav::AssetTestSupport;
        using AssetsFixture::Id;
        using namespace Application::Internal;

        /** @brief Queries close to the center of one tile without reaching its neighbor. */
        [[nodiscard]] Result<Nav::NavigationProjectionResult> ProjectFloor(const Nav::NavigationWorldReadLease &lease, const float x) {
            const auto &active = lease.Descriptor();
            const Nav::NavigationQueryRequirement requirement{Nav::NavigationQueryKind::NearestPoint, Nav::NavigationQualityLevel::Balanced,
                                                              {64, 1, 2}};
            return lease.Backend().ProjectPoint({active.world, active.topology, {x, 0, 2}, {1, 1, 1}, requirement}, lease.Cancellation());
        }

        /** @brief Uses the project's admitted filter to cross the rebuilt tile boundary. */
        void RequireRebuiltRoute(const Nav::NavigationWorldReadLease &lease, const Nav::NavigationFilterId filter) {
            const auto &active = lease.Descriptor();
            const auto route = lease.Backend().FindPath(
                {.world = active.world,
                 .topology = active.topology,
                 .start = {2, 0, 2},
                 .destination = {12, 0, 2},
                 .filter = filter,
                 .requirement = {Nav::NavigationQueryKind::Path, Nav::NavigationQualityLevel::Balanced, {64, 16, 20}}},
                lease.Cancellation());
            REQUIRE(route.HasValue());
            CHECK(route.Value().status == Nav::NavigationPathStatus::Reachable);
        }

        /** @brief Owns the exact authored identity and policy reused by both base cook and capture. */
        struct ResidentNavigationFixture final {
            const Nav::NavigationAgentProfileId profile = Id<Nav::NavigationAgentProfileId>(7);
            const Nav::SurfaceId surface = Id<Nav::SurfaceId>(9);
            const Nav::NavigationAreaId area = Id<Nav::NavigationAreaId>(11);
            const Nav::NavigationFilterId filter = Id<Nav::NavigationFilterId>(13);
            const Nav::NavigationBakeScopeId scope = Nav::NavigationBakeScopeId::ForScene(AssetsFixture::Asset(true).Bytes()).Value();
            const Sha256Digest digest = AssetsFixture::Digest(7);
            const Nav::NavigationAgentBuildGeometry geometry{.cellSizeMeters = 0.5F};
        };

        /** @brief Builds the configured project selection with a bounded resident provider budget. */
        [[nodiscard]] std::shared_ptr<const ProjectSceneNavigationSelection> MakeSelection(const ResidentNavigationFixture &fixture,
                                                                                           JobSystem &jobs) {
            const auto semanticSource = Id<Nav::NavigationDescriptorSourceId>(1);
            auto definition = Nav::NavigationDefinition::Create({.profiles = {{fixture.profile, "Grounded", fixture.geometry}},
                                                                 .areas = {{fixture.area, {.id = semanticSource}, 1.0F, {.bits = 1}}},
                                                                 .filters = {{fixture.filter, {.id = semanticSource}, {.bits = 1}}}});
            REQUIRE(definition.HasValue());
            Nav::NavigationProjectProfileInput policyInput{.id = Id<Nav::NavigationProjectProfileId>(5),
                                                           .revision = Id<Nav::NavigationProjectProfileRevision>(3),
                                                           .capacities = {8, 1, 4, 2, 65536, 16U * 1024U * 1024U, 8192},
                                                           .maximumQuery = {Nav::NavigationQueryKind::Path,
                                                                            Nav::NavigationQualityLevel::Balanced,
                                                                            {64, 16, 100}}};
            policyInput.capabilities.fill(Nav::NavigationCapabilityRequirement::Optional);
            policyInput.capabilities[static_cast<std::size_t>(Nav::NavigationCapability::GroundedQueries)] =
                Nav::NavigationCapabilityRequirement::Required;
            const auto policy = Nav::NavigationProjectProfile::Create(policyInput).Value();
            return std::make_shared<const ProjectSceneNavigationSelection>(
                ProjectSceneNavigationSelection{{fixture.surface, fixture.scope},
                                                AssetsFixture::Asset(),
                                                fixture.profile,
                                                AssetsFixture::Target(),
                                                policy,
                                                std::make_shared<const Nav::NavigationDefinition>(std::move(definition).Value()),
                                                jobs,
                                                1});
        }

        /** @brief Encodes one grounded tile and one declared empty tile in the authored product catalog. */
        [[nodiscard]] std::vector<std::uint8_t> CookedClosure(const ResidentNavigationFixture &fixture,
                                                              const Nav::NavigationProjectProfile &policy) {
            const Nav::NavigationPreparedTile initial{.tile = {.key = {fixture.profile, fixture.surface, {}, fixture.scope},
                                                               .bounds = {{0, -1, 0}, {8, 1, 8}},
                                                               .tileSizeMeters = 8},
                                                      .geometry = fixture.geometry,
                                                      .borderSizeCells = 4,
                                                      .dependencyKey = fixture.digest};
            const auto solid =
                Nav::NavigationCookedTile::Create(initial,
                                                  {.state = Nav::NavigationTileBuildState::Built,
                                                   .bounds = initial.tile.bounds,
                                                   .vertices = {{0, 0, 0}, {8, 0, 0}, {0, 0, 8}},
                                                   .polygons = {{{0, 3}, {0, 3}, fixture.area}},
                                                   .polygonVertexIndices = {0, 1, 2},
                                                   .polygonAdjacencies = {Nav::NavMeshBoundaryAdjacency, Nav::NavMeshBoundaryAdjacency,
                                                                          Nav::NavMeshBoundaryAdjacency},
                                                   .provenance = {{.producer = Id<Nav::NavigationSourceProducerId>(1),
                                                                   .contribution = Id<Nav::NavigationSourceContributionId>(1),
                                                                   .revision = Id<Nav::NavigationSourceRevision>(1),
                                                                   .sourceDigest = fixture.digest,
                                                                   .polygons = {0, 1}}}});
            REQUIRE(solid.HasValue());
            auto emptyInput = initial;
            emptyInput.tile.key.tile.x = 1;
            emptyInput.tile.bounds = {{8, -1, 0}, {16, 1, 8}};
            const auto empty =
                Nav::NavigationCookedTile::Create(emptyInput, {.key = emptyInput.tile.key.tile, .bounds = emptyInput.tile.bounds});
            REQUIRE(empty.HasValue());
            const Nav::NavigationCookedTileSet closure{fixture.digest,
                                                       {solid.Value(), empty.Value()},
                                                       Nav::NavigationCookedContentProvenance{{fixture.digest, fixture.digest,
                                                                                               fixture.digest},
                                                                                              policy}};
            return Nav::TestSupport::ContentEnvelope(closure, AssetsFixture::Asset(), AssetsFixture::Target());
        }

        /** @brief Captures a post-change floor spanning the existing grid, including previously empty coverage. */
        [[nodiscard]] ProjectSceneNavigationRebuildCommand Command(const ResidentNavigationFixture &fixture,
                                                                   const ProjectSceneNavigationSelection &selection,
                                                                   const Nav::NavigationWorldActivationDescriptor &previous) {
            const Nav::NavigationBakeInputRevisions revisions{Id<Nav::NavigationBakeRequestGeneration>(2),
                                                              Id<Nav::NavigationDefinitionRevision>(1),
                                                              Id<Nav::NavigationSceneDocumentRevision>(2),
                                                              Id<Nav::NavigationAreaRegistryRevision>(1),
                                                              selection.policy.Revision(),
                                                              Id<Nav::NavigationCoordinatePolicyRevision>(1),
                                                              Id<Nav::NavigationSourceSnapshotRevision>(2)};
            const std::array vertices{Math::Vec3{0, 0, 0}, Math::Vec3{0, 0, 8}, Math::Vec3{16, 0, 8}, Math::Vec3{16, 0, 0}};
            const std::array triangles{Nav::NavigationSourceTriangleInput{{0, 2, 3}, fixture.area},
                                       Nav::NavigationSourceTriangleInput{{0, 1, 2}, fixture.area}};
            const std::array sources{Nav::NavigationSourceContributionInput{.producer = Id<Nav::NavigationSourceProducerId>(1),
                                                                            .contribution = Id<Nav::NavigationSourceContributionId>(1),
                                                                            .revision = Id<Nav::NavigationSourceRevision>(2),
                                                                            .contentDigest = fixture.digest,
                                                                            .vertices = vertices,
                                                                            .triangles = triangles}};
            const std::array bindings{Nav::NavigationBakeSurfaceInput{.surface = fixture.surface,
                                                                      .profile = fixture.profile,
                                                                      .filter = fixture.filter,
                                                                      .producer = sources.front().producer,
                                                                      .contribution = sources.front().contribution,
                                                                      .scope = fixture.scope}};
            auto capture = Nav::NavigationSourceGeometrySnapshot::Create(revisions.geometry, sources).Value();
            const std::array profiles{Nav::NavigationAgentProfileDescriptor{fixture.profile, "Grounded", fixture.geometry}};
            auto input = Nav::NavigationBakeInputSnapshot::Create(revisions, selection.catalog->Registry(), profiles, bindings, {},
                                                                  std::move(capture));
            REQUIRE(input.HasValue());
            return {.expected = {previous, revisions, 1, 1, 2},
                    .nextTopology = Id<Nav::NavigationGeneration>(previous.topology.Value() + 1),
                    .input = std::make_shared<const Nav::NavigationBakeInputSnapshot>(std::move(input).Value()),
                    .compatibility = {fixture.digest, fixture.digest, fixture.digest},
                    .maximumRebuiltTiles = 2,
                    .maximumOwnedBytes = 12U * 1024U * 1024U};
        }
    }  // namespace

    TEST_CASE("Project resident Navigation gates rebuilt Detour coverage at the owner cutoff", "[world_cook][navigation][native]") {
        const bool revokeBeforePublication = GENERATE(false, true);
        CAPTURE(revokeBeforePublication);
        unsigned observations{};
        const ResidentNavigationFixture fixture;
        JobSystem jobs{JobSystemConfig{1, 4}};
        const auto selection = MakeSelection(fixture, jobs);
        auto navigation = CreateProjectSceneNavigationWorld(*selection).Value();
        Assets::MemoryAssetProvider provider;
        provider.Insert(AssetsFixture::Asset(), CookedClosure(fixture, selection->policy));
        Assets::AssetLoadService loads{jobs, provider};
        Assets::AssetRegistry registry;
        REQUIRE(registry.Publish({AssetsFixture::Record(AssetsFixture::Asset())}).status == Assets::AssetRegistryBuildStatus::Complete);
        Runtime::RuntimeSceneService scene{registry, loads, {}, {101}};
        Runtime::SceneDefinitionBuilder startup{{99}, {1}};
        startup.Add({.object = {42},
                     .components = {.navigationSurface = Runtime::NavigationSurfaceComponent{.id = fixture.surface,
                                                                                             .definition = AssetsFixture::Asset(),
                                                                                             .generation = 1,
                                                                                             .profiles = {fixture.profile}}}});
        REQUIRE(startup.RequireAsset({AssetsFixture::Asset(), AssetsFixture::Type()}).HasValue());
        REQUIRE(RegisterProjectSceneNavigation(scene, *navigation, selection, false, true).HasValue());
        REQUIRE(scene.Startup({}).HasValue());
        REQUIRE_FALSE(AssetsFixture::ActivateService(scene, {}, std::move(startup).Build().Value()));

        const auto world = navigation->assets->ActiveWorld();
        REQUIRE(world);
        auto previous = navigation->assets->Acquire().Value();
        REQUIRE(ProjectFloor(previous, 2).HasValue());
        Nav::TestSupport::RequireError(ProjectFloor(previous, 12), Nav::NavigationErrors::NoNavigationData);
        auto command = Command(fixture, *selection, previous.Descriptor());
        const auto next = command.nextTopology;
        command.observe = [&observations, revokeBeforePublication, expected = command.expected] {
            auto actual = expected;
            if (++observations == 2 && revokeBeforePublication)
                ++actual.worldChanges;
            return Result<Nav::NavigationRuntimeRebuildFence>::Success(actual);
        };
        const auto queued = QueueProjectSceneNavigationRebuild(*navigation, std::move(command));
        REQUIRE(queued.HasValue());
        jobs.Shutdown(ShutdownPolicy::Drain);
        CHECK(world->ActiveDescriptor().Value() == previous.Descriptor());
        CHECK(observations == 0);
        Nav::TestSupport::RequireError(ProjectFloor(previous, 12), Nav::NavigationErrors::NoNavigationData);
        const auto committed = CommitProjectSceneNavigationRebuild(*navigation, {});
        CHECK(observations == 2);
        CHECK_FALSE(world->HasStagedCandidate());
        CHECK_FALSE(navigation->rebuild->Busy());
        if (revokeBeforePublication) {
            Nav::TestSupport::RequireError(committed, Nav::NavigationErrors::StaleSnapshot);
            CHECK(world->ActiveDescriptor().Value() == previous.Descriptor());
            CHECK_FALSE(previous.IsRevoked());
            CHECK(ProjectFloor(previous, 2).HasValue());
            Nav::TestSupport::RequireError(ProjectFloor(previous, 12), Nav::NavigationErrors::NoNavigationData);
            CHECK(world->RetiredCount() == 0);
            return;
        }
        REQUIRE(committed.HasValue());
        CHECK(committed.Value());
        const auto current = navigation->assets->Acquire().Value();
        CHECK(current.Descriptor().topology == next);
        CHECK(previous.IsRevoked());
        Nav::TestSupport::RequireError(ProjectFloor(previous, 2), Nav::NavigationErrors::QueryCancelled);
        CHECK(ProjectFloor(current, 12).HasValue());
        RequireRebuiltRoute(current, fixture.filter);
        CHECK_FALSE(navigation->assets->ActiveRuntimeRebuildTiles()->back()->Topology().IsEmpty());
        CHECK(world->RetiredCount() == 1);
        previous = {};
        static_cast<void>(world->CollectRetired());
        CHECK(world->RetiredCount() == 0);
    }
}  // namespace Horo::WorldCook
