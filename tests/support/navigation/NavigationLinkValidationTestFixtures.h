#pragma once

#include "Horo/Navigation/NavigationLinkValidation.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <utility>

namespace Horo::Navigation::TestSupport {
    inline NavigationBakeInputRevisions LinkRevisions() {
        return {.requestGeneration = Id<NavigationBakeRequestGeneration>(1),
                .definition = Id<NavigationDefinitionRevision>(1),
                .scene = Id<NavigationSceneDocumentRevision>(1),
                .areaRegistry = Id<NavigationAreaRegistryRevision>(1),
                .projectProfile = Id<NavigationProjectProfileRevision>(1),
                .coordinates = Id<NavigationCoordinatePolicyRevision>(1),
                .geometry = Id<NavigationSourceSnapshotRevision>(1)};
    }

    inline NavigationAreaRegistry LinkRegistry() {
        const std::array areas{NavigationAreaDescriptor{.id = Id<NavigationAreaId>(1),
                                                        .source = {.id = Id<NavigationDescriptorSourceId>(1)},
                                                        .flags = {.bits = 1}}};
        const std::array filters{
            NavigationQueryFilterDescriptor{.id = Id<NavigationFilterId>(1), .source = {.id = Id<NavigationDescriptorSourceId>(1)}}};
        return std::move(NavigationAreaRegistry::Create(areas, filters)).Value();
    }

    inline NavigationBakeInputSnapshot LinkBakeInput(const NavigationAreaRegistry &areas) {
        OwnedTriangleContribution low;
        auto high = low;
        high.producer = Id<NavigationSourceProducerId>(2);
        high.contribution = Id<NavigationSourceContributionId>(2);
        for (auto &vertex : high.vertices)
            vertex.x += 2.0F;
        const std::array sources{NavigationSourceContributionInput{.producer = low.producer,
                                                                   .contribution = low.contribution,
                                                                   .revision = low.revision,
                                                                   .contentDigest = low.digest,
                                                                   .vertices = low.vertices,
                                                                   .triangles = low.triangles},
                                 NavigationSourceContributionInput{.producer = high.producer,
                                                                   .contribution = high.contribution,
                                                                   .revision = high.revision,
                                                                   .contentDigest = high.digest,
                                                                   .vertices = high.vertices,
                                                                   .triangles = high.triangles}};
        auto geometry = std::move(NavigationSourceGeometrySnapshot::Create(LinkRevisions().geometry, sources)).Value();
        const std::array profiles{NavigationAgentProfileDescriptor{.id = Id<NavigationAgentProfileId>(1), .displayName = "Human"}};
        const std::array surfaces{NavigationBakeSurfaceInput{.surface = Id<SurfaceId>(10),
                                                             .profile = profiles.front().id,
                                                             .filter = Id<NavigationFilterId>(1),
                                                             .producer = low.producer,
                                                             .contribution = low.contribution},
                                  NavigationBakeSurfaceInput{.surface = Id<SurfaceId>(20),
                                                             .profile = profiles.front().id,
                                                             .filter = Id<NavigationFilterId>(1),
                                                             .producer = high.producer,
                                                             .contribution = high.contribution}};
        return std::move(NavigationBakeInputSnapshot::Create(LinkRevisions(), areas, profiles, surfaces, {}, std::move(geometry))).Value();
    }

    struct LinkFixture final {
        NavigationAreaRegistry areas{LinkRegistry()};
        NavigationBakeInputSnapshot input{LinkBakeInput(areas)};
        NavigationLinkProjectionContext context{.profile = Id<NavigationAgentProfileId>(1),
                                                .world = Id<NavigationWorldId>(1),
                                                .topology = Id<NavigationGeneration>(1),
                                                .requirement = {.query = NavigationQueryKind::NearestPoint,
                                                                .limits = {.maximumNodeExpansions = 32,
                                                                           .maximumResultPoints = 1,
                                                                           .maximumSearchDistanceMeters = 10.0F}}};
        NavigationBakeLinkInput link{.id = Id<NavigationLinkId>(1),
                                     .profile = context.profile,
                                     .start = {.surface = Id<SurfaceId>(10), .position = {0.2F, 0.0F, 0.2F}},
                                     .end = {.surface = Id<SurfaceId>(20), .position = {2.2F, 0.0F, 0.2F}}};
        std::vector<NavigationTraversalDescriptor> descriptors{
            {.profile = context.profile, .area = Id<NavigationAreaId>(1), .supportsBidirectional = true}};
        std::vector<NavigationLinkClearanceEvidence> clearance;

        LinkFixture() {
            Observe(link);
        }

        void Observe(const NavigationBakeLinkInput &candidate, const float radius = 0.5F, const float height = 1.8F) {
            clearance = {{.bakeFingerprint = input.Fingerprint(),
                          .profile = candidate.profile,
                          .start = candidate.start,
                          .end = candidate.end,
                          .radiusMeters = radius,
                          .heightMeters = height},
                         {.bakeFingerprint = input.Fingerprint(),
                          .profile = candidate.profile,
                          .start = candidate.end,
                          .end = candidate.start,
                          .radiusMeters = radius,
                          .heightMeters = height}};
        }

        [[nodiscard]] std::array<NavigationLinkGenerationAnchor, 2> Anchors() const {
            return {NavigationLinkGenerationAnchor{.id = Id<NavigationLinkAnchorId>(1), .endpoint = link.start},
                    NavigationLinkGenerationAnchor{.id = Id<NavigationLinkAnchorId>(2), .endpoint = link.end}};
        }

        [[nodiscard]] std::array<NavigationSourceObservation, 2> Observations() const {
            return {NavigationSourceObservation{.producer = Id<NavigationSourceProducerId>(1),
                                                .contribution = Id<NavigationSourceContributionId>(1),
                                                .revision = Id<NavigationSourceRevision>(1),
                                                .contentDigest = Digest(1)},
                    NavigationSourceObservation{.producer = Id<NavigationSourceProducerId>(2),
                                                .contribution = Id<NavigationSourceContributionId>(2),
                                                .revision = Id<NavigationSourceRevision>(1),
                                                .contentDigest = Digest(1)}};
        }

        [[nodiscard]] Result<NavigationLinkValidationSnapshot> Validate(
            const INavigationQueryBackend &backend, const std::span<const NavigationBakeLinkInput> authored,
            const std::optional<NavigationLinkGenerationPolicy> &generation = {}, const CancellationToken &token = {},
            const NavigationLinkValidationLimits &limits = {}) const {
            return NavigationLinkValidationSnapshot::Validate({.input = input,
                                                               .areas = areas,
                                                               .backend = backend,
                                                               .context = context,
                                                               .authored = authored,
                                                               .descriptors = descriptors,
                                                               .clearance = clearance,
                                                               .generation = generation,
                                                               .limits = limits},
                                                              token);
        }

        [[nodiscard]] Result<NavigationCookedLinkSet> Cook(const NavigationLinkValidationSnapshot &snapshot,
                                                           const NavigationGeneratedLinkCookPolicy policy,
                                                           const CancellationToken &token = {}) const {
            return snapshot.PrepareCookedLinks(policy, input, input.Revisions(), Observations(), context,
                                               NavigationBakePublicationState::Ready, token);
        }
    };
}  // namespace Horo::Navigation::TestSupport
