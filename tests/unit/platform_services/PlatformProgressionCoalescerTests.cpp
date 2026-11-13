#include "AllocationProbe.h"
#include "Horo/PlatformServices/PlatformProgressionCoalescer.h"
#include "Horo/PlatformServices/PlatformRequest.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace {
    using namespace Horo::PlatformServices;
    constexpr auto Highest = LeaderboardOrdering::HighestFirst;

    PlatformProgressionMutationEnvelope Envelope(std::uint8_t occurrence, std::int64_t value,
                                                 PlatformProgressionMutationKind kind = PlatformProgressionMutationKind::SetStatMaximum) {
        PlatformProgressionMutationCandidate candidate;
        candidate.occurrence.bytes.back() = static_cast<std::byte>(occurrence);
        candidate.scope = {.provider = {1}, .session = {2}, .accessPolicy = {3}};
        candidate.scope.subjectPartition.digest.bytes.back() = 1;
        candidate.definitionKind = PlatformServiceIdKind::Stat;
        candidate.definition = {42};
        candidate.kind = kind;
        candidate.value = value;
        candidate.policy = {1};
        if (kind == PlatformProgressionMutationKind::SubmitBestScore || kind == PlatformProgressionMutationKind::ReplaceScoreAtRevision)
            candidate.definitionKind = PlatformServiceIdKind::Leaderboard;
        if (kind == PlatformProgressionMutationKind::SetStatSnapshot || kind == PlatformProgressionMutationKind::ReplaceScoreAtRevision)
            candidate.expectedRevision = 7;
        const auto result = BuildPlatformProgressionMutationEnvelope(candidate);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    std::unique_ptr<PlatformProgressionCoalescer> Owner(std::size_t capacity = 32) {
        auto result = PlatformProgressionCoalescer::Create({capacity, 5});
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

TEST_CASE("Progression bounded windows preserve extrema and original envelopes in every order",
          "[platform-services][progression][pls004-contract][coalescing]") {
    for (const auto kind : {PlatformProgressionMutationKind::SetStatMaximum, PlatformProgressionMutationKind::SetStatMinimum,
                            PlatformProgressionMutationKind::SubmitBestScore}) {
        for (const auto ordering : {Highest, LeaderboardOrdering::LowestFirst}) {
            std::array<std::int64_t, 3> values{-9, 2, 15};
            do {
                auto owner = Owner();
                std::array<PlatformProgressionMutationEnvelope, 3> envelopes;
                for (std::size_t index = 0; index < values.size(); ++index) {
                    envelopes[index] = Envelope(static_cast<std::uint8_t>(index + 1), values[index], kind);
                    REQUIRE(owner->Admit(envelopes[index], ordering, index).HasValue());
                }
                REQUIRE(owner->TakeReady(4).Value() == std::nullopt);
                const auto dispatch = owner->TakeReady(5).Value();
                REQUIRE(dispatch.has_value());
                const bool minimum =
                    kind == PlatformProgressionMutationKind::SetStatMinimum ||
                    (kind == PlatformProgressionMutationKind::SubmitBestScore && ordering == LeaderboardOrdering::LowestFirst);
                CHECK(std::get<std::int64_t>(dispatch->envelope.value) == (minimum ? -9 : 15));
                CHECK(std::ranges::find(envelopes, dispatch->envelope) != envelopes.end());
                REQUIRE(owner->Complete(*dispatch, PlatformProgressionOutcome::Failed, PlatformProviderFailureCategory::PermanentFailure)
                            .HasValue());
                for (const auto &envelope : envelopes) {
                    CHECK(owner->Observe(envelope.mutation) == PlatformProgressionOutcome::Failed);
                    CHECK(owner->FailureCategory(envelope.mutation) == PlatformProviderFailureCategory::PermanentFailure);
                    CHECK(owner->Admit(envelope, ordering, 5).Value().disposition ==
                          PlatformProgressionAdmissionDisposition::JoinedExisting);
                }
                CHECK_FALSE(owner->TakeReady(5).Value().has_value());
                CHECK(owner->Complete(*dispatch, PlatformProgressionOutcome::Succeeded).HasError());
            } while (std::next_permutation(values.begin(), values.end()));
        }
    }
}

TEST_CASE("Progression dispatch freezes identity and serializes late arrivals",
          "[platform-services][progression][pls004-contract][coalescing]") {
    auto owner = Owner();
    const auto first = Envelope(1, 5);
    const auto later = Envelope(2, 90);
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    REQUIRE(owner->Admit(later, Highest, 5).HasValue());
    CHECK_FALSE(owner->TakeReady(10).Value().has_value());
    CHECK(dispatch.envelope == first);
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded).HasValue());
    const auto next = owner->TakeReady(10).Value().value();
    CHECK(next.envelope == later);
    CHECK(owner->Observe(first.mutation) == PlatformProgressionOutcome::Succeeded);
}

TEST_CASE("Progression conditional and additive intents never coalesce", "[platform-services][progression][pls004-contract][coalescing]") {
    for (const auto kind : {PlatformProgressionMutationKind::SetStatSnapshot, PlatformProgressionMutationKind::AddStatOnce,
                            PlatformProgressionMutationKind::ReplaceScoreAtRevision}) {
        auto owner = Owner();
        const auto first = Envelope(1, 3, kind);
        const auto second = Envelope(2, 8, kind);
        REQUIRE(owner->Admit(first, Highest, 0).HasValue());
        REQUIRE(owner->Admit(second, Highest, 0).HasValue());
        const auto dispatch = owner->TakeReady(5).Value().value();
        CHECK(dispatch.envelope == first);
        CHECK(owner->Observe(second.mutation) == PlatformProgressionOutcome::Pending);
        REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded).HasValue());
        CHECK(owner->TakeReady(5).Value()->envelope == second);
    }
}

TEST_CASE("Progression clocks cancellation retention and unknown effects fail closed",
          "[platform-services][progression][pls004-contract][coalescing]") {
    auto owner = Owner(2);
    const auto first = Envelope(1, 2);
    const auto second = Envelope(2, 9);
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    REQUIRE(owner->Admit(second, Highest, 1).HasValue());
    CHECK(owner->Admit(Envelope(3, 3), Highest, 1).HasError());
    auto conflict = first;
    conflict.value = std::int64_t{8};
    CHECK(owner->Admit(conflict, Highest, 1).HasError());
    REQUIRE(owner->Cancel(second.mutation).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    CHECK(dispatch.envelope == first);
    CHECK(owner->TakeReady(4).HasError());
    owner->Close();
    owner->Close();
    CHECK(owner->Observe(first.mutation) == PlatformProgressionOutcome::RemoteOutcomeUnknown);
    CHECK(owner->Observe(second.mutation) == PlatformProgressionOutcome::Cancelled);
    CHECK(owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded).HasError());
    CHECK(owner->Admit(first, Highest, 5).HasError());
}

TEST_CASE("Progression concurrent duplicate arrival and completion retain one immutable outcome",
          "[platform-services][progression][pls004-contract][coalescing]") {
    auto owner = Owner();
    const auto envelope = Envelope(1, 20);
    REQUIRE(owner->Admit(envelope, Highest, 0).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    std::atomic<unsigned> joined{};
    std::array<std::jthread, 8> threads;
    for (auto &thread : threads)
        thread = std::jthread([&] {
            const auto result = owner->Admit(envelope, Highest, 5);
            if (result.HasValue() && result.Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting)
                ++joined;
        });
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Failed).HasValue());
    for (auto &thread : threads)
        thread.join();
    CHECK(joined == threads.size());
    CHECK(owner->Observe(envelope.mutation) == PlatformProgressionOutcome::Failed);
    CHECK_FALSE(owner->TakeReady(5).Value().has_value());
}

TEST_CASE("Progression ambiguity fences the lane until exact conclusive reconciliation",
          "[platform-services][progression][pls004-contract][coalescing]") {
    auto owner = Owner();
    const auto first = Envelope(1, 2);
    const auto second = Envelope(2, 9);
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::RemoteOutcomeUnknown).HasValue());
    REQUIRE(owner->Admit(second, Highest, 5).HasValue());
    CHECK_FALSE(owner->TakeReady(10).Value().has_value());
    auto forged = dispatch;
    forged.envelope.value = std::int64_t{99};
    CHECK(owner->Complete(forged, PlatformProgressionOutcome::Succeeded).HasError());
    CHECK(owner->Observe(first.mutation) == PlatformProgressionOutcome::RemoteOutcomeUnknown);
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded).HasValue());
    CHECK(owner->TakeReady(10).Value()->envelope == second);
}

TEST_CASE("Progression unrelated targets may complete out of order without crossing receipts",
          "[platform-services][progression][pls004-contract][coalescing][qualification]") {
    auto owner = Owner();
    const auto first = Envelope(1, 2);
    auto second = Envelope(2, 9);
    second.definition.value = 43;
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    REQUIRE(owner->Admit(second, Highest, 0).HasValue());
    const auto firstDispatch = owner->TakeReady(5).Value().value();
    const auto secondDispatch = owner->TakeReady(5).Value().value();
    REQUIRE(owner->Complete(secondDispatch, PlatformProgressionOutcome::Succeeded).HasValue());
    CHECK(owner->Observe(first.mutation) == PlatformProgressionOutcome::InFlight);
    CHECK(owner->Observe(second.mutation) == PlatformProgressionOutcome::Succeeded);
    REQUIRE(owner->Complete(firstDispatch, PlatformProgressionOutcome::Failed, PlatformProviderFailureCategory::Forbidden).HasValue());
    CHECK(owner->FailureCategory(first.mutation) == PlatformProviderFailureCategory::Forbidden);
    CHECK_FALSE(owner->FailureCategory(second.mutation).has_value());
}

TEST_CASE("Progression allocation failure cannot publish a partially initialized owner",
          "[platform-services][progression][pls004-contract][coalescing]") {
    for (std::size_t successfulAllocations = 0; successfulAllocations < 4; ++successfulAllocations) {
        const Horo::Tests::AllocationProbe::ScopedFailure fail(successfulAllocations);
        const auto result = PlatformProgressionCoalescer::Create();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code == PlatformProgressionErrors::CapacityExceeded.code);
    }
}

TEST_CASE("Progression rejected admission preserves the prior scheduling clock",
          "[platform-services][progression][pls004-contract][coalescing]") {
    auto owner = Owner();
    const auto first = Envelope(1, 2);
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    auto conflict = first;
    conflict.value = std::int64_t{100};
    REQUIRE(owner->Admit(conflict, Highest, 100).HasError());
    const auto ready = owner->TakeReady(5);
    REQUIRE(ready.HasValue());
    REQUIRE(ready.Value().has_value());
    CHECK(ready.Value()->envelope == first);
}

TEST_CASE("Progression windows never merge policy subject generation or numeric representation",
          "[platform-services][progression][pls004-contract][coalescing]") {
    for (unsigned field = 0; field != 7; ++field) {
        auto owner = Owner();
        const auto first = Envelope(1, 3);
        auto second = Envelope(2, 8);
        switch (field) {
            case 0:
                ++second.policy.value;
                break;
            case 1:
                ++second.scope.session.value;
                break;
            case 2:
                ++second.scope.provider.value;
                break;
            case 3:
                ++second.scope.accessPolicy.value;
                break;
            case 4:
                ++second.scope.subjectPartition.digest.bytes.back();
                break;
            case 5:
                second.value = std::uint64_t{8};
                break;
            case 6:
                second.authority = ProgressionAuthorityMode::AuthorityServer;
                break;
        }
        REQUIRE(owner->Admit(first, Highest, 0).HasValue());
        REQUIRE(owner->Admit(second, Highest, 0).HasValue());
        CHECK(owner->TakeReady(5).Value()->envelope == first);
        CHECK(owner->Observe(second.mutation) == PlatformProgressionOutcome::Pending);
    }
}

TEST_CASE("Progression late reconciliation never rewrites an ADR-130 timeout",
          "[platform-services][progression][pls004-contract][qualification]") {
    auto owner = Owner();
    Horo::PlatformServices::PlatformRequestStore requests;
    const auto first = Envelope(1, 2);
    const auto second = Envelope(2, 9);
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    REQUIRE(owner->Admit(second, Highest, 1).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    auto request = requests.Admit<void>();
    REQUIRE(request.HasValue());
    REQUIRE(requests.MarkRunning(request.Value()).HasValue());
    REQUIRE(requests.CompleteTimedOut(request.Value(), Horo::MakeError(RequestErrors::TimedOut)).HasValue());
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::RemoteOutcomeUnknown).HasValue());
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded).HasValue());
    REQUIRE(requests.CompleteSuccess(request.Value()).HasValue());
    const auto snapshot = requests.Query(request.Value());
    REQUIRE(snapshot.HasValue());
    CHECK(snapshot.Value().state == PlatformRequestState::TimedOut);
    REQUIRE(snapshot.Value().terminal.has_value());
    REQUIRE(snapshot.Value().terminal->ErrorValue() != nullptr);
    CHECK(snapshot.Value().terminal->ErrorValue()->code == RequestErrors::TimedOut.code);
    CHECK(owner->Observe(first.mutation) == PlatformProgressionOutcome::Succeeded);
    CHECK(owner->Observe(second.mutation) == PlatformProgressionOutcome::Succeeded);
    CHECK(owner->Admit(first, Highest, 5).Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting);
    CHECK_FALSE(owner->TakeReady(5).Value().has_value());
}

TEST_CASE("Progression normalized offline signed-out throttle and permanent failures remain observable",
          "[platform-services][progression][pls004-contract][qualification]") {
    for (const auto category : {PlatformProviderFailureCategory::Offline, PlatformProviderFailureCategory::NotSignedIn,
                                PlatformProviderFailureCategory::RateLimited, PlatformProviderFailureCategory::PermanentFailure}) {
        auto owner = Owner();
        const auto first = Envelope(1, 2);
        const auto second = Envelope(2, 9);
        REQUIRE(owner->Admit(first, Highest, 0).HasValue());
        REQUIRE(owner->Admit(second, Highest, 1).HasValue());
        const auto dispatch = owner->TakeReady(5).Value().value();
        REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Failed, category).HasValue());
        for (const auto &envelope : {first, second}) {
            CHECK(owner->Observe(envelope.mutation) == PlatformProgressionOutcome::Failed);
            CHECK(owner->FailureCategory(envelope.mutation) == category);
            CHECK(owner->Admit(envelope, Highest, 5).Value().disposition == PlatformProgressionAdmissionDisposition::JoinedExisting);
        }
        CHECK_FALSE(owner->TakeReady(5).Value().has_value());
    }
}

TEST_CASE("Progression concurrent stronger arrivals cannot mutate or overtake active dispatch",
          "[platform-services][progression][pls004-contract][coalescing][qualification]") {
    auto owner = Owner();
    const auto first = Envelope(1, 2);
    const std::array arrivals{Envelope(2, 20), Envelope(3, 90)};
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    std::atomic<unsigned> accepted{};
    std::array<std::jthread, 2> threads;
    for (std::size_t index = 0; index < arrivals.size(); ++index)
        threads[index] = std::jthread([&, index] {
            if (owner->Admit(arrivals[index], Highest, 5).HasValue())
                ++accepted;
        });
    const auto completed = owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded);
    for (auto &thread : threads)
        thread.join();
    REQUIRE(completed.HasValue());
    REQUIRE(accepted == arrivals.size());
    CHECK(dispatch.envelope == first);
    const auto next = owner->TakeReady(10).Value().value();
    CHECK(next.envelope == arrivals.back());
    REQUIRE(owner->Complete(next, PlatformProgressionOutcome::Failed, PlatformProviderFailureCategory::PermanentFailure).HasValue());
    CHECK(owner->Observe(first.mutation) == PlatformProgressionOutcome::Succeeded);
    for (const auto &arrival : arrivals)
        CHECK(owner->FailureCategory(arrival.mutation) == PlatformProviderFailureCategory::PermanentFailure);
}

TEST_CASE("Progression invalid bounds and exact window boundaries do not extend debounce",
          "[platform-services][progression][pls004-contract][coalescing]") {
    CHECK(PlatformProgressionCoalescer::Create({0, 1}).HasError());
    CHECK(PlatformProgressionCoalescer::Create({1, 0}).HasError());
    CHECK(PlatformProgressionCoalescer::Create({PlatformProgressionMaximumInFlightMutations + 1, 1}).HasError());
    auto owner = Owner();
    const auto first = Envelope(1, 2);
    const auto second = Envelope(2, 9);
    REQUIRE(owner->Admit(first, Highest, 0).HasValue());
    REQUIRE(owner->Admit(second, Highest, 5).HasValue());
    const auto dispatch = owner->TakeReady(5).Value().value();
    CHECK(dispatch.envelope == first);
    CHECK(owner->Observe(second.mutation) == PlatformProgressionOutcome::Pending);
    REQUIRE(owner->Complete(dispatch, PlatformProgressionOutcome::Succeeded).HasValue());
    CHECK_FALSE(owner->TakeReady(9).Value().has_value());
    CHECK(owner->TakeReady(10).Value()->envelope == second);
}
