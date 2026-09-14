#include "Horo/WorldStreaming/OriginShiftPolicy.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <set>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        [[nodiscard]] OriginFrameBinding FrameBinding(const std::uint64_t identity = 17, const std::uint64_t revision = 3,
                                                      const std::uint64_t generation = 9) {
            return {.identity = IdentityFrom<OriginFrameId>(identity),
                    .revision = IdentityFrom<OriginFrameRevision>(revision),
                    .generation = IdentityFrom<OriginGeneration>(generation)};
        }

        [[nodiscard]] OriginShiftPolicyRequest PolicyRequest(
            const OriginShiftRuntimeMode mode = OriginShiftRuntimeMode::StandaloneGameplay) {
            return {.id = IdentityFrom<OriginShiftPolicyId>(41),
                    .revision = IdentityFrom<OriginShiftPolicyRevision>(7),
                    .frame = IdentityFrom<OriginFrameId>(17),
                    .mode = mode,
                    .gameplayThresholdMillimeters = 1'000,
                    .editorThresholdMillimeters = 2'000,
                    .networkThresholdMillimeters = 3'000};
        }

        [[nodiscard]] OriginShiftEvaluationContext Context() {
            return {.policy = IdentityFrom<OriginShiftPolicyId>(41),
                    .policyRevision = IdentityFrom<OriginShiftPolicyRevision>(7),
                    .activeFrame =
                        OriginFrame::Create(FrameBinding(), Math::WorldCoordinate64::FromMillimeters(10'000, -20'000, 30'000)).Value(),
                    .state = OriginShiftPolicyState::Active};
        }

        [[nodiscard]] OriginShiftEvaluationRequest Request(const OriginShiftRequester requester, const std::int64_t xOffsetMillimeters) {
            return {.id = IdentityFrom<OriginShiftRequestId>(99),
                    .requester = requester,
                    .focalPosition = Math::WorldCoordinate64::FromMillimeters(10'000 + xOffsetMillimeters, -20'000, 30'000)};
        }

        [[nodiscard]] OriginShiftPolicy Policy(const OriginShiftRuntimeMode mode = OriginShiftRuntimeMode::StandaloneGameplay) {
            return OriginShiftPolicy::Create(PolicyRequest(mode)).Value();
        }
    }  // namespace

    TEST_CASE("Origin shift policy preserves exact threshold boundaries and canonical targets",
              "[unit][world_streaming][origin_shift_policy]") {
        const auto remain = EvaluateOriginShift(Policy(), Context(), Request(OriginShiftRequester::Gameplay, 1'000)).Value();
        CHECK(remain.kind == OriginShiftDecisionKind::Remain);
        CHECK(remain.thresholdMillimeters == 1'000);
        CHECK(remain.targetOrigin == Context().activeFrame.Origin());
        CHECK(remain.observedFrame == Context().activeFrame.Binding());

        const auto shift = EvaluateOriginShift(Policy(), Context(), Request(OriginShiftRequester::Gameplay, 1'001)).Value();
        CHECK(shift.kind == OriginShiftDecisionKind::RequestShift);
        CHECK(shift.targetOrigin == Request(OriginShiftRequester::Gameplay, 1'001).focalPosition);
        CHECK(shift.request == IdentityFrom<OriginShiftRequestId>(99));
    }

    TEST_CASE("Origin shift policy uses radial distance without overflowing extreme coordinates",
              "[unit][world_streaming][origin_shift_policy]") {
        auto diagonal = Request(OriginShiftRequester::Gameplay, 800);
        diagonal.focalPosition = Math::WorldCoordinate64::FromMillimeters(10'800, -19'200, 30'000);
        CHECK(EvaluateOriginShift(Policy(), Context(), diagonal).Value().kind == OriginShiftDecisionKind::RequestShift);

        auto extreme = Request(OriginShiftRequester::Gameplay, 0);
        extreme.focalPosition = Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::min(), -20'000, 30'000);
        CHECK(EvaluateOriginShift(Policy(), Context(), extreme).Value().kind == OriginShiftDecisionKind::RequestShift);
    }

    TEST_CASE("Origin shift authority matrix separates gameplay editor and network thresholds",
              "[unit][world_streaming][origin_shift_policy][authority]") {
        const auto editorPolicy = Policy(OriginShiftRuntimeMode::EditorPreview);
        CHECK(EvaluateOriginShift(editorPolicy, Context(), Request(OriginShiftRequester::Gameplay, 1'001)).Value().kind ==
              OriginShiftDecisionKind::RequestShift);
        CHECK(EvaluateOriginShift(editorPolicy, Context(), Request(OriginShiftRequester::Editor, 2'000)).Value().kind ==
              OriginShiftDecisionKind::Remain);
        CHECK(EvaluateOriginShift(editorPolicy, Context(), Request(OriginShiftRequester::Editor, 2'001)).Value().kind ==
              OriginShiftDecisionKind::RequestShift);
        RequireError(EvaluateOriginShift(editorPolicy, Context(), Request(OriginShiftRequester::NetworkAuthority, 3'001)),
                     WorldStreamingErrors::OriginShiftPolicyUnauthorized);

        const auto clientPolicy = Policy(OriginShiftRuntimeMode::NetworkClient);
        CHECK(EvaluateOriginShift(clientPolicy, Context(), Request(OriginShiftRequester::NetworkAuthority, 3'001)).Value().kind ==
              OriginShiftDecisionKind::RequestShift);
        RequireError(EvaluateOriginShift(clientPolicy, Context(), Request(OriginShiftRequester::Gameplay, 1'001)),
                     WorldStreamingErrors::OriginShiftPolicyUnauthorized);

        const auto serverPolicy = Policy(OriginShiftRuntimeMode::AuthoritativeServer);
        CHECK(EvaluateOriginShift(serverPolicy, Context(), Request(OriginShiftRequester::Gameplay, 1'001)).HasValue());
        RequireError(EvaluateOriginShift(serverPolicy, Context(), Request(OriginShiftRequester::NetworkAuthority, 3'001)),
                     WorldStreamingErrors::OriginShiftPolicyUnauthorized);
    }

    TEST_CASE("Origin shift policy validates versions identities modes and bounded thresholds",
              "[unit][world_streaming][origin_shift_policy]") {
        auto request = PolicyRequest();
        request.contractVersion += 1;
        RequireError(OriginShiftPolicy::Create(request), WorldStreamingErrors::OriginShiftPolicyUnsupported);
        request = PolicyRequest();
        request.mode = OriginShiftRuntimeMode::Count;
        RequireError(OriginShiftPolicy::Create(request), WorldStreamingErrors::OriginShiftPolicyUnsupported);
        request = PolicyRequest();
        request.id = {};
        RequireError(OriginShiftPolicy::Create(request), WorldStreamingErrors::OriginShiftPolicyInvalid);
        request = PolicyRequest();
        request.gameplayThresholdMillimeters = 0;
        RequireError(OriginShiftPolicy::Create(request), WorldStreamingErrors::OriginShiftPolicyInvalid);
        request = PolicyRequest();
        request.networkThresholdMillimeters = static_cast<std::uint64_t>(OriginFrame::MaximumLocalHalfExtentMillimeters) + 1;
        RequireError(OriginShiftPolicy::Create(request), WorldStreamingErrors::OriginShiftPolicyInvalid);
    }

    TEST_CASE("Origin shift evaluation fences stale replacement cancellation shutdown and invalid requests",
              "[unit][world_streaming][origin_shift_policy][lifecycle]") {
        auto context = Context();
        context.policyRevision = IdentityFrom<OriginShiftPolicyRevision>(8);
        RequireError(EvaluateOriginShift(Policy(), context, Request(OriginShiftRequester::Gameplay, 1'001)),
                     WorldStreamingErrors::OriginShiftPolicyStale);
        context = Context();
        context.activeFrame =
            OriginFrame::Create(FrameBinding(18), Math::WorldCoordinate64::FromMillimeters(10'000, -20'000, 30'000)).Value();
        RequireError(EvaluateOriginShift(Policy(), context, Request(OriginShiftRequester::Gameplay, 1'001)),
                     WorldStreamingErrors::OriginShiftPolicyStale);
        context = Context();
        context.state = OriginShiftPolicyState::Cancelling;
        RequireError(EvaluateOriginShift(Policy(), context, Request(OriginShiftRequester::Gameplay, 1'001)),
                     WorldStreamingErrors::OriginShiftPolicyLifecycleUnavailable);
        context.state = OriginShiftPolicyState::Closed;
        RequireError(EvaluateOriginShift(Policy(), context, Request(OriginShiftRequester::Gameplay, 1'001)),
                     WorldStreamingErrors::OriginShiftPolicyLifecycleUnavailable);
        context = Context();
        auto invalid = Request(OriginShiftRequester::Gameplay, 1'001);
        invalid.id = {};
        RequireError(EvaluateOriginShift(Policy(), context, invalid), WorldStreamingErrors::OriginShiftPolicyInvalid);
        invalid = Request(OriginShiftRequester::Count, 1'001);
        RequireError(EvaluateOriginShift(Policy(), context, invalid), WorldStreamingErrors::OriginShiftPolicyUnsupported);
    }

    TEST_CASE("Origin shift policy errors expose unique stable descriptors", "[unit][world_streaming][origin_shift_policy][errors]") {
        const std::array descriptors{&WorldStreamingErrors::OriginShiftPolicyInvalid, &WorldStreamingErrors::OriginShiftPolicyUnsupported,
                                     &WorldStreamingErrors::OriginShiftPolicyStale, &WorldStreamingErrors::OriginShiftPolicyUnauthorized,
                                     &WorldStreamingErrors::OriginShiftPolicyLifecycleUnavailable};
        std::set<std::string_view> codes;
        for (const auto *descriptor : descriptors) {
            CHECK(descriptor->domain.Value() == std::string_view{"horo.world_streaming"});
            CHECK(codes.insert(descriptor->code.Value()).second);
        }
    }
}  // namespace Horo::WorldStreaming
