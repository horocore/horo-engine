#include "PreFracturedIntersection.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>

namespace Horo::Destruction::Detail {
    TEST_CASE("Pre-fractured surface check finds crossing and coplanar nonadjacent triangles", "[destruction][import]") {
        Assets::PreFracturedSourceNode node;
        node.positions = {{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0.5F, 0.5F, -1}, {0.5F, 0.5F, 1}, {1, 1, 0}}};
        node.triangleIndices = {0, 1, 2, 3, 4, 5};
        std::uint64_t remaining = 1;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Intersecting);
        CHECK(remaining == 0);

        node.positions[3] = {0.5F, 0.5F, 0};
        node.positions[4] = {2, 0.5F, 0};
        node.positions[5] = {0.5F, 2, 0};
        remaining = 1;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Intersecting);
    }

    TEST_CASE("Pre-fractured intersection checks obey work and cancellation budgets", "[destruction][import]") {
        Assets::PreFracturedSourceNode node;
        node.positions = {{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0.5F, 0.5F, -1}, {0.5F, 0.5F, 1}, {1, 1, 0}}};
        node.triangleIndices = {0, 1, 2, 3, 4, 5};
        std::uint64_t remaining = 0;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::TooMuchWork);
        node.triangleIndices[3] = 0;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::TooMuchWork);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        remaining = 1;
        CHECK(CheckSelfIntersection(node, remaining, cancelled.Token()) == IntersectionCheck::Cancelled);
    }

    TEST_CASE("Pre-fractured sweep retains axis bounds and candidate work accounting", "[destruction][import]") {
        Assets::PreFracturedSourceNode node;
        node.positions = {{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0, 3, 0}, {2, 3, 0}, {0, 5, 0}}};
        node.triangleIndices = {0, 1, 2, 3, 4, 5};
        std::uint64_t remaining = 1;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Clear);
        CHECK(remaining == 0);

        for (std::size_t vertex = 3; vertex < 6; ++vertex)
            node.positions[vertex][0] += 3;
        remaining = 1;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Clear);
        CHECK(remaining == 1);
    }

    TEST_CASE("Pre-fractured intersection distinguishes shared-vertex crossing from legal adjacency", "[destruction][import]") {
        Assets::PreFracturedSourceNode node;
        node.positions = {{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}, {0.5F, 0.5F, -1}, {0.5F, 0.5F, 1}}};
        node.triangleIndices = {0, 1, 2, 0, 3, 4};
        std::uint64_t remaining = 4;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Intersecting);

        node.positions[3] = {1, -1, 0};
        node.triangleIndices = {0, 1, 2, 0, 1, 3};
        remaining = 4;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Clear);

        node.positions[3] = {1, 1, 0};
        remaining = 4;
        CHECK(CheckSelfIntersection(node, remaining, CancellationToken{}) == IntersectionCheck::Intersecting);
    }
}  // namespace Horo::Destruction::Detail
