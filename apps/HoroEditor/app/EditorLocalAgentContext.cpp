#include "EditorLocalAgentContext.h"

#include "AgentHistoryReview.h"

#include <array>
#include <format>

namespace Horo::Editor {
    namespace {
        constexpr auto Residence = Security::AgentProviderResidence::Local;
        constexpr auto PreviewLifetime = std::chrono::minutes{5};

        /** @brief Reuses the host's bounded plain-display redaction without passing any raw data into history. */
        Result<std::string> RedactSource(const Agent::AgentContextProvenance &, const std::string_view text) {
            auto admitted = Agent::RedactHistoryDisplayText(Agent::AgentHistoryTextKind::UserMessage, text);
            if (!admitted.admittedText)
                return Result<std::string>::Failure(MakeError(Application::CodeQueryErrors::Invalid));
            return Result<std::string>::Success(std::move(*admitted.admittedText));
        }
    }  // namespace

    EditorLocalAgentContext::EditorLocalAgentContext(SourceDocumentSnapshot source, Platform::ProjectReadContext project,
                                                     std::unique_ptr<Security::AgentPolicy> policy, Agent::AgentContextPreview preview,
                                                     std::function<bool(const Agent::AgentContextProvenance &)> current)
        : source_(std::move(source)), project_(std::move(project)), policy_(std::move(policy)), preview_(std::move(preview)),
          current_(std::move(current)) {}

    /** @copydoc EditorLocalAgentContext::Prepare */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::Prepare(SourceDocumentSnapshot source, std::string projectIdentity,
                                                                     const std::uint64_t workspaceGeneration, CancellationToken lifetime,
                                                                     const std::uint64_t requestId) {
        using Output = Result<EditorLocalAgentContext>;
        using namespace Agent;
        if (!source.IsCurrent() || lifetime.IsCancellationRequested())
            return Output::Failure(MakeError(Application::CodeQueryErrors::Stale));
        if (source.Text().size() > Security::AgentPolicyLimits{}.maximumItemBytes)
            return Output::Failure(MakeError(Application::CodeQueryErrors::Capacity));

        const auto identity = source.Identity();
        const auto &path = identity.key.source.Value();
        const auto revision = std::format("source:{}:{}", identity.instance.Value(), source.Revision());
        Platform::ProjectReadContext project{std::move(projectIdentity), workspaceGeneration, std::move(lifetime),
                                             std::chrono::steady_clock::now() + PreviewLifetime};
        auto policy = std::make_unique<Security::AgentPolicy>();
        // The panel does not mint project trust or authorize any non-read-only tool.
        if (!policy->SetProjectTrust(false, workspaceGeneration))
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));

        auto current = [source, path, revision](const AgentContextProvenance &provenance) {
            return source.IsCurrent() && provenance.origin == path && provenance.revision == revision;
        };
        const AgentContextReaders
            readers{.existingText = [&source, &path, &revision](std::string_view requested, const Platform::ProjectReadContext &context) {
            using Reading = Result<std::optional<Platform::ProjectReadTextSnapshot>>;
            if (requested != path || !source.IsCurrent())
                return Reading::Failure(MakeError(Application::CodeQueryErrors::Stale));
            return Reading::Success(Platform::ProjectReadTextSnapshot{context.projectIdentity, context.projectGeneration, path, revision,
                                                                      std::make_shared<const std::string>(source.Text())});
        }, .redact = RedactSource, .isCurrent = current};
        const std::array<AgentContextSelection, 1> selected{AgentContextTextRequest{AgentContextKind::File, path, revision}};
        auto preview = AgentContextPreview::Capture(selected, readers, project, Residence, requestId, workspaceGeneration);
        if (preview.HasError())
            return Output::Failure(std::move(preview).ErrorValue());
        return Output::Success(EditorLocalAgentContext{std::move(source), std::move(project), std::move(policy), std::move(preview).Value(),
                                                       std::move(current)});
    }

    /** @copydoc EditorLocalAgentContext::Matches */
    bool EditorLocalAgentContext::Matches(const SourceDocumentSnapshot &active) const noexcept {
        return source_.IsCurrent() && active.SharesStateWith(source_) && !project_.cancellation.IsCancellationRequested() &&
               std::chrono::steady_clock::now() < project_.deadline;
    }

    /** @copydoc EditorLocalAgentContext::Approve */
    bool EditorLocalAgentContext::Approve() {
        return preview_.IsCurrent(*policy_, project_, current_) && preview_.Grant(*policy_).Allowed();
    }

    /** @copydoc EditorLocalAgentContext::Dispatch */
    Agent::AgentTurnContext EditorLocalAgentContext::Dispatch() noexcept {
        return {preview_, *policy_, project_, current_, Residence};
    }
}  // namespace Horo::Editor
