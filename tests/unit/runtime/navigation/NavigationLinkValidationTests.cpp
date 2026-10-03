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

    TEST_CASE("Authored link acceptance preserves ordered endpoints and converts to neutral cook rows", "[unit][navigation][links]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        const std::array authored{fixture.link};
        const auto snapshot = std::move(fixture.Validate(backend, authored)).Value();
        REQUIRE(snapshot.Authored().size() == 1);
        REQUIRE(snapshot.Suggestions().empty());
        REQUIRE(snapshot.Diagnostics().empty());
        REQUIRE(backend.calls == 2);
        const auto cooked = fixture.Cook(snapshot, NavigationGeneratedLinkCookPolicy::AuthoredOnly);
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().links.size() == 1);
        REQUIRE(cooked.Value().links.front().start == fixture.link.start.position);
        REQUIRE(cooked.Value().links.front().end == fixture.link.end.position);
        REQUIRE(cooked.Value().links.front().startPolygon == 0);
        REQUIRE(cooked.Value().links.front().endPolygon == 1);
        REQUIRE_FALSE(cooked.Value().links.front().bidirectional);
    }

    TEST_CASE("Invalid links retain both endpoints and exact failing rules before provider work", "[unit][navigation][links][malformed]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        auto rule = NavigationLinkValidationRule::Malformed;
        SECTION("Reserved identity") {
            fixture.link.id = {};
        }
        SECTION("NaN endpoint") {
            fixture.link.start.position.y = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("Infinite endpoint") {
            fixture.link.end.position.z = std::numeric_limits<float>::infinity();
        }
        SECTION("Zero connection radius") {
            fixture.link.start.connectionRadiusMeters = 0.0F;
        }
        SECTION("Unknown direction") {
            fixture.link.direction = static_cast<NavigationLinkDirection>(255);
        }
        SECTION("Nonfinite cost") {
            fixture.link.traversalCost = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("Unrepresentable area cost") {
            fixture.link.traversalCost = 2.0F;
            rule = NavigationLinkValidationRule::TraversalCost;
        }
        SECTION("Unknown kind") {
            fixture.link.kind = static_cast<NavigationLinkKind>(255);
        }
        SECTION("Missing descriptor") {
            fixture.descriptors.clear();
            rule = NavigationLinkValidationRule::DescriptorUnavailable;
        }
        SECTION("Incompatible profile") {
            fixture.link.profile = Id<NavigationAgentProfileId>(2);
            rule = NavigationLinkValidationRule::ProfileMismatch;
        }
        SECTION("Missing end surface") {
            fixture.link.end.surface = Id<SurfaceId>(999);
            rule = NavigationLinkValidationRule::ProfileMismatch;
        }
        const std::array authored{fixture.link};
        const auto result = fixture.Validate(backend, authored);
        REQUIRE(result.HasValue());
        RequireRule(result.Value(), rule, fixture.link);
        REQUIRE(backend.calls == 0);
        RequireError(fixture.Cook(result.Value(), NavigationGeneratedLinkCookPolicy::AuthoredOnly), NavigationErrors::BakeInputInvalid);
    }

    TEST_CASE("Both projection failures retain original typed errors and ordered endpoints", "[unit][navigation][links][projection]") {
        LinkFixture fixture;
        ProjectionBackend backend;
        backend.failStart = true;
        backend.failEnd = true;
        const auto result = fixture.Validate(backend, std::array{fixture.link});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().Diagnostics().size() == 2);
        REQUIRE(result.Value().Diagnostics()[0].rule == NavigationLinkValidationRule::StartProjection);
        REQUIRE(result.Value().Diagnostics()[1].rule == NavigationLinkValidationRule::EndProjection);
        for (const auto &row : result.Value().Diagnostics()) {
            REQUIRE(row.start == fixture.link.start);
            REQUIRE(row.end == fixture.link.end);
            REQUIRE(row.cause.has_value());
            REQUIRE(row.cause->code.Value() == NavigationErrors::NoNavigationData.code.Value());
        }
    }

    TEST_CASE("Projection cannot accept foreign stale malformed or out-of-radius evidence", "[unit][navigation][links][projection]") {
        LinkFixture fixture;
        ProjectionBackend backend;
        SECTION("Foreign surface") {
            backend.foreignSurface = true;
        }
        SECTION("Replaced topology") {
            backend.staleTopology = true;
        }
        SECTION("Nonfinite hit") {
            backend.malformedHit = true;
        }
        SECTION("Hit outside endpoint radius") {
            backend.distantHit = true;
        }
        const auto result = fixture.Validate(backend, std::array{fixture.link});
        REQUIRE(result.HasValue());
        RequireRule(result.Value(), NavigationLinkValidationRule::StartProjection, fixture.link);
    }

    TEST_CASE("Traversal direction checks asymmetric rise drop and bidirectional descriptor support",
              "[unit][navigation][links][direction]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        fixture.link.end.position.y = 0.5F;
        fixture.Observe(fixture.link);
        fixture.descriptors.front().maximumRiseMeters = 0.5F;
        fixture.descriptors.front().maximumDropMeters = 0.0F;
        SECTION("Exact rise boundary is accepted") {
            const auto result = fixture.Validate(backend, std::array{fixture.link});
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Authored().size() == 1);
        }
        SECTION("Reverse direction violates drop") {
            std::swap(fixture.link.start, fixture.link.end);
            const auto result = fixture.Validate(backend, std::array{fixture.link});
            REQUIRE(result.HasValue());
            RequireRule(result.Value(), NavigationLinkValidationRule::Direction, fixture.link);
        }
        SECTION("Bidirectional must qualify both directions") {
            fixture.link.direction = NavigationLinkDirection::Bidirectional;
            const auto result = fixture.Validate(backend, std::array{fixture.link});
            REQUIRE(result.HasValue());
            RequireRule(result.Value(), NavigationLinkValidationRule::Direction, fixture.link);
        }
        SECTION("Descriptor explicitly lacks bidirectional capability") {
            fixture.link.direction = NavigationLinkDirection::Bidirectional;
            fixture.descriptors.front().supportsBidirectional = false;
            const auto result = fixture.Validate(backend, std::array{fixture.link});
            REQUIRE(result.HasValue());
            RequireRule(result.Value(), NavigationLinkValidationRule::Direction, fixture.link);
            REQUIRE(backend.calls == 0);
        }
        SECTION("Distance over traversal descriptor limit") {
            fixture.descriptors.front().maximumDistanceMeters = 1.0F;
            const auto result = fixture.Validate(backend, std::array{fixture.link});
            REQUIRE(result.HasValue());
            RequireRule(result.Value(), NavigationLinkValidationRule::Direction, fixture.link);
        }
    }

    TEST_CASE("Clearance fails closed for missing insufficient or reverse-only evidence", "[unit][navigation][links][clearance]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        SECTION("Missing observation") {
            fixture.clearance.clear();
        }
        SECTION("Insufficient width") {
            fixture.clearance.front().radiusMeters = 0.49F;
        }
        SECTION("Insufficient height") {
            fixture.clearance.front().heightMeters = 1.79F;
        }
        SECTION("Endpoint acceptance radius too small") {
            fixture.link.start.connectionRadiusMeters = 0.49F;
        }
        SECTION("Forward evidence does not cover reverse") {
            fixture.link.direction = NavigationLinkDirection::Bidirectional;
            fixture.clearance.resize(1);
        }
        SECTION("Different profile evidence") {
            fixture.clearance.front().profile = Id<NavigationAgentProfileId>(2);
        }
        const auto result = fixture.Validate(backend, std::array{fixture.link});
        REQUIRE(result.HasValue());
        RequireRule(result.Value(), NavigationLinkValidationRule::Clearance, fixture.link);
    }

    TEST_CASE("Descriptor and collision capture reject malformed ambiguous or stale input transactionally",
              "[unit][navigation][links][hostile]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        const ErrorCodeDescriptor *error = &NavigationErrors::BakeInputInvalid;
        SECTION("Duplicate descriptor kind") {
            fixture.descriptors.push_back(fixture.descriptors.front());
            error = &NavigationErrors::DescriptorConflict;
        }
        SECTION("Nonfinite distance") {
            fixture.descriptors.front().maximumDistanceMeters = std::numeric_limits<float>::infinity();
        }
        SECTION("Negative rise") {
            fixture.descriptors.front().maximumRiseMeters = -1.0F;
        }
        SECTION("Wrong descriptor profile") {
            fixture.descriptors.front().profile = Id<NavigationAgentProfileId>(2);
        }
        SECTION("Unknown traversal area") {
            fixture.descriptors.front().area = Id<NavigationAreaId>(999);
        }
        SECTION("Nonfinite clearance") {
            fixture.clearance.front().heightMeters = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("Negative measured clearance") {
            fixture.clearance.front().radiusMeters = -1.0F;
        }
        SECTION("Stale collision source") {
            fixture.clearance.front().bakeFingerprint = TestSupport::Digest(90);
            error = &NavigationErrors::BakeInputStale;
        }
        SECTION("Ambiguous clearance") {
            fixture.clearance.push_back(fixture.clearance.front());
            error = &NavigationErrors::DescriptorConflict;
        }
        RequireError(fixture.Validate(backend, std::array{fixture.link}), *error);
        REQUIRE(backend.calls == 0);
    }

    TEST_CASE("Duplicate authored identities reject every ambiguous row before projection", "[unit][navigation][links][duplicates]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        auto duplicate = fixture.link;
        duplicate.end.position.y = 0.1F;
        const auto result = fixture.Validate(backend, std::array{fixture.link, duplicate});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().Authored().empty());
        REQUIRE(result.Value().Diagnostics().size() == 2);
        REQUIRE(result.Value().Diagnostics()[0].rule == NavigationLinkValidationRule::DuplicateIdentity);
        REQUIRE(result.Value().Diagnostics()[1].rule == NavigationLinkValidationRule::DuplicateIdentity);
        REQUIRE(backend.calls == 0);
    }

    TEST_CASE("Duplicate traversals preserve opposite one-way links and reject bidirectional overlap",
              "[unit][navigation][links][duplicates]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        auto other = fixture.link;
        other.id = Id<NavigationLinkId>(2);
        bool duplicate = true;
        SECTION("Same direction with different identity") {
            // The unmodified endpoints exercise the forward duplicate baseline.
        }
        SECTION("Opposite one-way direction remains distinct") {
            std::swap(other.start, other.end);
            duplicate = false;
        }
        SECTION("Reverse overlap with bidirectional link") {
            fixture.link.direction = NavigationLinkDirection::Bidirectional;
            std::swap(other.start, other.end);
        }
        const auto result = fixture.Validate(backend, std::array{other, fixture.link});
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().Authored().size() == (duplicate ? 1 : 2));
        if (duplicate) {
            REQUIRE(result.Value().Diagnostics().size() == 1);
            REQUIRE(result.Value().Diagnostics().front().rule == NavigationLinkValidationRule::DuplicateTraversal);
        } else {
            REQUIRE(result.Value().Diagnostics().empty());
        }
    }

    TEST_CASE("Coincident projected endpoints fail even on the same compatible surface", "[unit][navigation][links][boundary]") {
        LinkFixture fixture;
        const ProjectionBackend backend;
        fixture.link.end = fixture.link.start;
        fixture.clearance.clear();
        const auto result = fixture.Validate(backend, std::array{fixture.link});
        REQUIRE(result.HasValue());
        RequireRule(result.Value(), NavigationLinkValidationRule::CoincidentEndpoints, fixture.link);
    }
}  // namespace Horo::Navigation
