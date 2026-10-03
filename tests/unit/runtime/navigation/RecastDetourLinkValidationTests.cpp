#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "navigation/NavigationLinkValidationTestFixtures.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using TestSupport::Digest;
        using TestSupport::Id;
        using TestSupport::LinkFixture;

        struct TwoSurfaceTopology final {
            std::array<Math::Vec3, 6> vertices{
                {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {2.0F, 0.0F, 0.0F}, {3.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 1.0F}}};
            std::array<GroundedNavigationPolygon, 2> polygons{
                {{.vertexIndices = {0, 1, 2}, .vertexCount = 3, .area = Id<NavigationAreaId>(1), .surface = Id<SurfaceId>(10)},
                 {.vertexIndices = {3, 4, 5}, .vertexCount = 3, .area = Id<NavigationAreaId>(1), .surface = Id<SurfaceId>(20)}}};
        };

        Result<NavMeshData> CookArtifact(const NavigationCookedLinkSet &links, const TwoSurfaceTopology &topology) {
            const NavMeshArtifactHeader header{.coordinateFrame = {.tileSizeMeters = 32.0F},
                                               .profile = {.id = Id<NavigationAgentProfileId>(1), .contentDigest = links.fingerprint},
                                               .tileCount = 1,
                                               .vertexCount = 6,
                                               .polygonCount = 2,
                                               .polygonVertexIndexCount = 6,
                                               .offMeshLinkCount = static_cast<std::uint32_t>(links.links.size()),
                                               .provenanceCount = 1,
                                               .portableEncodedBytes = 512,
                                               .portableDecodedBytes = 512,
                                               .payloadDigest = Digest(10)};
            const std::array tiles{NavMeshTileDescriptor{.bounds = {.minimum = {0.0F, -1.0F, 0.0F}, .maximum = {32.0F, 2.0F, 32.0F}},
                                                         .vertices = {0, 6},
                                                         .polygons = {0, 2},
                                                         .polygonVertexIndices = {0, 6},
                                                         .offMeshLinks = {0, header.offMeshLinkCount},
                                                         .provenance = {0, 1},
                                                         .payloadDigest = Digest(20)}};
            const std::array tileDigests{Digest(20)};
            const std::array polygons{NavMeshPolygon{.vertexIndices = {0, 3}, .area = Id<NavigationAreaId>(1)},
                                      NavMeshPolygon{.vertexIndices = {3, 3}, .area = Id<NavigationAreaId>(1)}};
            const std::array<std::uint32_t, 6> indices{0, 1, 2, 3, 4, 5};
            const std::array provenance{NavMeshSourceProvenance{.producer = Id<NavigationSourceProducerId>(1),
                                                                .contribution = Id<NavigationSourceContributionId>(1),
                                                                .revision = Id<NavigationSourceRevision>(1),
                                                                .sourceDigest = Digest(1),
                                                                .polygons = {0, 2}}};
            return NavMeshData::Create({.header = header,
                                        .observedPayloadDigest = header.payloadDigest,
                                        .tiles = tiles,
                                        .observedTilePayloadDigests = tileDigests,
                                        .tables = {.vertices = topology.vertices,
                                                   .polygons = polygons,
                                                   .polygonVertexIndices = indices,
                                                   .offMeshLinks = links.links,
                                                   .provenance = provenance}});
        }
    }  // namespace

    TEST_CASE("Real Detour endpoint projection and explicit policy feed the existing cooked NavMesh contract",
              "[unit][navigation][recast_detour][links][cook]") {
        LinkFixture fixture;
        const TwoSurfaceTopology topology;
        const auto backend = CreateRecastDetourNavigationQueryBackend({.world = fixture.context.world,
                                                                       .topology = fixture.context.topology,
                                                                       .vertices = topology.vertices,
                                                                       .polygons = topology.polygons,
                                                                       .maximumQueryNodes = 64,
                                                                       .areas = fixture.areas.Areas(),
                                                                       .filters = fixture.areas.Filters()});
        REQUIRE(backend.HasValue());
        const auto anchors = fixture.Anchors();
        const NavigationLinkGenerationPolicy policy{.direction = NavigationLinkDirection::Bidirectional, .anchors = anchors};
        const auto validated = fixture.Validate(*backend.Value(), {}, policy);
        REQUIRE(validated.HasValue());
        REQUIRE(validated.Value().Suggestions().size() == 1);
        const auto authoredOnly = fixture.Cook(validated.Value(), NavigationGeneratedLinkCookPolicy::AuthoredOnly);
        const auto declared = fixture.Cook(validated.Value(), NavigationGeneratedLinkCookPolicy::IncludeValidatedSuggestions);
        REQUIRE(authoredOnly.HasValue());
        REQUIRE(declared.HasValue());
        auto authoredArtifact = CookArtifact(authoredOnly.Value(), topology);
        auto declaredArtifact = CookArtifact(declared.Value(), topology);
        REQUIRE(authoredArtifact.HasValue());
        REQUIRE(declaredArtifact.HasValue());
        REQUIRE(authoredArtifact.Value().Header().offMeshLinkCount == 0);
        REQUIRE(declaredArtifact.Value().Header().offMeshLinkCount == 1);
        const auto tile = declaredArtifact.Value().ResolveTile({});
        REQUIRE(tile.HasValue());
        REQUIRE(tile.Value().tables.offMeshLinks.front().bidirectional);
        REQUIRE(tile.Value().tables.offMeshLinks.front().startPolygon == 0);
        REQUIRE(tile.Value().tables.offMeshLinks.front().endPolygon == 1);
    }

    TEST_CASE("Neutral polygon boundaries automatically generate stable anchors with count work and reference validation",
              "[unit][navigation][recast_detour][links][generation]") {
        const TwoSurfaceTopology topology;
        const auto mesh = std::move(CookArtifact({.fingerprint = Digest(3)}, topology)).Value();
        std::array bindings{NavigationLinkGenerationSurfaceBinding{.polygons = {0, 1}, .surface = Id<SurfaceId>(10)},
                            NavigationLinkGenerationSurfaceBinding{.polygons = {1, 1}, .surface = Id<SurfaceId>(20)}};
        const auto forward = CaptureNavigationLinkBoundaryAnchors(mesh, bindings, 6, 1'000, {});
        REQUIRE(forward.HasValue());
        REQUIRE(forward.Value().size() == 6);
        REQUIRE(forward.Value().front().id == Id<NavigationLinkAnchorId>(1));
        REQUIRE(forward.Value()[3].id == Id<NavigationLinkAnchorId>(257));
        REQUIRE(forward.Value().front().endpoint.position == Math::Vec3{0.5F, 0.0F, 0.0F});
        std::swap(bindings[0], bindings[1]);
        const auto reverse = CaptureNavigationLinkBoundaryAnchors(mesh, bindings, 6, 1'000, {});
        REQUIRE(reverse.HasValue());
        for (std::size_t index = 0; index < forward.Value().size(); ++index) {
            REQUIRE(forward.Value()[index].id == reverse.Value()[index].id);
            REQUIRE(forward.Value()[index].endpoint == reverse.Value()[index].endpoint);
        }
        TestSupport::RequireError(CaptureNavigationLinkBoundaryAnchors(mesh, bindings, 5, 1'000, {}),
                                  NavigationErrors::BakeInputCapacityExceeded);
        TestSupport::RequireError(CaptureNavigationLinkBoundaryAnchors(mesh, bindings, 6, 1, {}),
                                  NavigationErrors::BakeInputCapacityExceeded);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        TestSupport::RequireError(CaptureNavigationLinkBoundaryAnchors(mesh, bindings, 6, 1'000, cancellation.Token()),
                                  NavigationErrors::BakeInputCancelled);
        bindings[1] = bindings[0];
        TestSupport::RequireError(CaptureNavigationLinkBoundaryAnchors(mesh, bindings, 6, 1'000, {}), NavigationErrors::DescriptorConflict);
        bindings = {NavigationLinkGenerationSurfaceBinding{.polygons = {0, 3}, .surface = Id<SurfaceId>(10)},
                    NavigationLinkGenerationSurfaceBinding{.tile = {.x = 99}, .polygons = {0, 1}, .surface = Id<SurfaceId>(20)}};
        TestSupport::RequireError(CaptureNavigationLinkBoundaryAnchors(mesh, std::span{bindings}.first(1), 6, 1'000, {}),
                                  NavigationErrors::BakeInputInvalid);
        TestSupport::RequireError(CaptureNavigationLinkBoundaryAnchors(mesh, std::span{bindings}.last(1), 6, 1'000, {}),
                                  NavigationErrors::NavMeshTileUnknown);
    }
}  // namespace Horo::Navigation
