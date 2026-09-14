#pragma once

/**
 * @file OriginShiftPolicy.h
 * @brief Typed threshold and requester-authority policy for floating-origin shifts.
 */

#include "Horo/WorldStreaming/OriginFrame.h"

#include <compare>
#include <cstdint>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct OriginShiftPolicyIdTag;
        struct OriginShiftPolicyRevisionTag;
        struct OriginShiftRequestIdTag;
    }  // namespace Detail

    /** @brief Stable identity of one host-owned origin-shift policy. */
    using OriginShiftPolicyId = Foundation::Detail::NonZeroId64<Detail::OriginShiftPolicyIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact immutable publication revision of one origin-shift policy. */
    using OriginShiftPolicyRevision =
        Foundation::Detail::NonZeroId64<Detail::OriginShiftPolicyRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Stable identity of one origin-shift evaluation request. */
    using OriginShiftRequestId = Foundation::Detail::NonZeroId64<Detail::OriginShiftRequestIdTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Runtime composition that determines which requester owns shift authority. */
    enum class OriginShiftRuntimeMode : std::uint8_t {
        StandaloneGameplay,
        EditorPreview,
        AuthoritativeServer,
        NetworkClient,
        Count,
    };

    /** @brief Typed source requesting threshold evaluation. */
    enum class OriginShiftRequester : std::uint8_t {
        Gameplay,
        Editor,
        NetworkAuthority,
        Count,
    };

    /** @brief Lifecycle gate captured at one host safe point. */
    enum class OriginShiftPolicyState : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count,
    };

    /** @brief Outcome of an authorized threshold evaluation. */
    enum class OriginShiftDecisionKind : std::uint8_t {
        Remain,
        RequestShift,
    };

    /** @brief Versioned immutable configuration for requester authority and thresholds. */
    struct OriginShiftPolicyRequest final {
        static constexpr std::uint32_t CurrentContractVersion = 1;

        std::uint32_t contractVersion{CurrentContractVersion};                   /**< Exact supported in-memory contract version. */
        OriginShiftPolicyId id{};                                                /**< Stable policy owner identity. */
        OriginShiftPolicyRevision revision{};                                    /**< Exact immutable policy publication. */
        OriginFrameId frame{};                                                   /**< Stable local-frame owner governed by this policy. */
        OriginShiftRuntimeMode mode{OriginShiftRuntimeMode::StandaloneGameplay}; /**< Host composition and authority model. */
        std::uint64_t gameplayThresholdMillimeters{1'000'000};                   /**< Gameplay focal-distance threshold. */
        std::uint64_t editorThresholdMillimeters{1'000'000};                     /**< Editor-preview focal-distance threshold. */
        std::uint64_t networkThresholdMillimeters{1'000'000};                    /**< Server-issued client focal-distance threshold. */
    };

    /** @brief Immutable validated origin-shift policy with no ambient registration or activation. */
    class OriginShiftPolicy final {
    public:
        /**
         * @brief Validates and captures one complete origin-shift policy.
         * @param request Versioned host-authored policy request.
         * @return Immutable policy or a typed invalid or unsupported failure.
         */
        [[nodiscard]] static Result<OriginShiftPolicy> Create(const OriginShiftPolicyRequest &request);

        /** @brief Returns the stable policy owner. @return Non-zero identity. */
        [[nodiscard]] OriginShiftPolicyId Id() const noexcept;
        /** @brief Returns the exact policy publication. @return Non-zero revision. */
        [[nodiscard]] OriginShiftPolicyRevision Revision() const noexcept;

    private:
        explicit OriginShiftPolicy(const OriginShiftPolicyRequest &request) noexcept;

        friend Result<struct OriginShiftDecision> EvaluateOriginShift(const OriginShiftPolicy &,
                                                                      const struct OriginShiftEvaluationContext &,
                                                                      const struct OriginShiftEvaluationRequest &);

        OriginShiftPolicyRequest request_{};
    };

    /** @brief Exact policy, frame and lifecycle evidence captured at one host safe point. */
    struct OriginShiftEvaluationContext final {
        OriginShiftPolicyId policy{};                                 /**< Expected policy owner. */
        OriginShiftPolicyRevision policyRevision{};                   /**< Expected immutable publication. */
        OriginFrame activeFrame;                                      /**< Immutable frame publication used for distance evaluation. */
        OriginShiftPolicyState state{OriginShiftPolicyState::Closed}; /**< Current admission lifecycle. */
    };

    /** @brief One value-owned focal-position evaluation request. */
    struct OriginShiftEvaluationRequest final {
        OriginShiftRequestId id{};               /**< Stable request identity retained in the decision. */
        OriginShiftRequester requester{};        /**< Typed request authority. */
        Math::WorldCoordinate64 focalPosition{}; /**< Canonical focal position; never local fp32 authority. */
    };

    /** @brief Transparent immutable result ready for a later safe-point transaction. */
    struct OriginShiftDecision final {
        OriginShiftRequestId request{};         /**< Exact evaluated request. */
        OriginFrameBinding observedFrame{};     /**< Exact frame fence used by this decision. */
        OriginShiftRequester requester{};       /**< Authorized source that produced the decision. */
        OriginShiftDecisionKind kind{};         /**< Remain or request a later atomic shift. */
        std::uint64_t thresholdMillimeters{};   /**< Source-specific threshold applied. */
        Math::WorldCoordinate64 targetOrigin{}; /**< Canonical requested origin; current origin when remaining. */

        [[nodiscard]] constexpr auto operator<=>(const OriginShiftDecision &) const noexcept = default;
    };

    /**
     * @brief Authorizes a requester and evaluates its source-specific threshold against the active origin.
     * @param policy Immutable policy publication.
     * @param context Exact policy, active-frame and lifecycle evidence.
     * @param request Stable requester identity and canonical focal position.
     * @return Transparent decision or a typed invalid, unsupported, stale, unauthorized or lifecycle failure.
     * @details This pure evaluation performs no registration, allocation, frame replacement or participant callback. A
     *          RequestShift decision is only input to the later atomic safe-point transaction.
     */
    [[nodiscard]] Result<OriginShiftDecision> EvaluateOriginShift(const OriginShiftPolicy &policy,
                                                                  const OriginShiftEvaluationContext &context,
                                                                  const OriginShiftEvaluationRequest &request);
}  // namespace Horo::WorldStreaming
