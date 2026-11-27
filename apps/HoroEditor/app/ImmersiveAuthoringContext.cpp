#include "ImmersiveAuthoringContext.h"

#include "AgentTurnMcp.h"
#include "EditorCommandMcpSession.h"
#include "EditorInspectionMcpTools.h"
#include "Horo/Mcp/McpErrors.h"

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

        /** @brief Recognizes a single bounded Scene proposal produced by registered tool inspection. */
        bool HasSceneProposal(const Agent::AgentTurnPlan &plan) noexcept {
            return plan.draft && plan.draft->toolIntents.size() == 1 && plan.tools.size() == 1 && plan.tools.front().scope &&
                   plan.tools.front().effect == Agent::AgentPlannedEffect::Presentation;
        }

        /** @brief Checks targets on the same workspace lane that can stage a viewport review. */
        bool InspectEditScope(const EditorInspectionCommandEndpoint &scene, const Agent::AgentPlanObjectScope &scope) noexcept {
            try {
                return scene.inspectEditTargets({scope.opening, scope.revision}, scope.objects).HasValue();
            } catch (...) {
                return false;
            }
        }

        /** @brief Capture-only Play admits no user, conversation or tool dispatch credentials. */
        bool ValidAuthority(const ImmersiveContextIdentity &identity) noexcept {
            const auto &command = identity.commandSession;
            if (command != WorkspaceMcpSessionKey{} && (!command.principal || !command.session || !command.generation))
                return false;
            if (identity.localSpeechOnly)
                return identity.mode == ImmersiveAuthoringMode::Play && identity.diagnosticsAllowed && !identity.planningAllowed &&
                       !identity.user && !identity.conversation && !identity.toolContext && !command.principal;
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
                                             Agent::AgentTurnFreshness authority,
                                             std::weak_ptr<const EditorInspectionCommandEndpoint> scene)
        : context_(std::move(context)), plan_(std::move(plan)), stop_(stop), authority_(std::move(authority)), scene_(std::move(scene)) {}

    /** @copydoc ImmersivePlanReview::HasCurrentAuthority */
    bool ImmersivePlanReview::HasCurrentAuthority() const noexcept {
        return context_.IsCurrent() && authority_ && authority_();
    }

    /** @copydoc ImmersivePlanReview::StageScenePreview */
    Result<std::uint64_t> ImmersivePlanReview::StageScenePreview(const ImmersivePlanningContext &current) const {
        using Output = Result<std::uint64_t>;
        if (current.Identity().mode != ImmersiveAuthoringMode::Edit)
            return Output::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
        const auto scene = scene_.lock();
        if (!scene || scene->owner != std::this_thread::get_id() || !scene->inspectEditTargets)
            return Output::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
        const auto *plan = Read(current, [&scene](const Agent::AgentPlanObjectScope &scope) {
            return InspectEditScope(*scene, scope);
        });
        if (!plan || !HasSceneProposal(*plan))
            return Output::Failure(MakeError(Mcp::McpErrors::ToolUnavailable));
        const auto &intent = plan->draft->toolIntents.front();
        auto authorized = [context = context_, authority = authority_, stop = stop_] {
            return !stop.stop_requested() && context.IsCurrent() && authority();
        };
        return StageEditorScenePlan(*scene, intent.name, nlohmann::json::parse(intent.argumentsJson, nullptr, false),
                                    std::move(authorized));
    }

    ImmersiveSceneCandidate::ImmersiveSceneCandidate(ImmersivePlanningContext source, Agent::ModelToolIntent intent,
                                                     Agent::AgentPlanObjectScope scope, Agent::AgentTurnFreshness sourceAuthority,
                                                     std::weak_ptr<const EditorInspectionCommandEndpoint> scene)
        : source_(std::move(source)), intent_(std::move(intent)), scope_(std::move(scope)), sourceAuthority_(std::move(sourceAuthority)),
          scene_(std::move(scene)) {}

    /** @copydoc ImmersivePlanReview::RetainSceneCandidate */
    std::optional<ImmersiveSceneCandidate> ImmersivePlanReview::RetainSceneCandidate(const ImmersivePlanningContext &current) const {
        if (current.Identity().mode != ImmersiveAuthoringMode::Simulate || !context_.SharesGeneration(current) ||
            Status() != Agent::AgentTurnStatus::Completed || scene_.expired() || !HasSceneProposal(plan_))
            return std::nullopt;
        auto sourceAuthority = [authority = authority_, stop = stop_] {
            return !stop.stop_requested() && authority();
        };
        return ImmersiveSceneCandidate{context_, plan_.draft->toolIntents.front(), *plan_.tools.front().scope, std::move(sourceAuthority),
                                       scene_};
    }

    /** @copydoc ImmersivePlanReview::Status */
    Agent::AgentTurnStatus ImmersivePlanReview::Status() const noexcept {
        using enum Agent::AgentTurnStatus;
        if (plan_.status != Completed && plan_.status != ProviderFailed)
            return plan_.status;
        if (stop_.stop_requested())
            return Cancelled;
        return HasCurrentAuthority() ? plan_.status : ContextDenied;
    }

    /** @copydoc ImmersivePlanReview::ProviderFailure */
    std::optional<ImmersivePlanReview::ProviderFailureInfo> ImmersivePlanReview::ProviderFailure(
        const ImmersivePlanningContext &current) const noexcept {
        if (!context_.SharesGeneration(current) || Status() != Agent::AgentTurnStatus::ProviderFailed || !plan_.providerError)
            return std::nullopt;
        const auto code = *plan_.providerError;
        return ProviderFailureInfo{code, code == Agent::ModelErrorCode::RateLimited ? plan_.retryAfter : std::nullopt};
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
        const auto trustLifetime = approvedContext ? approvedContext->policy.ObserveProjectRevision() : std::optional{CancellationToken{}};
        if (approvedContext && (!trustLifetime || projectIdentity_.empty() || approvedContext->project.projectIdentity != projectIdentity_ ||
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
        const auto projectDeadline = approvedContext ? approvedContext->project.deadline : std::chrono::steady_clock::time_point::max();
        auto projectAuthority = [catalog = catalog_, projectLifetime, trust = *trustLifetime]() noexcept {
            return !projectLifetime.IsCancellationRequested() && !trust.IsCancellationRequested() && (!catalog || catalog->IsCurrent());
        };
        const Agent::AgentTurnFreshness providerAuthority = [context = context_, projectAuthority]() noexcept {
            return context.IsCurrent() && projectAuthority();
        };
        auto plan = runtime.Plan(provider, turnId, std::exchange(request_, {}), limits, inspect, stop, approvedContext, providerAuthority);
        auto reviewAuthority = [projectAuthority = std::move(projectAuthority), projectDeadline]() noexcept {
            return std::chrono::steady_clock::now() < projectDeadline && projectAuthority();
        };
        return {context_, std::move(plan), stop, std::move(reviewAuthority), scene_};
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
        return PublishValidated(identity, selection, sourceLifetime);
    }

    /** @copydoc ImmersiveAuthoringContext::PublishValidated */
    std::optional<ImmersivePlanningContext> ImmersiveAuthoringContext::PublishValidated(
        const ImmersiveContextIdentity &identity, const std::span<const SceneSource::SceneObjectId> selection,
        const CancellationToken &sourceLifetime) noexcept {
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
        editTransferSource_ = 0;
    }

    /** @copydoc ImmersiveAuthoringContext::TransitionToEdit */
    std::optional<ImmersivePlanningContext> ImmersiveAuthoringContext::TransitionToEdit(
        const ImmersivePlanningContext &source, const ImmersiveContextIdentity &identity,
        const std::span<const SceneSource::SceneObjectId> selection, const CancellationToken &sourceLifetime) noexcept {
        if (Admit(source, ImmersiveRequestKind::Plan) != ImmersiveAdmission::Admitted ||
            source.Identity().mode != ImmersiveAuthoringMode::Simulate || !source.Identity().commandSession.principal)
            return std::nullopt;
        auto expected = source.Identity();
        expected.mode = ImmersiveAuthoringMode::Edit;
        expected.worldGeneration = 0;
        if (identity != expected || sourceLifetime.IsCancellationRequested() || !ValidSelection(selection) ||
            generation_ == std::numeric_limits<std::uint64_t>::max())
            return std::nullopt;
        const auto sourceGeneration = source.Generation();
        auto edit = PublishValidated(identity, selection, sourceLifetime);
        if (edit)
            editTransferSource_ = sourceGeneration;
        return edit;
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
        if (context.Identity().commandSession.principal)
            return std::nullopt;
        return CapturePlanRequest(context, std::move(request), projectIdentity);
    }

    /** @brief Admits a fresh plan payload through the owning editor context. */
    std::optional<ImmersiveAgentRequest> ImmersiveAuthoringContext::CapturePlanRequest(const ImmersivePlanningContext &context,
                                                                                       Agent::ModelRequest &&request,
                                                                                       const std::string_view projectIdentity) const {
        if (Admit(context, ImmersiveRequestKind::Plan) != ImmersiveAdmission::Admitted || !ValidFreshRequest(request, true))
            return std::nullopt;
        return ImmersiveAgentRequest{context, std::move(request), true, std::string{projectIdentity}};
    }

    ImmersiveMcpCatalog::ImmersiveMcpCatalog(ImmersivePlanningContext context, std::shared_ptr<const Agent::McpPlanCatalog> catalog,
                                             std::string project, std::weak_ptr<const EditorInspectionCommandEndpoint> scene)
        : context_(std::move(context)), catalog_(std::move(catalog)), project_(std::move(project)), scene_(std::move(scene)) {}

    /** @copydoc ImmersiveAuthoringContext::CaptureMcpCatalog */
    std::optional<ImmersiveMcpCatalog> ImmersiveAuthoringContext::CaptureMcpCatalog(
        const ImmersivePlanningContext &context, std::shared_ptr<const Mcp::McpToolRegistry> registry,
        const std::span<const std::string> capabilities, const std::span<const std::string> approvedTools,
        const CancellationToken &approvalLifetime) const {
        if (context.Identity().commandSession.principal || Admit(context, ImmersiveRequestKind::Plan) != ImmersiveAdmission::Admitted)
            return std::nullopt;
        auto catalog = Agent::McpPlanCatalog::Capture(std::move(registry), capabilities, approvedTools, approvalLifetime);
        if (!catalog || !context.IsCurrent())
            return std::nullopt;
        return ImmersiveMcpCatalog{context, std::move(catalog)};
    }

    /** @copydoc ImmersiveAuthoringContext::CaptureMcpCatalog */
    std::optional<ImmersiveMcpCatalog> ImmersiveAuthoringContext::CaptureMcpCatalog(const ImmersivePlanningContext &context,
                                                                                    const EditorCommandMcpSession &session,
                                                                                    const std::string_view project,
                                                                                    const std::span<const std::string> approvedTools,
                                                                                    const CancellationToken &approvalLifetime) const {
        const auto &key = context.Identity().commandSession;
        if (project.empty() || !key.principal || key != session.SessionKey() ||
            Admit(context, ImmersiveRequestKind::Plan) != ImmersiveAdmission::Admitted)
            return std::nullopt;
        auto source = session.CapturePlanningSource();
        if (!source || source->project != project)
            return std::nullopt;
        auto catalog = Agent::McpPlanCatalog::CaptureSession(std::move(source->tools), source->grants, approvedTools, approvalLifetime,
                                                             std::move(source->isCurrent));
        if (!catalog || !context.IsCurrent())
            return std::nullopt;
        return ImmersiveMcpCatalog{context, std::move(catalog), std::move(source->project), std::move(source->scene)};
    }

    /** @copydoc ImmersiveAuthoringContext::CaptureMcpPlan */
    std::optional<ImmersiveAgentRequest> ImmersiveAuthoringContext::CaptureMcpPlan(const ImmersivePlanningContext &context,
                                                                                   Agent::ModelRequest request,
                                                                                   const ImmersiveMcpCatalog &catalog,
                                                                                   const std::string_view projectIdentity) const {
        if (!catalog.context_.SharesGeneration(context) || !context.IsCurrent() || !catalog.catalog_ || !catalog.catalog_->IsCurrent() ||
            !request.tools.empty())
            return std::nullopt;
        if (!catalog.project_.empty() && !projectIdentity.empty() && catalog.project_ != projectIdentity)
            return std::nullopt;
        request.tools = catalog.catalog_->Tools();
        request.requiredFeatures.bits |= static_cast<std::uint32_t>(Agent::ModelFeature::Tools);
        auto captured = CapturePlanRequest(context, std::move(request), catalog.project_.empty() ? projectIdentity : catalog.project_);
        if (captured) {
            captured->catalog_ = catalog.catalog_;
            captured->scene_ = catalog.scene_;
        }
        return captured;
    }

    /** @copydoc ImmersiveAuthoringContext::StageSceneCandidate */
    Result<std::uint64_t> ImmersiveAuthoringContext::StageSceneCandidate(const ImmersivePlanningContext &current,
                                                                         const ImmersiveMcpCatalog &catalog,
                                                                         const ImmersiveSceneCandidate &candidate) const {
        using Output = Result<std::uint64_t>;
        if (Admit(current, ImmersiveRequestKind::Plan) != ImmersiveAdmission::Admitted ||
            candidate.source_.epoch_ != epoch_ || candidate.source_.Generation() != editTransferSource_ ||
            !catalog.context_.SharesGeneration(current) || !catalog.catalog_ || !candidate.sourceAuthority_ ||
            !candidate.sourceAuthority_())
            return Output::Failure(MakeError(Mcp::McpErrors::AuthorizationDenied));
        const auto scene = catalog.scene_.lock();
        if (!scene || candidate.scene_.lock() != scene || scene->owner != std::this_thread::get_id() || !scene->inspectEditTargets)
            return Output::Failure(MakeError(Mcp::McpErrors::OwnerUnavailable));
        const auto inspected = catalog.catalog_->Inspect(candidate.intent_);
        if (!inspected.tool || inspected.status != Agent::AgentTurnStatus::Completed ||
            inspected.tool->effect != Agent::AgentPlannedEffect::Presentation || !inspected.tool->scope ||
            !MatchesPlanScene(inspected, current.Identity()) || inspected.tool->scope->objects != candidate.scope_.objects ||
            !InspectEditScope(*scene, *inspected.tool->scope))
            return Output::Failure(MakeError(Mcp::McpErrors::ToolUnavailable));
        auto authorized = [current, approvedTools = catalog.catalog_, sourceAuthority = candidate.sourceAuthority_] {
            return current.IsCurrent() && approvedTools->IsCurrent() && sourceAuthority();
        };
        return StageEditorScenePlan(*scene, candidate.intent_.name, nlohmann::json::parse(candidate.intent_.argumentsJson, nullptr, false),
                                    std::move(authorized));
    }
}  // namespace Horo::Editor
