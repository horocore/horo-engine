#include "Horo/Navigation/NavigationLinkValidation.h"
#include "navigation/NavigationLinkProjectionTestBackend.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Navigation {
    static_assert(!std::is_copy_constructible_v<NavigationLinkValidationSnapshot>);
    static_assert(!std::is_move_assignable_v<NavigationLinkValidationSnapshot>);
    static_assert(std::is_nothrow_move_constructible_v<NavigationLinkValidationSnapshot>);

    namespace {
        using TestSupport::Id;
        using TestSupport::LinkFixture;
        using TestSupport::RequireError;

        using TestSupport::ProjectionBackend;
        using TestSupport::RequireRule;
    }  // namespace

    TEST_CASE("Generation is deterministic under anchor evidence and authored input reordering", "[unit][navigation][links][determinism]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        auto anchors = fixture.Anchors();
        NavigationLinkGenerationPolicy policy{.direction = NavigationLinkDirection::Bidirectional, .anchors = anchors};
        const auto first = std::move(fixture.Validate(backend, {}, policy)).Value();
        std::ranges::reverse(anchors);
        std::ranges::reverse(fixture.clearance);
        const auto second = std::move(fixture.Validate(backend, {}, policy)).Value();
        REQUIRE(first.Authored().empty());
        REQUIRE(first.Suggestions().size() == 1);
        REQUIRE(second.Suggestions().size() == 1);
        REQUIRE(first.Suggestions().front().startAnchor == Id<NavigationLinkAnchorId>(1));
        const auto one = fixture.Cook(first, NavigationGeneratedLinkCookPolicy::IncludeValidatedSuggestions);
        const auto two = fixture.Cook(second, NavigationGeneratedLinkCookPolicy::IncludeValidatedSuggestions);
        REQUIRE(one.HasValue());
        REQUIRE(two.HasValue());
        REQUIRE(one.Value().links == two.Value().links);
        REQUIRE(one.Value().fingerprint == two.Value().fingerprint);
    }

    TEST_CASE("Suggestions require explicit cook policy and rejected suggestions never poison authored acceptance",
              "[unit][navigation][links][cook]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        auto anchors = fixture.Anchors();
        const NavigationLinkGenerationPolicy policy{.direction = NavigationLinkDirection::Bidirectional, .anchors = anchors};
        const auto generated = std::move(fixture.Validate(backend, {}, policy)).Value();
        const auto authoredOnly = fixture.Cook(generated, NavigationGeneratedLinkCookPolicy::AuthoredOnly);
        const auto declared = fixture.Cook(generated, NavigationGeneratedLinkCookPolicy::IncludeValidatedSuggestions);
        REQUIRE(authoredOnly.HasValue());
        REQUIRE(declared.HasValue());
        REQUIRE(authoredOnly.Value().links.empty());
        REQUIRE(declared.Value().links.size() == 1);
        REQUIRE(declared.Value().links.front().bidirectional);
        REQUIRE(authoredOnly.Value().fingerprint != declared.Value().fingerprint);
        RequireError(fixture.Cook(generated, static_cast<NavigationGeneratedLinkCookPolicy>(255)), NavigationErrors::BakeInputInvalid);
        const auto withAuthored = std::move(fixture.Validate(backend, std::array{fixture.link}, policy)).Value();
        REQUIRE(withAuthored.Authored().size() == 1);
        REQUIRE(withAuthored.Suggestions().empty());
        REQUIRE(withAuthored.Diagnostics().front().rule == NavigationLinkValidationRule::DuplicateTraversal);
        REQUIRE(fixture.Cook(withAuthored, NavigationGeneratedLinkCookPolicy::AuthoredOnly).HasValue());
    }

    TEST_CASE("Generation validates anchors and reports pair suggestion work and storage exhaustion without partial results",
              "[unit][navigation][links][budget]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        auto anchors = fixture.Anchors();
        NavigationLinkGenerationPolicy policy{.anchors = anchors};
        NavigationLinkValidationLimits limits;
        const ErrorCodeDescriptor *error = &NavigationErrors::BakeInputCapacityExceeded;
        SECTION("Pair attempts") {
            limits.maximumPairAttempts = 1;
        }
        SECTION("Suggestion output") {
            limits.maximumSuggestions = 1;
        }
        SECTION("Work units") {
            limits.maximumWorkUnits = 1;
        }
        SECTION("Owned storage") {
            limits.maximumOwnedBytes = 1;
        }
        SECTION("Anchor count") {
            limits.maximumAnchors = 1;
        }
        SECTION("Duplicate anchor identities") {
            anchors.back().id = anchors.front().id;
            error = &NavigationErrors::DescriptorConflict;
        }
        SECTION("Nonfinite anchor") {
            anchors.back().endpoint.position.y = std::numeric_limits<float>::infinity();
            error = &NavigationErrors::BakeInputInvalid;
        }
        SECTION("Unknown generation direction") {
            policy.direction = static_cast<NavigationLinkDirection>(255);
            error = &NavigationErrors::BakeInputInvalid;
        }
        SECTION("Zero limit") {
            limits.maximumAuthoredLinks = 0;
            error = &NavigationErrors::BakeInputInvalid;
        }
        RequireError(fixture.Validate(backend, {}, policy, {}, limits), *error);
    }

    TEST_CASE("Cancellation and capability replacement discard validation before any cook result", "[unit][navigation][links][lifecycle]") {
        LinkFixture fixture;
        ProjectionBackend backend;
        CancellationSource cancellation;
        const ErrorCodeDescriptor *error = &NavigationErrors::BakeInputCancelled;
        SECTION("Cancelled before validation") {
            cancellation.RequestCancellation();
        }
        SECTION("Cancelled during first endpoint projection") {
            backend.cancelOnProjection = &cancellation;
        }
        SECTION("Capability replaced during validation") {
            backend.changeCapabilities = true;
            error = &NavigationErrors::CapabilityStale;
        }
        RequireError(fixture.Validate(backend, std::array{fixture.link}, {}, cancellation.Token()), *error);
        REQUIRE(backend.calls <= 2);
    }

    TEST_CASE("Cook adoption rechecks source request topology and all terminal lifecycle fences",
              "[unit][navigation][links][lifecycle][cook]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        const auto snapshot = std::move(fixture.Validate(backend, std::array{fixture.link})).Value();
        auto current = fixture.input.Revisions();
        auto context = fixture.context;
        auto sources = fixture.Observations();
        auto state = NavigationBakePublicationState::Ready;
        CancellationSource cancellation;
        SECTION("Latest request replacement") {
            current.requestGeneration = Id<NavigationBakeRequestGeneration>(2);
        }
        SECTION("Scene revision changed") {
            current.scene = Id<NavigationSceneDocumentRevision>(2);
        }
        SECTION("Source digest changed") {
            sources.front().contentDigest = TestSupport::Digest(99);
        }
        SECTION("Topology replaced") {
            context.topology = Id<NavigationGeneration>(2);
        }
        SECTION("World replaced") {
            context.world = Id<NavigationWorldId>(2);
        }
        SECTION("Profile replaced") {
            context.profile = Id<NavigationAgentProfileId>(2);
        }
        SECTION("Cancelled operation") {
            state = NavigationBakePublicationState::Cancelled;
        }
        SECTION("Superseded operation") {
            state = NavigationBakePublicationState::Superseded;
        }
        SECTION("Failed operation") {
            state = NavigationBakePublicationState::Failed;
        }
        SECTION("Shutting down") {
            state = NavigationBakePublicationState::ShuttingDown;
        }
        SECTION("Parent cancellation") {
            cancellation.RequestCancellation();
        }
        const auto cooked = snapshot.PrepareCookedLinks(NavigationGeneratedLinkCookPolicy::AuthoredOnly, fixture.input, current, sources,
                                                        context, state, cancellation.Token());
        REQUIRE(cooked.HasError());
    }

    TEST_CASE("Validation owns endpoint and descriptor evidence after source storage and provider teardown",
              "[unit][navigation][links][lifetime]") {
        LinkFixture fixture;
        const auto snapshot = [&] {
            const ProjectionBackend backend;
            auto authored = std::array{fixture.link};
            return std::move(fixture.Validate(backend, authored)).Value();
        }();
        fixture.link.start.position = {999.0F, 999.0F, 999.0F};
        fixture.descriptors.clear();
        fixture.clearance.clear();
        const auto cooked = fixture.Cook(snapshot, NavigationGeneratedLinkCookPolicy::AuthoredOnly);
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().links.front().start == Math::Vec3{0.2F, 0.0F, 0.2F});
    }

    TEST_CASE("Moving a validated result preserves owned rows after source destruction", "[unit][navigation][links][lifetime]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        const auto destination = [&] {
            auto source = std::move(fixture.Validate(backend, std::array{fixture.link})).Value();
            NavigationLinkValidationSnapshot moved{std::move(source)};
            REQUIRE(moved.Authored().size() == 1);
            return moved;
        }();
        const auto cooked = fixture.Cook(destination, NavigationGeneratedLinkCookPolicy::AuthoredOnly);
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().links.size() == 1);
        REQUIRE(cooked.Value().links.front().start == fixture.link.start.position);
    }

    TEST_CASE("Generated distance policy applies to final projected endpoints", "[unit][navigation][links][generation]") {
        LinkFixture fixture;
        ProjectionBackend backend;
        backend.shiftHitsOutward = true;
        const auto anchors = fixture.Anchors();
        const NavigationLinkGenerationPolicy policy{.direction = NavigationLinkDirection::Bidirectional,
                                                    .maximumDistanceMeters = 2.1F,
                                                    .anchors = anchors};
        const auto result = fixture.Validate(backend, {}, policy);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().Suggestions().empty());
        REQUIRE(result.Value().Diagnostics().size() == 1);
        REQUIRE(result.Value().Diagnostics().front().rule == NavigationLinkValidationRule::Direction);
    }

}  // namespace Horo::Navigation
