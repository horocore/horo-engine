#include "navigation/IncrementalBakeFixture.h"

namespace Horo::Navigation {
    namespace {
        using namespace TestSupport;
    }

    TEST_CASE("Empty definition input retains valid profiles and exact complete publication fences",
              "[unit][navigation][bake_input][empty]") {
        IncrementalBakeFixture fixture;
        const auto original = fixture.Input();
        const auto oldSources = fixture.Observations();
        fixture.RemoveSources();
        const auto empty = fixture.Input();
        CHECK(empty->Partitions().empty());
        CHECK(empty->Triangles().empty());
        REQUIRE(empty->Profiles().size() == 1);
        CHECK(empty->Fingerprint() != original->Fingerprint());
        CHECK(empty->ValidatePublication(fixture.revisions.requestGeneration, fixture.revisions, {}).HasValue());
        CHECK(empty->ValidatePublication(fixture.revisions.requestGeneration, fixture.revisions, oldSources).HasError());
    }

    TEST_CASE("An empty surface closure cannot discard nonempty geometry or modifiers", "[unit][navigation][bake_input][empty]") {
        IncrementalBakeFixture fixture;
        SECTION("retained contribution") {
            fixture.surfaces.clear();
        }
        SECTION("retained modifier") {
            fixture.ExcludeBorder();
            const auto retained = fixture.modifiers;
            fixture.RemoveSources();
            fixture.modifiers = retained;
        }
        RequireError(fixture.TryInput(), NavigationErrors::BakeInputInvalid);
    }

    TEST_CASE("Empty coverage retains clipped source ownership even without a tile extent", "[unit][navigation][bake_input][empty]") {
        IncrementalBakeFixture fixture;
        fixture.RemoveSources();
        fixture.hasContribution = true;
        const auto input = fixture.Input();
        CHECK(input->Partitions().empty());
        CHECK(input->Triangles().empty());
        CHECK(input->ValidatePublication(fixture.revisions.requestGeneration, fixture.revisions, fixture.Observations()).HasValue());
        CHECK(input->ValidatePublication(fixture.revisions.requestGeneration, fixture.revisions, {}).HasError());
    }

    TEST_CASE("Clipped empty source partitions still resolve filters and produce explicit empty tiles",
              "[unit][navigation][bake_input][empty]") {
        IncrementalBakeFixture fixture;
        fixture.vertices.clear();
        fixture.triangles.clear();
        const auto input = fixture.Input();
        REQUIRE(input->Partitions().size() == 1);
        CHECK(input->Partitions().front().triangleCount == 0);
        const auto prepared = PrepareNavigationBakeTile(*input, fixture.Tiles().front(), fixture.compatibility);
        REQUIRE(prepared.HasValue());
        CHECK(prepared.Value().triangles.empty());
        auto registry = NavigationAreaRegistry::Create(IncrementalBakeFixture::Areas(), IncrementalBakeFixture::Filters()).Value();
        const NavigationSourceContributionInput contribution{.producer = Id<NavigationSourceProducerId>(1),
                                                             .contribution = Id<NavigationSourceContributionId>(1),
                                                             .revision = Id<NavigationSourceRevision>(1),
                                                             .contentDigest = Digest(10)};
        auto geometry = NavigationSourceGeometrySnapshot::Create(fixture.revisions.geometry, std::span{&contribution, 1}).Value();
        const NavigationBakeSurfaceInput surface{fixture.surfaces.front(), fixture.profile.id, Id<NavigationFilterId>(999),
                                                 contribution.producer, contribution.contribution};
        RequireError(NavigationBakeInputSnapshot::Create(fixture.revisions, registry, std::span{&fixture.profile, 1},
                                                         std::span{&surface, 1}, {}, std::move(geometry)),
                     NavigationErrors::BakeInputReferenceMissing);
    }
}  // namespace Horo::Navigation
