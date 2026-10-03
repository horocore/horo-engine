#pragma once

#include "Horo/Navigation/NavigationTileDependencies.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <memory>

namespace Horo::Navigation::TestSupport {
    /** @brief Four neighboring grounded tiles with editable canonical source values and modifier bounds. */
    struct IncrementalBakeFixture {
        NavigationBakeInputRevisions revisions{.requestGeneration = Id<NavigationBakeRequestGeneration>(1),
                                               .definition = Id<NavigationDefinitionRevision>(1),
                                               .scene = Id<NavigationSceneDocumentRevision>(1),
                                               .areaRegistry = Id<NavigationAreaRegistryRevision>(1),
                                               .projectProfile = Id<NavigationProjectProfileRevision>(1),
                                               .coordinates = Id<NavigationCoordinatePolicyRevision>(1),
                                               .geometry = Id<NavigationSourceSnapshotRevision>(1)};
        NavigationAgentProfileDescriptor profile{.id = Id<NavigationAgentProfileId>(1),
                                                 .displayName = "Human",
                                                 .buildGeometry = {.radiusMeters = 0.5F,
                                                                   .heightMeters = 1.8F,
                                                                   .maxSlopeDegrees = 45,
                                                                   .stepHeightMeters = 0.4F,
                                                                   .cellSizeMeters = 0.5F,
                                                                   .cellHeightMeters = 0.2F}};
        NavigationTileBakeCompatibility compatibility{Digest(1), Digest(2), Digest(3)};
        std::vector<Math::Vec3> vertices;
        std::vector<NavigationSourceTriangleInput> triangles;
        std::vector<NavigationBakeModifierInput> modifiers;

        IncrementalBakeFixture() {
            for (int tile = 0; tile < 4; ++tile) {
                const float x = static_cast<float>(tile) * 8.0F;
                const auto first = static_cast<std::uint32_t>(vertices.size());
                vertices.insert(vertices.end(), {{x, 0, 0}, {x, 0, 8}, {x + 8, 0, 8}, {x + 8, 0, 0}});
                triangles.push_back({.vertexIndices = {first, first + 2, first + 3}, .area = Id<NavigationAreaId>(1)});
                triangles.push_back({.vertexIndices = {first, first + 1, first + 2}, .area = Id<NavigationAreaId>(1)});
            }
        }

        [[nodiscard]] static auto Areas() {
            return std::array{NavigationAreaDescriptor{.id = Id<NavigationAreaId>(1),
                                                       .source = {.id = Id<NavigationDescriptorSourceId>(1)},
                                                       .flags = {.bits = 1}}};
        }

        [[nodiscard]] static auto Filters() {
            return std::array{NavigationQueryFilterDescriptor{.id = Id<NavigationFilterId>(1),
                                                              .source = {.id = Id<NavigationDescriptorSourceId>(1)},
                                                              .includedFlags = {.bits = 1}}};
        }

        [[nodiscard]] std::shared_ptr<const NavigationBakeInputSnapshot> Input() const {
            const auto areas = Areas();
            const auto filters = Filters();
            auto registry = NavigationAreaRegistry::Create(areas, filters).Value();
            const std::array contributions{NavigationSourceContributionInput{.producer = Id<NavigationSourceProducerId>(1),
                                                                             .contribution = Id<NavigationSourceContributionId>(1),
                                                                             .revision = Id<NavigationSourceRevision>(1),
                                                                             .contentDigest = Digest(10),
                                                                             .vertices = vertices,
                                                                             .triangles = triangles}};
            auto geometry = NavigationSourceGeometrySnapshot::Create(revisions.geometry, contributions).Value();
            const std::array surfaces{NavigationBakeSurfaceInput{.surface = Id<SurfaceId>(1),
                                                                 .profile = profile.id,
                                                                 .filter = Id<NavigationFilterId>(1),
                                                                 .producer = contributions[0].producer,
                                                                 .contribution = contributions[0].contribution}};
            auto captured =
                NavigationBakeInputSnapshot::Create(revisions, registry, std::span{&profile, 1}, surfaces, modifiers, std::move(geometry));
            REQUIRE(captured.HasValue());
            return std::make_shared<const NavigationBakeInputSnapshot>(std::move(captured).Value());
        }

        [[nodiscard]] std::vector<NavigationBakeTile> Tiles() const {
            std::vector<NavigationBakeTile> tiles;
            for (int x = 0; x < 4; ++x)
                tiles.push_back({.key = {.profile = profile.id, .surface = Id<SurfaceId>(1), .tile = {.x = x}},
                                 .bounds = {{static_cast<float>(x) * 8.0F, -1, 0}, {static_cast<float>(x + 1) * 8.0F, 3, 8}},
                                 .tileSizeMeters = 8});
            return tiles;
        }

        void ExcludeBorder(const float x = 8) {
            modifiers = {{.id = Id<NavigationModifierId>(1),
                          .surface = Id<SurfaceId>(1),
                          .profile = profile.id,
                          .mode = NavigationBakeModifierMode::Exclude,
                          .area = Id<NavigationAreaId>(1),
                          .localBounds = {{x - 0.5F, -1, -1}, {x + 0.5F, 3, 9}}}};
            revisions.scene = Id<NavigationSceneDocumentRevision>(revisions.scene.Value() + 1);
            revisions.requestGeneration = Id<NavigationBakeRequestGeneration>(revisions.requestGeneration.Value() + 1);
        }

        [[nodiscard]] std::vector<NavigationSourceObservation> Observations() const {
            return {{.producer = Id<NavigationSourceProducerId>(1),
                     .contribution = Id<NavigationSourceContributionId>(1),
                     .revision = Id<NavigationSourceRevision>(1),
                     .contentDigest = Digest(10)}};
        }
    };
}  // namespace Horo::Navigation::TestSupport
