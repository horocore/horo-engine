#pragma once

#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "navigation/IncrementalBakeFixture.h"

#include <algorithm>
#include <array>

namespace Horo::Navigation::TestSupport {
    /** @brief Portable authored model and exact collision input shared by codec, capture and cooked-asset qualification. */
    struct NavigationDataQualificationCorpus final {
        IncrementalBakeFixture collision;

        [[nodiscard]] NavigationDefinition Definition(const bool reverse = false) const {
            auto second = collision.profile;
            second.id = Id<NavigationAgentProfileId>(2);
            second.displayName = "Large 地面";
            second.buildGeometry.radiusMeters = 1;
            const auto areas = IncrementalBakeFixture::Areas();
            const auto filters = IncrementalBakeFixture::Filters();
            NavigationDefinitionInput input{.profiles = {collision.profile, second},
                                            .areas = {areas.begin(), areas.end()},
                                            .filters = {filters.begin(), filters.end()},
                                            .tiles = {.tileSizeCells = 16, .maximumTiles = 8}};
            if (reverse)
                std::ranges::reverse(input.profiles);
            auto created = NavigationDefinition::Create(std::move(input));
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        [[nodiscard]] NavigationAuthoredRecord Record(const bool reverse = false) const {
            const auto definition = Definition(reverse);
            auto encoded = EncodeNavigationDefinitionRecord(definition, Id<NavigationAuthoredRecordId>(1));
            REQUIRE(encoded.HasValue());
            return std::move(encoded).Value();
        }

        /** @brief Capture decoded authored policy and owned collision triangles through the production geometry and bake seams. */
        [[nodiscard]] NavigationBakeInputSnapshot Capture(const NavigationDefinition &definition, const bool reverse = false) const {
            const std::array sources{NavigationSourceContributionInput{.producer = Id<NavigationSourceProducerId>(1),
                                                                       .contribution = Id<NavigationSourceContributionId>(1),
                                                                       .revision = Id<NavigationSourceRevision>(1),
                                                                       .contentDigest = Digest(10),
                                                                       .vertices = collision.vertices,
                                                                       .triangles = collision.triangles}};
            auto geometry = NavigationSourceGeometrySnapshot::Create(collision.revisions.geometry, sources);
            REQUIRE(geometry.HasValue());
            std::vector<NavigationBakeSurfaceInput> surfaces;
            for (const auto &profile : definition.Profiles())
                surfaces.push_back({.surface = Id<SurfaceId>(1),
                                    .profile = profile.id,
                                    .filter = Id<NavigationFilterId>(1),
                                    .producer = sources.front().producer,
                                    .contribution = sources.front().contribution});
            if (reverse)
                std::ranges::reverse(surfaces);
            auto captured = NavigationBakeInputSnapshot::Create(collision.revisions, definition.Registry(), definition.Profiles(), surfaces,
                                                                {}, std::move(geometry).Value());
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }
    };
}  // namespace Horo::Navigation::TestSupport
