#pragma once

/** @file ImmersiveAuthoringContext.h @brief Editor-owned immersive planning generations. */

#include "AgentTurn.h"
#include "Horo/Scene/SceneSourceModel.h"
#include "Horo/XR/XRIdentity.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::Editor {
    enum class ImmersiveAuthoringMode : std::uint8_t {
        Edit,
        Simulate,
        Play
    };
    enum class ImmersiveRequestKind : std::uint8_t {
        Plan,
        Diagnostics,
        SpeechCapture
    };
    enum class ImmersiveAdmission : std::uint8_t {
        Admitted,
        StaleContext,
        ModeDenied,
        PermissionDenied,
        InvalidRequest
    };

    /** @brief Values captured from the authenticated editor, document and XR owners. */
    struct ImmersiveContextIdentity final {
        std::uint64_t projectOpening{};
        std::uint64_t documentOpening{};
        std::uint64_t documentRevision{};
        XR::XRSessionId xrSession;
        XR::XRSpaceId referenceSpace;
        std::uint64_t user{};
        std::uint64_t conversation{};
        std::uint64_t toolContext{};
        XR::XRCapabilityRevision capabilities;
        ImmersiveAuthoringMode mode{};
        std::uint64_t worldGeneration{}; /**< Required for a disposable simulation or Play world. */
        bool planningAllowed{};
        bool diagnosticsAllowed{};
        bool localSpeechOnly{}; /**< Local Play diagnostics. No model, conversation or tool authority. */
        bool operator==(const ImmersiveContextIdentity &) const noexcept = default;
    };

    struct ImmersiveContextEpoch;

    /** @brief Immutable bounded capture. The epoch is only a revocation signal and contains no owner reference. */
    class ImmersivePlanningContext final {
    public:
        static constexpr std::size_t MaximumSelection = 32;

        [[nodiscard]] const ImmersiveContextIdentity &Identity() const & noexcept {
            return identity_;
        }

        [[nodiscard]] const ImmersiveContextIdentity &Identity() const && = delete;

        [[nodiscard]] std::uint64_t Generation() const noexcept {
            return generation_;
        }

        [[nodiscard]] std::span<const SceneSource::SceneObjectId> Selection() const & noexcept {
            return std::span{selection_}.first(selectionCount_);
        }

        [[nodiscard]] std::span<const SceneSource::SceneObjectId> Selection() const && = delete;
        [[nodiscard]] bool IsCurrent() const noexcept;

        /** @brief Checks whether two snapshots were issued for the exact same owner and generation. */
        [[nodiscard]] bool SharesGeneration(const ImmersivePlanningContext &other) const noexcept {
            return epoch_ && epoch_ == other.epoch_ && generation_ == other.generation_;
        }

    private:
        friend class ImmersiveAuthoringContext;
        ImmersiveContextIdentity identity_;
        std::array<SceneSource::SceneObjectId, MaximumSelection> selection_{};
        std::size_t selectionCount_{};
        std::uint64_t generation_{};
        std::shared_ptr<const ImmersiveContextEpoch> epoch_;
    };

    /** @brief One inert plan retained with its revocable authoring scope until the editor adopts it. */
    class ImmersivePlanReview final {
    public:
        /** @brief Reports terminal status, including cancellation or stale scope after inference. */
        [[nodiscard]] Agent::AgentTurnStatus Status() const noexcept;

        /** @brief Borrows the validated draft and effects for the exact current authoring generation.
         * @param current Editor-owned context at presentation time.
         * @return Plan while current and complete, or null after withdrawal or failure.
         */
        [[nodiscard]] const Agent::AgentTurnPlan *Read(const ImmersivePlanningContext &current) const & noexcept;
        [[nodiscard]] const Agent::AgentTurnPlan *Read(const ImmersivePlanningContext &current) const && = delete;

    private:
        friend class ImmersiveAgentRequest;
        ImmersivePlanReview(ImmersivePlanningContext context, Agent::AgentTurnPlan plan, std::stop_token stop);

        ImmersivePlanningContext context_;
        Agent::AgentTurnPlan plan_;
        std::stop_token stop_;
    };

    /** @brief An owner-admitted inference request that can move to a worker without borrowing editor state. */
    class ImmersiveAgentRequest final {
    public:
        ImmersiveAgentRequest(ImmersiveAgentRequest &&) noexcept = default;
        ImmersiveAgentRequest &operator=(ImmersiveAgentRequest &&) noexcept = default;

        /** @brief The exact generation used to admit and dispatch the request. */
        [[nodiscard]] std::uint64_t Generation() const noexcept {
            return context_.Generation();
        }

        /** @brief Checks whether this request can still publish on the editor lane after worker completion. */
        [[nodiscard]] bool IsCurrent() const noexcept {
            return context_.IsCurrent();
        }

        /** @brief Runs bounded text inference with the existing Agent runtime and checks revocation during dispatch.
         * @param provider Host-selected provider with separately admitted transport and payload.
         * @param turnId Nonzero turn identity assigned by the host.
         * @param stop Worker cancellation.
         * @return Terminal outcome. The host must recheck IsCurrent() when adopting the result.
         */
        [[nodiscard]] Agent::AgentTurnResult Run(Agent::IModelProvider &provider, std::uint64_t turnId, std::stop_token stop = {});

        /** @brief Runs an explicitly captured plan for review without any tool execution port.
         * @param provider Host-selected provider. @param turnId Nonzero host-issued identity.
         * @param limits Host draft budget. @param validateTool Host registry and policy check.
         * @param stop Worker cancellation. @return Inert draft or terminal rejection.
         */
        [[nodiscard]] ImmersivePlanReview Plan(Agent::IModelProvider &provider, std::uint64_t turnId, const Agent::AgentTurnLimits &limits,
                                               const Agent::AgentPlanToolValidator &validateTool, std::stop_token stop = {});

    private:
        friend class ImmersiveAuthoringContext;
        ImmersiveAgentRequest(ImmersivePlanningContext context, Agent::ModelRequest request, bool reviewOnly = false);

        ImmersivePlanningContext context_;
        Agent::ModelRequest request_;
        bool reviewOnly_{};
    };

    /** @brief Cold owner-lane publication and admission for one Editor opening. */
    class ImmersiveAuthoringContext final {
    public:
        ImmersiveAuthoringContext();
        ~ImmersiveAuthoringContext();
        ImmersiveAuthoringContext(const ImmersiveAuthoringContext &) = delete;
        ImmersiveAuthoringContext &operator=(const ImmersiveAuthoringContext &) = delete;

        /** @brief Publishes the current owner observation. Invalid observations revoke the last generation. */
        [[nodiscard]] std::optional<ImmersivePlanningContext> Publish(const ImmersiveContextIdentity &identity,
                                                                      std::span<const SceneSource::SceneObjectId> selection) noexcept;
        /** @brief Revokes all previously issued contexts before project, document or XR teardown. */
        void Revoke() noexcept;
        /** @brief Rechecks one captured request before any tool authority is consulted.
         * Admitted planning is non-authoritative, including in Simulate. It grants no mutation or approval.
         */
        [[nodiscard]] ImmersiveAdmission Admit(const ImmersivePlanningContext &context, ImmersiveRequestKind kind) const noexcept;

        /** @brief Binds a bounded text-only model request to the current admitted context before queueing.
         * @param context Snapshot of the owner observation to use.
         * @param kind Explicit planning or diagnostics intent from the host.
         * @param request Provider payload without tools or tool results. Moved only on successful capture.
         * @return Worker request, or empty for a stale, unadmitted or tool-bearing request.
         */
        [[nodiscard]] std::optional<ImmersiveAgentRequest> CaptureRequest(const ImmersivePlanningContext &context,
                                                                          ImmersiveRequestKind kind, Agent::ModelRequest &&request) const;
        /** @brief Captures a bounded planning request with host-selected tools for inert review only.
         * @param context Exact current Edit or Simulate context. @param request Host-authored model request without tool history.
         * @return One owned request or empty if mode, context, policy or payload cannot be admitted.
         */
        [[nodiscard]] std::optional<ImmersiveAgentRequest> CapturePlan(const ImmersivePlanningContext &context,
                                                                       Agent::ModelRequest &&request) const;

    private:
        std::shared_ptr<ImmersiveContextEpoch> epoch_;
        std::optional<ImmersivePlanningContext> current_;
        std::uint64_t generation_{};
    };
}  // namespace Horo::Editor
