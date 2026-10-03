#include "Horo/PlatformServices/PlatformProgressionIdempotency.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace {
    using namespace Horo::PlatformServices;

    [[nodiscard]] PlatformProgressionOccurrenceId Occurrence(const std::uint8_t value) {
        PlatformProgressionOccurrenceId occurrence;
        occurrence.bytes.back() = static_cast<std::byte>(value);
        return occurrence;
    }

    [[nodiscard]] PlatformProgressionSubjectPartition SubjectPartition(const std::uint8_t value) {
        PlatformProgressionSubjectPartition partition;
        partition.digest.bytes.back() = value;
        return partition;
    }

    [[nodiscard]] PlatformProgressionMutationCandidate Candidate(const std::uint8_t occurrence = 1, const std::uint64_t session = 2) {
        return PlatformProgressionMutationCandidate{.occurrence = Occurrence(occurrence),
                                                    .scope = {.provider = {1},
                                                              .session = {session},
                                                              .accessPolicy = {3},
                                                              .subjectPartition = SubjectPartition(11)},
                                                    .definitionKind = PlatformServiceIdKind::Stat,
                                                    .definition = {42},
                                                    .kind = PlatformProgressionMutationKind::SetStatMaximum,
                                                    .value = std::int64_t{7},
                                                    .expectedRevision = std::nullopt,
                                                    .authority = ProgressionAuthorityMode::LocalProduct,
                                                    .policy = {9}};
    }
}  // namespace

TEST_CASE("Platform progression mutation IDs are deterministic and session-partitioned", "[platform-services][progression][idempotency]") {
    const auto first = BuildPlatformProgressionMutationEnvelope(Candidate(1, 2));
    REQUIRE(first.HasValue());
    const auto repeat = BuildPlatformProgressionMutationEnvelope(Candidate(1, 2));
    REQUIRE(repeat.HasValue());
    CHECK(first.Value() == repeat.Value());
    CHECK(first.Value().mutation.IsValid());

    const auto otherOccurrence = BuildPlatformProgressionMutationEnvelope(Candidate(2, 2));
    REQUIRE(otherOccurrence.HasValue());
    CHECK(otherOccurrence.Value().mutation != first.Value().mutation);

    const auto otherSession = BuildPlatformProgressionMutationEnvelope(Candidate(1, 4));
    REQUIRE(otherSession.HasValue());
    CHECK(otherSession.Value().mutation != first.Value().mutation);
    CHECK(otherSession.Value().scope != first.Value().scope);

    auto otherSubjectCandidate = Candidate(1, 2);
    otherSubjectCandidate.scope.subjectPartition = SubjectPartition(12);
    const auto otherSubject = BuildPlatformProgressionMutationEnvelope(otherSubjectCandidate);
    REQUIRE(otherSubject.HasValue());
    CHECK(otherSubject.Value().mutation != first.Value().mutation);
    CHECK(otherSubject.Value().scope != first.Value().scope);
}

TEST_CASE("Platform progression mutation validation rejects incomplete or mismatched semantics",
          "[platform-services][progression][idempotency][validation]") {
    auto invalidOccurrence = Candidate();
    invalidOccurrence.occurrence = {};
    const auto missingOccurrence = BuildPlatformProgressionMutationEnvelope(invalidOccurrence);
    REQUIRE(missingOccurrence.HasError());
    CHECK(missingOccurrence.ErrorValue().code.Value() == "platform.progression.invalid_mutation");

    auto wrongDefinition = Candidate();
    wrongDefinition.definitionKind = PlatformServiceIdKind::Leaderboard;
    const auto mismatchedDefinition = BuildPlatformProgressionMutationEnvelope(wrongDefinition);
    REQUIRE(mismatchedDefinition.HasError());

    auto unexpectedRevision = Candidate();
    unexpectedRevision.expectedRevision = 4;
    const auto mismatchedRevision = BuildPlatformProgressionMutationEnvelope(unexpectedRevision);
    REQUIRE(mismatchedRevision.HasError());

    auto conditional = Candidate();
    conditional.kind = PlatformProgressionMutationKind::SetStatSnapshot;
    conditional.expectedRevision = 4;
    const auto validConditional = BuildPlatformProgressionMutationEnvelope(conditional);
    REQUIRE(validConditional.HasValue());
    CHECK(validConditional.Value().IsValid());

    auto negativeAchievementProgress = Candidate();
    negativeAchievementProgress.definitionKind = PlatformServiceIdKind::Achievement;
    negativeAchievementProgress.kind = PlatformProgressionMutationKind::SetProgressMaximum;
    negativeAchievementProgress.value = std::uint64_t{10};
    CHECK(BuildPlatformProgressionMutationEnvelope(negativeAchievementProgress).HasValue());
    negativeAchievementProgress.value = std::int64_t{-1};
    const auto invalidProgress = BuildPlatformProgressionMutationEnvelope(negativeAchievementProgress);
    REQUIRE(invalidProgress.HasError());
    CHECK(invalidProgress.ErrorValue().code.Value() == "platform.progression.invalid_mutation");

    auto missingSubjectPartition = Candidate();
    missingSubjectPartition.scope.subjectPartition = {};
    CHECK(BuildPlatformProgressionMutationEnvelope(missingSubjectPartition).HasError());
}

TEST_CASE("Exact in-flight duplicates join while conflicting mutation reuse is rejected", "[platform-services][progression][idempotency]") {
    const auto envelope = BuildPlatformProgressionMutationEnvelope(Candidate());
    REQUIRE(envelope.HasValue());

    PlatformProgressionIdempotencyStore store({.maximumInFlight = 2});
    const auto started = store.Admit(envelope.Value());
    REQUIRE(started.HasValue());
    CHECK(started.Value().disposition == PlatformProgressionAdmissionDisposition::Started);

    const auto joined = store.Admit(envelope.Value());
    REQUIRE(joined.HasValue());
    CHECK(joined.Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting);
    CHECK(store.InFlightCount() == 1);

    auto conflicting = envelope.Value();
    conflicting.value = std::int64_t{8};
    const auto conflict = store.Admit(conflicting);
    REQUIRE(conflict.HasError());
    CHECK(conflict.ErrorValue().code.Value() == "platform.progression.idempotency_conflict");
    CHECK(store.Find(envelope.Value().mutation, envelope.Value().scope).value() == envelope.Value());
}

TEST_CASE("In-flight capacity is bounded and terminal retirement is idempotent",
          "[platform-services][progression][idempotency][capacity]") {
    PlatformProgressionIdempotencyStore store({.maximumInFlight = 1});
    const auto first = BuildPlatformProgressionMutationEnvelope(Candidate(1));
    const auto second = BuildPlatformProgressionMutationEnvelope(Candidate(2));
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    REQUIRE(store.Admit(first.Value()).HasValue());

    const auto full = store.Admit(second.Value());
    REQUIRE(full.HasError());
    CHECK(full.ErrorValue().code.Value() == "platform.progression.capacity_exceeded");

    REQUIRE(store.Retire(first.Value()).HasValue());
    CHECK(store.InFlightCount() == 0);
    CHECK(store.Retire(first.Value()).Value() == PlatformProgressionRetireDisposition::Unchanged);
    CHECK(store.Admit(second.Value()).Value().disposition == PlatformProgressionAdmissionDisposition::Started);
}

TEST_CASE("Progression in-flight configuration accepts the hard maximum and rejects larger bounds",
          "[platform-services][progression][idempotency][capacity]") {
    PlatformProgressionIdempotencyStore atMaximum({.maximumInFlight = PlatformProgressionMaximumInFlightMutations});
    for (std::size_t index = 0; index < PlatformProgressionMaximumInFlightMutations; ++index) {
        const auto envelope = BuildPlatformProgressionMutationEnvelope(Candidate(1, index + 1));
        REQUIRE(envelope.HasValue());
        const auto admitted = atMaximum.Admit(envelope.Value());
        REQUIRE(admitted.HasValue());
        CHECK(admitted.Value().disposition == PlatformProgressionAdmissionDisposition::Started);
    }
    CHECK(atMaximum.InFlightCount() == PlatformProgressionMaximumInFlightMutations);

    const auto overCapacity = BuildPlatformProgressionMutationEnvelope(Candidate(2, PlatformProgressionMaximumInFlightMutations + 1));
    REQUIRE(overCapacity.HasValue());
    const auto full = atMaximum.Admit(overCapacity.Value());
    REQUIRE(full.HasError());
    CHECK(full.ErrorValue().code.Value() == "platform.progression.capacity_exceeded");

    PlatformProgressionIdempotencyStore oversized({.maximumInFlight = PlatformProgressionMaximumInFlightMutations + 1});
    const auto validEnvelope = BuildPlatformProgressionMutationEnvelope(Candidate());
    REQUIRE(validEnvelope.HasValue());
    const auto rejected = oversized.Admit(validEnvelope.Value());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "platform.progression.invalid_configuration");

    PlatformProgressionIdempotencyStore unrepresentable({.maximumInFlight = std::numeric_limits<std::size_t>::max()});
    const auto rejectedBeforeAllocation = unrepresentable.Admit(validEnvelope.Value());
    REQUIRE(rejectedBeforeAllocation.HasError());
    CHECK(rejectedBeforeAllocation.ErrorValue().code.Value() == "platform.progression.invalid_configuration");
}

TEST_CASE("Different subject partitions do not join or conflict when generations match", "[platform-services][progression][idempotency]") {
    const auto first = BuildPlatformProgressionMutationEnvelope(Candidate());
    auto otherSubjectCandidate = Candidate();
    otherSubjectCandidate.scope.subjectPartition = SubjectPartition(12);
    const auto otherSubject = BuildPlatformProgressionMutationEnvelope(otherSubjectCandidate);
    REQUIRE(first.HasValue());
    REQUIRE(otherSubject.HasValue());

    PlatformProgressionIdempotencyStore store({.maximumInFlight = 2});
    const auto firstAdmission = store.Admit(first.Value());
    const auto otherSubjectAdmission = store.Admit(otherSubject.Value());
    REQUIRE(firstAdmission.HasValue());
    REQUIRE(otherSubjectAdmission.HasValue());
    CHECK(firstAdmission.Value().disposition == PlatformProgressionAdmissionDisposition::Started);
    CHECK(otherSubjectAdmission.Value().disposition == PlatformProgressionAdmissionDisposition::Started);
    CHECK(store.InFlightCount() == 2);

    auto forcedCollision = otherSubject.Value();
    forcedCollision.mutation = first.Value().mutation;
    const auto conflict = store.Admit(forcedCollision);
    REQUIRE(conflict.HasError());
    CHECK(conflict.ErrorValue().code.Value() == "platform.progression.idempotency_conflict");
}

TEST_CASE("Concurrent exact submissions produce one owner and joined observers",
          "[platform-services][progression][idempotency][concurrency]") {
    const auto envelope = BuildPlatformProgressionMutationEnvelope(Candidate());
    REQUIRE(envelope.HasValue());
    PlatformProgressionIdempotencyStore store({.maximumInFlight = 4});

    std::atomic_uint started{};
    std::atomic_uint joined{};
    std::atomic_uint failures{};
    std::vector<std::thread> threads;
    threads.reserve(32);
    for (std::size_t index = 0; index < 32; ++index) {
        threads.emplace_back([&] {
            const auto admitted = store.Admit(envelope.Value());
            if (admitted.HasError()) {
                ++failures;
                return;
            }
            if (admitted.Value().disposition == PlatformProgressionAdmissionDisposition::Started)
                ++started;
            else
                ++joined;
        });
    }
    for (auto &thread : threads)
        thread.join();

    CHECK(started.load() == 1);
    CHECK(joined.load() == 31);
    CHECK(failures.load() == 0);
    CHECK(store.InFlightCount() == 1);
}

TEST_CASE("Shutdown closes progression admission and removes session-scoped in-flight records",
          "[platform-services][progression][idempotency][shutdown]") {
    const auto envelope = BuildPlatformProgressionMutationEnvelope(Candidate());
    REQUIRE(envelope.HasValue());
    PlatformProgressionIdempotencyStore store;
    REQUIRE(store.Admit(envelope.Value()).HasValue());

    store.Shutdown();
    store.Shutdown();
    CHECK(store.IsClosed());
    CHECK(store.InFlightCount() == 0);
    CHECK_FALSE(store.Find(envelope.Value().mutation, envelope.Value().scope).has_value());

    const auto rejected = store.Admit(envelope.Value());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "platform.progression.closed");
}
