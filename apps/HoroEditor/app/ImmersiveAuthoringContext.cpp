#include "ImmersiveAuthoringContext.h"

#include "AgentTurnMcp.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <utility>

namespace Horo::Editor {
    struct ImmersiveContextEpoch final {
        std::atomic<std::uint64_t> current{};
    };

    namespace {
        /** @brief Proposed writes require a scope, and every scoped step must name the current planning document. */
        bool MatchesPlanScene(const Agent::AgentPlanToolInspection &inspection, const ImmersiveContextIdentity &scene) noexcept {
            if (!inspection.tool)
                return true;
            if (!inspection.tool->scope)
                return inspection.tool->effect != Agent::AgentPlannedEffect::Mutation;
            const auto &scope = *inspection.tool->scope;
            return scope.opening == scene.documentOpening && scope.revision == scene.documentRevision;
        }

        /** @brief Capture-only Play admits no user, conversation or tool dispatch credentials. */
        bool ValidAuthority(const ImmersiveContextIdentity &identity) noexcept {
            if (identity.localSpeechOnly)
                return identity.mode == ImmersiveAuthoringMode::Play && identity.diagnosticsAllowed && !identity.planningAllowed &&
                       !identity.user && !identity.conversation && !identity.toolContext;
            return identity.user && identity.conversation && identity.toolContext;
        }

        /** @brief Verifies that the mode, optional world and spatial scope come from the same owner. */
        bool ValidXrScope(const ImmersiveContextIdentity &identity) noexcept {
            using enum ImmersiveAuthoringMode;
            const bool validMode = identity.mode == Edit || identity.mode == Simulate || identity.mode == Play;
            const bool validWorld = (identity.worldGeneration == 0) == (identity.mode == Edit);
            return validMode && validWorld && identity.capabilities.IsValid() && identity.xrSession.IsValid() &&
                   identity.referenceSpace.IsValid() && identity.referenceSpace.session == identity.xrSession;
        }

        /** @brief Admits only a complete document observation and the matching authority. */
        bool ValidIdentity(const ImmersiveContextIdentity &identity) noexcept {
            return identity.projectOpening && identity.documentOpening && identity.documentRevision && ValidAuthority(identity) &&
                   ValidXrScope(identity);
        }

        /** @brief Play can consume speech locally. Other modes use the planning admission. */
        ImmersiveAdmission AdmitPlanningOrSpeech(const ImmersiveContextIdentity &identity, const ImmersiveRequestKind kind) noexcept {
            using enum ImmersiveAdmission;
            if (identity.mode != ImmersiveAuthoringMode::Play)
                return identity.planningAllowed ? Admitted : PermissionDenied;
            return kind == ImmersiveRequestKind::SpeechCapture && identity.localSpeechOnly ? Admitted : ModeDenied;
        }

        /** @brief Verifies selection without allocations or reordering the owner projection. */
        bool ValidSelection(const std::span<const SceneSource::SceneObjectId> selection) noexcept {
            if (selection.size() > ImmersivePlanningContext::MaximumSelection)
                return false;
            for (std::size_t index = 0; index < selection.size(); ++index) {
                const auto previous = selection.first(index);
                if (!selection[index].IsValid() || std::ranges::find(previous, selection[index]) != previous.end())
                    return false;
            }
            return true;
        }

        /** @brief Rejects tool-bearing history and malformed payloads before transferring them to the queue. */
        bool ValidFreshRequest(const Agent::ModelRequest &request, const bool withTools) {
            using Agent::ModelFeature;
            constexpr Agent::ModelFeatures textFeatures{static_cast<std::uint32_t>(ModelFeature::Streaming) |
                                                        static_cast<std::uint32_t>(ModelFeature::Usage)};
            const Agent::ModelFeatures features{textFeatures.bits | (withTools ? static_cast<std::uint32_t>(ModelFeature::Tools) : 0U)};
            return Agent::AdmitModelRequest(request, features).Succeeded() &&
                   std::ranges::none_of(request.messages, [](const Agent::ModelMessage &message) {
                return message.role == Agent::ModelRole::Tool || !message.toolIntents.empty();
            });
        }
    }  // namespace

    bool ImmersivePlanningContext::IsCurrent() const noexcept {
        return epoch_ && generation_ != 0 && epoch_->current.load(std::memory_order_acquire) == generation_ &&
               !sourceLifetime_.IsCancellationRequested();
    }

    ImmersiveAgentRequest::ImmersiveAgentRequest(ImmersivePlanningContext context, Agent::ModelRequest request, const bool reviewOnly,
                                                 std::string projectIdentity)
        : context_(std::move(context)), request_(std::move(request)), reviewOnly_(reviewOnly),
          projectIdentity_(std::move(projectIdentity)) {}

    /** @copydoc ImmersiveAgentRequest::IsCurrent */
    bool ImmersiveAgentRequest::IsCurrent() const noexcept {
        return context_.IsCurrent() && (!catalog_ || catalog_->IsCurrent());
    }

    /** @copydoc ImmersiveAgentRequest::Run */
    Agent::AgentTurnResult ImmersiveAgentRequest::Run(Agent::IModelProvider &provider, std::uint64_t turnId, std::stop_token stop) {
        if (reviewOnly_)
            return {.status = Agent::AgentTurnStatus::InvalidRequest};
        const Agent::AgentTurnFreshness authority = [context = context_]() noexcept { return context.IsCurrent(); };
        Agent::AgentTurnRuntime runtime{authority};
        return runtime.RunScopedText(provider, turnId, std::exchange(request_, {}), {}, stop, authority);
    }

    ImmersivePlanReview::ImmersivePlanReview(ImmersivePlanningContext context, Agent::AgentTurnPlan plan, const std::stop_token stop,
                                             CancellationToken projectLifetime, std::shared_ptr<const Agent::McpPlanCatalog> catalog)
        : context_(std::move(context)), plan_(std::move(plan)), stop_(stop), projectLifetime_(std::move(projectLifetime)),
          catalog_(std::move(catalog)) {}

    /** @copydoc ImmersivePlanReview::HasCurrentAuthority */
    bool ImmersivePlanReview::HasCurrentAuthority() const noexcept {
        return context_.IsCurrent() && !projectLifetime_.IsCancellationRequested() && (!catalog_ || catalog_->IsCurrent());
    }

    /** @copydoc ImmersivePlanReview::Status */
    Agent::AgentTurnStatus ImmersivePlanReview::Status() const noexcept {
        using enum Agent::AgentTurnStatus;
        if (plan_.status != Completed)
            return plan_.status;
        if (stop_.stop_requested())
            return Cancelled;
        return HasCurrentAuthority() ? Completed : ContextDenied;
    }

    /** @copydoc ImmersivePlanReview::ProviderFailure */
    std::optional<Agent::ModelErrorCode> ImmersivePlanReview::ProviderFailure(const ImmersivePlanningContext &current) const noexcept {
        if (plan_.status != Agent::AgentTurnStatus::ProviderFailed || !context_.SharesGeneration(current) || stop_.stop_requested() ||
            !HasCurrentAuthority())
            return std::nullopt;
        return plan_.providerError;
    }

    /** @copydoc ImmersivePlanReview::Read */
    const Agent::AgentTurnPlan *ImmersivePlanReview::Read(const ImmersivePlanningContext &current,
                                                          const TargetValidator &inspectTargets) const & noexcept {
        using enum Agent::AgentTurnStatus;
        if (!context_.SharesGeneration(current) || Status() != Completed)
            return nullptr;
        try {
            for (const auto &tool : plan_.tools)
                if (tool.scope && (!inspectTargets || !inspectTargets(*tool.scope) || Status() != Completed))
                    return nullptr;
            return Status() == Completed ? &plan_ : nullptr;
        } catch (...) {
            return nullptr;
        }
    }

    /** @copydoc ImmersiveAgentRequest::Plan */
    ImmersivePlanReview ImmersiveAgentRequest::Plan(Agent::IModelProvider &provider, const std::uint64_t turnId,
                                                    const Agent::AgentTurnLimits &limits, const Agent::AgentPlanToolValidator &validateTool,
                                                    const std::stop_token stop, const Agent::AgentTurnContext *approvedContext) {
        const bool hasExactlyOneInspector = static_cast<bool>(catalog_) != static_cast<bool>(validateTool);
        if (!reviewOnly_ || !hasExactlyOneInspector)
            return {context_, {.status = Agent::AgentTurnStatus::InvalidRequest}, stop};
        if (!approvedContext && !projectIdentity_.empty())
            return {context_, {.status = Agent::AgentTurnStatus::ContextDenied}, stop};
        if (approvedContext && (projectIdentity_.empty() || approvedContext->project.projectIdentity != projectIdentity_ ||
                                approvedContext->project.projectGeneration != context_.Identity().projectOpening))
            return {context_,
                    {.status = Agent::AgentTurnStatus::ContextDenied, .contextError = Security::AgentPolicyReason::StaleAuthority},
                    stop};
        Agent::AgentTurnRuntime runtime{[this] {
            return IsCurrent();
        }};
        const Agent::AgentPlanToolValidator inspect = [&](const Agent::ModelToolIntent &intent) {
            auto inspected = catalog_ ? catalog_->Inspect(intent) : validateTool(intent);
            if (!MatchesPlanScene(inspected, context_.Identity()))
                return Agent::AgentPlanToolInspection{Agent::AgentTurnStatus::ContextDenied};
            return inspected;
        };
        const auto projectLifetime = approvedContext ? approvedContext->project.cancellation : CancellationToken{};
        const Agent::AgentTurnFreshness providerAuthority = [context = context_, catalog = catalog_, projectLifetime]() noexcept {
            return context.IsCurrent() && !projectLifetime.IsCancellationRequested() && (!catalog || catalog->IsCurrent());
        };
        auto plan = runtime.Plan(provider, turnId, std::exchange(request_, {}), limits, inspect, stop, approvedContext, providerAuthority);
        return {context_, std::move(plan), stop, projectLifetime, catalog_};
    }

    ImmersiveAuthoringContext::ImmersiveAuthoringContext() : epoch_(std::make_shared<ImmersiveContextEpoch>()) {}

    ImmersiveAuthoringContext::~ImmersiveAuthoringContext() {
        Revoke();
    }

    /** @copydoc ImmersiveAuthoringContext::Publish */
    std::optional<ImmersivePlanningContext> ImmersiveAuthoringContext::Publish(const ImmersiveContextIdentity &identity,
                                                                               const std::span<const SceneSource::SceneObjectId> selection,
                                                                               const CancellationToken &sourceLifetime) noexcept {
        if (sourceLifetime.IsCancellationRequested() || !ValidIdentity(identity) || !ValidSelection(selection)) {
            Revoke();
            return std::nullopt;
        }
        if (current_ && identity == current_->Identity() && current_->sourceLifetime_ == sourceLifetime &&
            std::ranges::equal(selection, current_->Selection()))
            return current_;
        Revoke();
        if (generation_ == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        auto &context = current_.emplace();
        context.identity_ = identity;
        std::ranges::copy(selection, context.selection_.begin());
        context.selectionCount_ = selection.size();
        context.generation_ = ++generation_;
        context.epoch_ = epoch_;
        context.sourceLifetime_ = sourceLifetime;
        epoch_->current.store(context.generation_, std::memory_order_release);
        return current_;
    }

    void ImmersiveAuthoringContext::Revoke() noexcept {
        epoch_->current.store(0, std::memory_order_release);
        current_.reset();
    }

    ImmersiveAdmission ImmersiveAuthoringContext::Admit(const ImmersivePlanningContext &context,
                                                        const ImmersiveRequestKind kind) const noexcept {
        if (context.epoch_ != epoch_ || !context.IsCurrent())
            return ImmersiveAdmission::StaleContext;
        const auto &identity = context.Identity();
        using enum ImmersiveAdmission;
        using enum ImmersiveRequestKind;
        if (kind == Diagnostics)
            return identity.diagnosticsAllowed ? Admitted : PermissionDenied;
        if (kind != Plan && kind != SpeechCapture)
            return InvalidRequest;
        return AdmitPlanningOrSpeech(identity, kind);
    }

    /** @copydoc ImmersiveAuthoringContext::CaptureRequest */
    std::optional<ImmersiveAgentRequest> ImmersiveAuthoringContext::CaptureRequest(const ImmersivePlanningContext &context,
                                                                                   const ImmersiveRequestKind kind,
                                                                                   Agent::ModelRequest &&request) const {
        if (context.Identity().localSpeechOnly || kind == ImmersiveRequestKind::SpeechCapture)
            return std::nullopt;
        if (Admit(context, kind) != ImmersiveAdmission::Admitted || !ValidFreshRequest(request, false))
            return std::nullopt;
        return ImmersiveAgentRequest{context, std::move(request)};
    }

    /** @copydoc ImmersiveAuthoringContext::CapturePlan */
    std::optional<ImmersiveAgentRequest> ImmersiveAuthoringContext::CapturePlan(const ImmersivePlanningContext &context,
                                                                                Agent::ModelRequest &&request,
                                                                                const std::string_view projectIdentity) const {
        if (Admit(context, ImmersiveRequestKind::Plan) != ImmersiveAdmission::Admitted || !ValidFreshRequest(request, true))
            return std::nullopt;
        return ImmersiveAgentRequest{context, std::move(request), true, std::string{projectIdentity}};
    }

    /** @copydoc ImmersiveAuthoringContext::CaptureMcpPlan */
    std::optional<ImmersiveAgentRequest> ImmersiveAuthoringContext::CaptureMcpPlan(const ImmersivePlanningContext &context,
                                                                                   Agent::ModelRequest request,
                                                                                   std::shared_ptr<const Agent::McpPlanCatalog> catalog,
                                                                                   const std::string_view projectIdentity) const {
        if (!catalog || !catalog->IsCurrent() || !request.tools.empty())
            return std::nullopt;
        request.tools = catalog->Tools();
        request.requiredFeatures.bits |= static_cast<std::uint32_t>(Agent::ModelFeature::Tools);
        auto captured = CapturePlan(context, std::move(request), projectIdentity);
        if (captured)
            captured->catalog_ = std::move(catalog);
        return captured;
    }
}  // namespace Horo::Editor
