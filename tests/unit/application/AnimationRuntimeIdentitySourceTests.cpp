#include "AllocationProbe.h"
#include "AnimationRuntimeIdentitySource.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>

namespace {
    using namespace Horo::Application::Internal;
    static_assert(!std::is_copy_constructible_v<AnimationRuntimeIdentitySource>);
    static_assert(!std::is_move_constructible_v<AnimationRuntimeIdentitySource>);
}  // namespace

TEST_CASE("Animation runtime owner identities stay unique across copied host factory pins", "[animation][host][identity]") {
    auto process = std::make_shared<AnimationRuntimeIdentitySource>(AnimationRuntimeIdentityRange{100, 103});
    auto play = process;
    auto preview = process;
    auto headless = process;
    const auto failedPreparation = play->Issue();
    const auto nextProject = preview->Issue();
    const auto otherWorld = headless->Issue();
    REQUIRE(failedPreparation.HasValue());
    REQUIRE(nextProject.HasValue());
    REQUIRE(otherWorld.HasValue());
    CHECK(failedPreparation.identity.Value() == 100);
    CHECK(nextProject.identity.Value() == 101);
    CHECK(otherWorld.identity.Value() == 102);
    play.reset();
    preview = process;
    const auto reopened = preview->Issue();
    REQUIRE(reopened.HasValue());
    CHECK(reopened.identity.Value() == 103);
    const auto exhausted = process->Issue();
    CHECK_FALSE(exhausted.HasValue());
    CHECK(exhausted.failure == AnimationRuntimeIdentityFailure::Exhausted);
    CHECK_FALSE(exhausted.identity.IsValid());
}

TEST_CASE("Animation runtime identity ranges reject zero and reversed admission", "[animation][host][identity]") {
    for (const auto range : {AnimationRuntimeIdentityRange{}, AnimationRuntimeIdentityRange{0, 9}, AnimationRuntimeIdentityRange{9, 8}}) {
        AnimationRuntimeIdentitySource source{range};
        const auto issue = source.Issue();
        CHECK_FALSE(issue.HasValue());
        CHECK(issue.failure == AnimationRuntimeIdentityFailure::InvalidRange);
        CHECK_FALSE(issue.identity.IsValid());
    }
}

TEST_CASE("Animation runtime identity exhaustion cannot wrap or recycle the maximum value", "[animation][host][identity]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    AnimationRuntimeIdentitySource source{{maximum - 1, maximum}};
    const auto first = source.Issue();
    const auto last = source.Issue();
    REQUIRE(first.HasValue());
    REQUIRE(last.HasValue());
    CHECK(first.identity.Value() == maximum - 1);
    CHECK(last.identity.Value() == maximum);
    for (unsigned retry = 0; retry < 3; ++retry) {
        const auto denied = source.Issue();
        CHECK_FALSE(denied.HasValue());
        CHECK(denied.failure == AnimationRuntimeIdentityFailure::Exhausted);
        CHECK_FALSE(denied.identity.IsValid());
    }
}

TEST_CASE("Foreign animation identity requests cannot consume the host owner sequence", "[animation][host][identity]") {
    AnimationRuntimeIdentitySource source{{71, 72}};
    AnimationRuntimeIdentityIssue rejected;
    std::jthread other([&] {
        rejected = source.Issue();
    });
    other.join();
    CHECK_FALSE(rejected.HasValue());
    CHECK(rejected.failure == AnimationRuntimeIdentityFailure::WrongThread);
    const auto owner = source.Issue();
    REQUIRE(owner.HasValue());
    CHECK(owner.identity.Value() == 71);
}

TEST_CASE("Animation runtime identity issuance owns no allocation or resource reclamation", "[animation][host][identity]") {
    AnimationRuntimeIdentitySource source{{1, 2}};
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto first = source.Issue();
    const auto second = source.Issue();
    const auto exhausted = source.Issue();
    const auto afterAllocations = Horo::Tests::AllocationProbe::Count();
    const auto afterFrees = Horo::Tests::AllocationProbe::FreeCount();
    CHECK(first.HasValue());
    CHECK(second.HasValue());
    CHECK(exhausted.failure == AnimationRuntimeIdentityFailure::Exhausted);
    CHECK(afterAllocations == allocations);
    CHECK(afterFrees == frees);
}
