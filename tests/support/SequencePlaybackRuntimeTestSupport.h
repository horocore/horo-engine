/** @file
 * @brief Private shared fixtures for playback admission, coordination and lifecycle regressions.
 */
#pragma once

#include "Horo/Cinematic/SequenceEvaluationErrors.h"
#include "Horo/Cinematic/SequencePlaybackRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace Horo::Cinematic::PlaybackTestSupport {
    [[nodiscard]] constexpr CinematicRuntimeSessionId Session() {
        return {41, 3};
    }

    [[nodiscard]] constexpr SequencePlayerHandle Handle(const std::uint64_t player = 7, const std::uint32_t generation = 1) {
        return {Session(), {player, generation}};
    }

    [[nodiscard]] constexpr SequenceRestoreTargetId RestoreTarget(const std::uint64_t value, const std::uint32_t generation = 1) {
        return {value, generation};
    }

    [[nodiscard]] constexpr SequenceAuthorityTargetId AuthorityTarget(const std::uint64_t value, const std::uint32_t generation = 1) {
        return {value, generation};
    }

    [[nodiscard]] inline SequenceFrameEvaluationPlan Plan(const SequenceLoopMode loop = SequenceLoopMode::Once) {
        auto plan = SequenceFrameEvaluationPlan::Create(10, loop, 4, {}, {}, {});
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }

    [[nodiscard]] inline CinematicRuntimeService Service() {
        auto service = CinematicRuntimeService::Create({Session(), SequenceCookTier::Standard});
        REQUIRE(service.HasValue());
        return std::move(service).Value();
    }

    struct RestoreProbe final {
        float value{};
    };

    inline bool ApplyRestore(void *context, const float value) noexcept {
        static_cast<RestoreProbe *>(context)->value = value;
        return true;
    }

    inline bool RejectRestore(void *, float) noexcept {
        return false;
    }

    struct ValueProbe final {
        float value{4.0F};
        std::size_t writes{};
    };

    inline Result<float> SampleValue(const void *, const SequenceTime time) {
        return Result<float>::Success(10.0F + static_cast<float>(time));
    }

    inline void ApplyValue(void *context, const float value) noexcept {
        auto &probe = *static_cast<ValueProbe *>(context);
        probe.value = value;
        ++probe.writes;
    }

    struct CoordinationProbe final {
        std::uint64_t nextRevision{1};
        std::size_t acquired{};
        std::size_t released{};
    };

    inline Result<SequenceCoordinationLease> AcquireCoordination(void *context, const SequencePlayerHandle &player,
                                                                 const SequenceCoordinationLeaseKind kind) {
        auto &probe = *static_cast<CoordinationProbe *>(context);
        ++probe.acquired;
        return Result<SequenceCoordinationLease>::Success({player, kind, probe.nextRevision++});
    }

    inline void ReleaseCoordination(void *context, const SequenceCoordinationLease &) noexcept {
        ++static_cast<CoordinationProbe *>(context)->released;
    }

    struct PlaybackProbe final {
        CinematicRuntimeService *service{};
        std::array<SequenceFrameEventOccurrence, 16> events{};
        std::size_t eventCount{};
        std::size_t finishedCount{};
        SequencePlaybackState stateAtFinish{SequencePlaybackState::Ready};
    };

    inline Result<void> OnPlaybackEvent(const BorrowedCallbackContext &context,
                                        const std::span<const SequenceFrameEventOccurrence> events) {
        auto &probe = *context.Get<PlaybackProbe>();
        for (const SequenceFrameEventOccurrence &event : events)
            probe.events[probe.eventCount++] = event;
        return Result<void>::Success();
    }

    inline void OnPlaybackFinished(void *context, const SequencePlayerHandle &handle) noexcept {
        auto &probe = *static_cast<PlaybackProbe *>(context);
        ++probe.finishedCount;
        const auto snapshot = probe.service->Snapshot(handle);
        if (snapshot.HasValue())
            probe.stateAtFinish = snapshot.Value().state;
    }

    [[nodiscard]] inline SequenceFrameHooks PlaybackHooks(PlaybackProbe &probe) {
        return {BorrowedCallbackContext{&probe}, OnPlaybackEvent, {}, nullptr, &probe, OnPlaybackFinished};
    }

    [[nodiscard]] inline SequenceFrameEvaluationPlan EventPlan(const SequenceLoopMode mode) {
        const std::array events{SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{1, 1}, 0, true},
                                SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{2, 1}, 5, true},
                                SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{3, 1}, 10, true}};
        auto plan = SequenceFrameEvaluationPlan::Create(10, mode, 4, {}, events, {});
        REQUIRE(plan.HasValue());
        return std::move(plan).Value();
    }

    struct EventScratch final {
        std::array<SequenceFrameEventOccurrence, 16> events{};

        [[nodiscard]] SequenceFrameScratch View() {
            return {{}, events, {}};
        }
    };

    template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace Horo::Cinematic::PlaybackTestSupport
