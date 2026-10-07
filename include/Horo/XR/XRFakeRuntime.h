#pragma once

/** @file XRFakeRuntime.h
 * @brief Deterministic, headset-free host harness for the production XR lifecycle contracts.
 */
#include "Horo/XR/XRFrameLifecycle.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::XR {
    /** @brief Explicit simulated operation failure; no native or GPU success is claimed. */
    enum class XRFakeFailure : std::uint8_t {
        None,
        Wait,
        Begin,
        Locate,
        Acquire,
        Submit,
        Release,
        End,
        Count
    };
    /** @brief Backend-neutral scalar action evidence; semantic Input projection remains host-owned. */
    enum class XRFakeActionKind : std::uint8_t {
        Boolean,
        Float,
        Vector2,
        Count
    };

    /** @brief Exact session-owned action sample with finite values and explicit availability. */
    struct XRFakeAction final {
        XRActionId action;
        XRFakeActionKind kind{XRFakeActionKind::Boolean};
        std::array<float, 2> value{};
        bool active{};
    };

    /** @brief Hard ceilings for script admission and allocation-free step publications. */
    struct XRFakeLimits final {
        static constexpr std::size_t MaximumSteps = 256;
        static constexpr std::size_t MaximumActions = 64;
    };

    /** @brief Owned script step; event-only steps do not reserve a predicted frame. */
    struct XRFakeStep final {
        std::optional<XRSessionEvent> event;
        bool executeFrame{true};
        XRRenderPredictionTime prediction;
        bool shouldRender{true};
        XRFakeFailure failure{XRFakeFailure::None};
        std::uint32_t viewCount{};
        std::array<std::optional<XRPoseSample>, XRFrameHardLimits::MaximumViews> views{};
        XRRuntimeTime actionSampleTime; /**< Runtime sample clock, never inferred from presentation prediction. */
        std::uint32_t actionCount{};
        std::array<XRFakeAction, XRFakeLimits::MaximumActions> actions{};
    };

    /** @brief Fixed owned result; a consumed failure is observable and leaves no open simulated frame. */
    struct XRFakeOutcome final {
        XRFrameStatus status{XRFrameStatus::Unavailable};
        XRFakeFailure failure{XRFakeFailure::None}; /**< Injected call failure; None for admission/exhaustion failures. */
        bool consumed{};
        std::size_t nextStep{};
        XRSessionSnapshot session;
        XRFrameSnapshot frame; /**< Diagnostic evidence captured before cleanup/end; no frame remains open. */
        std::uint32_t viewCount{};
        std::array<std::optional<XRPoseSample>, XRFrameHardLimits::MaximumViews> views{};
        XRRuntimeTime actionSampleTime; /**< Runtime sample clock, never inferred from presentation prediction. */
        std::uint32_t actionCount{};
        std::array<XRFakeAction, XRFakeLimits::MaximumActions> actions{};
    };

    /**
     * @brief Single-control-thread fake runtime and headless session host with explicit owned lifetime.
     *
     * Owns the resource port, production session gate, production frame gate and copied script. Methods cannot race
     * or re-enter. Activation and script replacement are tooling/lifecycle work; Step uses bounded fixed storage,
     * performs no allocation on success, I/O, native access, waits, or GPU synchronization. Simulated cleanup is
     * immediate because this owner has no external leases. Evidence never qualifies a physical runtime/backend.
     */
    class XRFakeRuntime final {
    public:
        /** @brief Constructs an inactive harness with no ambient registration. */
        XRFakeRuntime() noexcept;
        /** @brief Closes frame admission before retiring session resources. */
        ~XRFakeRuntime();
        XRFakeRuntime(const XRFakeRuntime &) = delete;
        XRFakeRuntime &operator=(const XRFakeRuntime &) = delete;
        /**
         * @brief Activate or replace using the same public session and feature contracts as production.
         * @param capabilities Exact runtime/system evidence.
         * @param plan Accepted immutable feature plan.
         * @param limits Frame bounds, checked before session publication.
         * @param failStage Optional candidate preparation failure, rolled back by the production gate.
         * @return Exact new owner or typed error; failure preserves the prior session and script.
         */
        [[nodiscard]] Result<XRSessionId> Activate(const XRCapabilitySnapshot &capabilities, const XRFeaturePlan &plan,
                                                   XRFrameLimits limits, std::optional<XRSessionPreparation> failStage = {});
        /**
         * @brief Atomically copy a bounded exact-owner script and reset its cursor.
         * @param session Current owner; scripts cannot be reused across replacement.
         * @param steps Finite time, view, action, event and failure evidence; no span is retained.
         * @return Typed invalid, stale, unsupported, capacity or shutdown status without mutation on rejection.
         * @throws std::bad_alloc If bounded script copying fails; the old script remains intact.
         */
        [[nodiscard]] XRFrameStatus LoadScript(const XRSessionId &session, std::span<const XRFakeStep> steps);
        /**
         * @brief Consume one step through production session/frame gates and publish owned evidence.
         * @param session Exact expected owner, fenced before consuming any step.
         * @return Outcome with explicit consumption, current state and frame evidence; exhausted scripts are Unavailable.
         */
        [[nodiscard]] XRFakeOutcome Step(const XRSessionId &session);
        /** @brief Read current session state. @return Exact generation-fenced production publication. */
        [[nodiscard]] XRSessionSnapshot Snapshot() const noexcept;
        /** @brief Permanently shut down frame and session gates and discard scripts; idempotent. */
        void Shutdown() noexcept;

    private:
        /** @brief Immediate fake resource port; lifetime encloses both production gates. */
        class Resources final : public IXRSessionResources {
        public:
            std::optional<XRSessionPreparation> failStage;
            [[nodiscard]] Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &, const XRFeaturePlan &) override;
            void Release(XRSessionPreparation, const XRSessionId &) noexcept override;
        };

        /** @brief Validate one copied step without changing published evidence. */
        [[nodiscard]] XRFrameStatus ValidateStep(const XRFakeStep &step, const XRSessionId &session) const;
        /** @brief Validate one step's exact-owner predicted view evidence. */
        [[nodiscard]] XRFrameStatus ValidateViews(const XRFakeStep &step, const XRSessionId &session) const noexcept;
        /** @brief Validate bounded unique action values against the accepted plan. */
        [[nodiscard]] XRFrameStatus ValidateActions(const XRFakeStep &step, const XRSessionId &session) const noexcept;
        /** @brief Simulate rendering operations, retaining and draining exact acquired-image obligations on failure. */
        [[nodiscard]] XRFrameStatus RenderFrame(const XRFakeStep &step, const XRFrameId &frame, XRFakeOutcome &outcome) noexcept;
        /** @brief Run the bounded simulated frame and close all obligations before returning. */
        [[nodiscard]] XRFrameStatus RunFrame(const XRFakeStep &step, const XRSessionId &session, XRFakeOutcome &outcome) noexcept;
        /** @brief Copy valid frame evidence and neutralize unfocused or tracking-lost actions without allocation. */
        static void PublishEvidence(const XRFakeStep &step, XRFakeOutcome &outcome) noexcept;
        Resources resources_;
        XRSessionLifecycle sessions_;
        XRFrameLifecycle frames_;
        XRFrameLimits limits_{};
        std::optional<XRFeaturePlan> plan_;
        std::vector<XRFakeStep> script_;
        std::size_t cursor_{};
        bool shutdown_{};
    };
}  // namespace Horo::XR
