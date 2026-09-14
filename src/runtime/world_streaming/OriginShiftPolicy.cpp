#include "Horo/WorldStreaming/OriginShiftPolicy.h"

#include "Horo/WorldStreaming/WorldStreamingErrors.h"

#include <array>

namespace Horo::WorldStreaming {
    namespace {
        [[nodiscard]] bool IsKnown(const OriginShiftRuntimeMode mode) noexcept {
            return mode < OriginShiftRuntimeMode::Count;
        }

        [[nodiscard]] bool IsKnown(const OriginShiftRequester requester) noexcept {
            return requester < OriginShiftRequester::Count;
        }

        [[nodiscard]] bool IsKnown(const OriginShiftPolicyState state) noexcept {
            return state < OriginShiftPolicyState::Count;
        }

        [[nodiscard]] bool IsValidThreshold(const std::uint64_t value) noexcept {
            return value > 0 && value <= static_cast<std::uint64_t>(OriginFrame::MaximumLocalHalfExtentMillimeters);
        }

        [[nodiscard]] bool IsAuthorized(const OriginShiftRuntimeMode mode, const OriginShiftRequester requester) noexcept {
            using enum OriginShiftRuntimeMode;
            switch (mode) {
                case StandaloneGameplay:
                case AuthoritativeServer:
                    return requester == OriginShiftRequester::Gameplay;
                case EditorPreview:
                    return requester == OriginShiftRequester::Gameplay || requester == OriginShiftRequester::Editor;
                case NetworkClient:
                    return requester == OriginShiftRequester::NetworkAuthority;
                case Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] std::uint64_t Threshold(const OriginShiftPolicyRequest &policy, const OriginShiftRequester requester) noexcept {
            using enum OriginShiftRequester;
            switch (requester) {
                case Gameplay:
                    return policy.gameplayThresholdMillimeters;
                case Editor:
                    return policy.editorThresholdMillimeters;
                case NetworkAuthority:
                    return policy.networkThresholdMillimeters;
                case Count:
                    return 0;
            }
            return 0;
        }

        [[nodiscard]] std::uint64_t AbsoluteDelta(const std::int64_t value, const std::int64_t origin) noexcept {
            return value >= origin ? static_cast<std::uint64_t>(value) - static_cast<std::uint64_t>(origin)
                                   : static_cast<std::uint64_t>(origin) - static_cast<std::uint64_t>(value);
        }

        [[nodiscard]] bool ExceedsThreshold(const Math::WorldCoordinate64 &focal, const Math::WorldCoordinate64 &origin,
                                            const std::uint64_t threshold) noexcept {
            const auto focalAxes = focal.Millimeters();
            const auto originAxes = origin.Millimeters();
            std::array<std::uint64_t, 3> delta{};
            for (std::size_t index = 0; index < delta.size(); ++index) {
                delta[index] = AbsoluteDelta(focalAxes[index], originAxes[index]);
                if (delta[index] > threshold)
                    return true;
            }
            const std::uint64_t squaredThreshold = threshold * threshold;
            std::uint64_t squaredDistance = 0;
            for (const auto axis : delta) {
                const std::uint64_t squaredAxis = axis * axis;
                if (squaredDistance > squaredThreshold - squaredAxis)
                    return true;
                squaredDistance += squaredAxis;
            }
            return squaredDistance > squaredThreshold;
        }

        [[nodiscard]] Result<void> ValidateEvaluationInput(const OriginShiftEvaluationContext &context,
                                                           const OriginShiftEvaluationRequest &request) {
            if (!context.policy.IsValid() || !context.policyRevision.IsValid() || !request.id.IsValid())
                return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyInvalid));
            if (!IsKnown(context.state) || !IsKnown(request.requester))
                return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyUnsupported));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEvaluationFence(const OriginShiftPolicyRequest &policy,
                                                           const OriginShiftEvaluationContext &context) {
            if (context.policy != policy.id || context.policyRevision != policy.revision ||
                context.activeFrame.Binding().identity != policy.frame)
                return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyStale));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEvaluationLifecycle(const OriginShiftPolicyRequest &policy,
                                                               const OriginShiftEvaluationContext &context,
                                                               const OriginShiftEvaluationRequest &request) {
            if (context.state != OriginShiftPolicyState::Active)
                return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyLifecycleUnavailable));
            if (!IsAuthorized(policy.mode, request.requester))
                return Result<void>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyUnauthorized));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OriginShiftPolicy::Create */
    Result<OriginShiftPolicy> OriginShiftPolicy::Create(const OriginShiftPolicyRequest &request) {
        if (request.contractVersion != OriginShiftPolicyRequest::CurrentContractVersion || !IsKnown(request.mode))
            return Result<OriginShiftPolicy>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyUnsupported));
        if (!request.id.IsValid() || !request.revision.IsValid() || !request.frame.IsValid() ||
            !IsValidThreshold(request.gameplayThresholdMillimeters) || !IsValidThreshold(request.editorThresholdMillimeters) ||
            !IsValidThreshold(request.networkThresholdMillimeters))
            return Result<OriginShiftPolicy>::Failure(MakeError(WorldStreamingErrors::OriginShiftPolicyInvalid));
        return Result<OriginShiftPolicy>::Success(OriginShiftPolicy{request});
    }

    OriginShiftPolicy::OriginShiftPolicy(const OriginShiftPolicyRequest &request) noexcept : request_(request) {}

    /** @copydoc OriginShiftPolicy::Id */
    OriginShiftPolicyId OriginShiftPolicy::Id() const noexcept {
        return request_.id;
    }

    /** @copydoc OriginShiftPolicy::Revision */
    OriginShiftPolicyRevision OriginShiftPolicy::Revision() const noexcept {
        return request_.revision;
    }

    /** @copydoc EvaluateOriginShift */
    Result<OriginShiftDecision> EvaluateOriginShift(const OriginShiftPolicy &policy, const OriginShiftEvaluationContext &context,
                                                    const OriginShiftEvaluationRequest &request) {
        if (const auto valid = ValidateEvaluationInput(context, request); valid.HasError())
            return Result<OriginShiftDecision>::Failure(valid.ErrorValue());
        if (const auto current = ValidateEvaluationFence(policy.request_, context); current.HasError())
            return Result<OriginShiftDecision>::Failure(current.ErrorValue());
        if (const auto admitted = ValidateEvaluationLifecycle(policy.request_, context, request); admitted.HasError())
            return Result<OriginShiftDecision>::Failure(admitted.ErrorValue());

        const auto threshold = Threshold(policy.request_, request.requester);
        const bool requestShift = ExceedsThreshold(request.focalPosition, context.activeFrame.Origin(), threshold);
        return Result<OriginShiftDecision>::Success(
            {.request = request.id,
             .observedFrame = context.activeFrame.Binding(),
             .requester = request.requester,
             .kind = requestShift ? OriginShiftDecisionKind::RequestShift : OriginShiftDecisionKind::Remain,
             .thresholdMillimeters = threshold,
             .targetOrigin = requestShift ? request.focalPosition : context.activeFrame.Origin()});
    }
}  // namespace Horo::WorldStreaming
