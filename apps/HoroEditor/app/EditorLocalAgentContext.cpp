#include "EditorLocalAgentContext.h"

#include "AgentHistoryReview.h"

#include <format>

namespace Horo::Editor {
    namespace {
        constexpr auto Residence = Security::AgentProviderResidence::Local;
        constexpr auto PreviewLifetime = std::chrono::minutes{5};

        /** @brief Reuses the host's bounded plain-display redaction without passing any raw data into history. */
        Result<std::string> RedactEvidence(const Agent::AgentContextProvenance &, const std::string_view text) {
            auto admitted = Agent::RedactHistoryDisplayText(Agent::AgentHistoryTextKind::UserMessage, text);
            if (!admitted.admittedText)
                return Result<std::string>::Failure(MakeError(Application::CodeQueryErrors::Invalid));
            return Result<std::string>::Success(std::move(*admitted.admittedText));
        }

        /** @brief Builds a bounded project fence shared by explicit source and build evidence. */
        Platform::ProjectReadContext ProjectContext(std::string identity, const std::uint64_t generation, CancellationToken lifetime) {
            return {std::move(identity), generation, std::move(lifetime), std::chrono::steady_clock::now() + PreviewLifetime};
        }
    }  // namespace

    EditorLocalAgentContext::EditorLocalAgentContext(SourceDocumentSnapshot source, Platform::ProjectReadContext project,
                                                     std::unique_ptr<Security::AgentPolicy> policy, Agent::AgentContextPreview preview,
                                                     std::function<bool(const Agent::AgentContextProvenance &)> current,
                                                     const std::optional<SourceContextRange> selection,
                                                     const std::optional<OperationId> buildOperation,
                                                     const Agent::AgentHistoryReference historyReference)
        : source_(std::move(source)), project_(std::move(project)), policy_(std::move(policy)), preview_(std::move(preview)),
          current_(std::move(current)), selection_(selection), buildOperation_(buildOperation), historyReference_(historyReference) {}

    /** @brief Creates one exclusive policy and preview without granting consent or transferring any source owners. */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::Capture(
        const Agent::AgentContextSelection &selected, Agent::AgentContextReaders readers, Platform::ProjectReadContext project,
        const std::uint64_t requestId, SourceDocumentSnapshot source, const std::optional<SourceContextRange> range,
        const std::optional<OperationId> buildOperation, const Agent::AgentHistoryReference historyReference) {
        using Output = Result<EditorLocalAgentContext>;
        auto policy = std::make_unique<Security::AgentPolicy>();
        // Local evidence consent grants no project trust and no tool capability.
        if (!policy->SetProjectTrust(false, project.projectGeneration))
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        auto preview =
            Agent::AgentContextPreview::Capture(std::span{&selected, 1}, readers, project, Residence, requestId, project.projectGeneration);
        if (preview.HasError())
            return Output::Failure(std::move(preview).ErrorValue());
        return Output::Success(EditorLocalAgentContext{std::move(source), std::move(project), std::move(policy), std::move(preview).Value(),
                                                       std::move(readers.isCurrent), range, buildOperation, historyReference});
    }

    /** @copydoc EditorLocalAgentContext::Prepare */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::Prepare(SourceDocumentSnapshot source, std::string projectIdentity,
                                                                     const std::uint64_t workspaceGeneration, CancellationToken lifetime,
                                                                     const std::uint64_t requestId,
                                                                     const std::optional<SourceContextRange> selection) {
        using Output = Result<EditorLocalAgentContext>;
        using namespace Agent;
        if (!source.IsCurrent() || lifetime.IsCancellationRequested())
            return Output::Failure(MakeError(Application::CodeQueryErrors::Stale));
        const auto textSize = source.Text().size();
        if (selection && (selection->offset > textSize || selection->length == 0 || selection->length > textSize - selection->offset))
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        if (textSize > Platform::ProjectReadLimits{}.maximumFileBytes ||
            (selection ? selection->length : textSize) > Security::AgentPolicyLimits{}.maximumItemBytes)
            return Output::Failure(MakeError(Application::CodeQueryErrors::Capacity));

        const auto identity = source.Identity();
        const auto &path = identity.key.source.Value();
        const auto revision = std::format("source:{}:{}", identity.instance.Value(), source.Revision());
        auto current = [source, path, revision](const AgentContextProvenance &provenance) {
            return source.IsCurrent() && provenance.origin == path && provenance.revision == revision;
        };
        const bool sensitive = selection && IsSensitiveDisplayRange(source.Text(), selection->offset, selection->length);
        const AgentContextReaders
            readers{.existingText = [&source, &path, &revision](std::string_view requested, const Platform::ProjectReadContext &context) {
            using Reading = Result<std::optional<Platform::ProjectReadTextSnapshot>>;
            if (requested != path || !source.IsCurrent())
                return Reading::Failure(MakeError(Application::CodeQueryErrors::Stale));
            return Reading::Success(
                Platform::ProjectReadTextSnapshot{context.projectIdentity, context.projectGeneration, path, revision, source.TextLease()});
        }, .redact = [sensitive](const AgentContextProvenance &provenance, const std::string_view payload) {
            if (!sensitive)
                return RedactEvidence(provenance, payload);
            const std::string framed = std::string{payload.substr(0, payload.size() - provenance.capturedBytes)} + "[redacted]";
            return RedactEvidence(provenance, framed);
        }, .isCurrent = current};
        const AgentContextSelection selected =
            AgentContextTextRequest{selection ? AgentContextKind::SourceRange : AgentContextKind::File, path, revision,
                                    selection ? selection->offset : 0, selection ? selection->length : 0};
        return Capture(selected, readers, ProjectContext(std::move(projectIdentity), workspaceGeneration, std::move(lifetime)), requestId,
                       source, selection, {}, {AgentHistoryReferenceKind::Document, identity.instance.Value(), source.Revision()});
    }

    /** @copydoc EditorLocalAgentContext::PrepareBuildOutput */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::PrepareBuildOutput(BuildOutputOperationTail tail, std::string projectIdentity,
                                                                                const std::uint64_t workspaceGeneration,
                                                                                CancellationToken lifetime, const std::uint64_t requestId) {
        using Output = Result<EditorLocalAgentContext>;
        using namespace Agent;
        if (!tail.witness.IsCurrent() || lifetime.IsCancellationRequested())
            return Output::Failure(MakeError(Application::CodeQueryErrors::Stale));
        if (!tail.operation || !tail.lastSequence || tail.messages.empty() || tail.messages.size() > 16)
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        constexpr std::size_t MaximumBytes = 3U << 10U;
        std::string text;
        for (const auto &message : tail.messages) {
            if (text.size() == MaximumBytes || message.size() > MaximumBytes - text.size() - 1)
                return Output::Failure(MakeError(Application::CodeQueryErrors::Capacity));
            text.append(message);
            text.push_back('\n');
        }
        const auto origin = std::format("gameplay build {}", tail.operation);
        const auto revision = std::format("output:{}", tail.lastSequence);
        auto current = [witness = std::move(tail.witness), origin, revision](const AgentContextProvenance &provenance) {
            return witness.IsCurrent() && provenance.origin == origin && provenance.revision == revision;
        };
        const AgentContextSelection selected =
            AgentContextAttachment{AgentContextAttachmentKind::BuildOutput, origin, revision, std::move(text), tail.truncated};
        AgentContextReaders readers{.redact = RedactEvidence, .isCurrent = std::move(current)};
        return Capture(selected, std::move(readers), ProjectContext(std::move(projectIdentity), workspaceGeneration, std::move(lifetime)),
                       requestId, {}, {}, tail.operation, {AgentHistoryReferenceKind::BuildOutput, tail.operation, tail.lastSequence});
    }

    /** @copydoc EditorLocalAgentContext::Matches */
    bool EditorLocalAgentContext::Matches(const SourceDocumentSnapshot &active) const noexcept {
        return source_.IsCurrent() && active.SharesStateWith(source_) && !project_.cancellation.IsCancellationRequested() &&
               std::chrono::steady_clock::now() < project_.deadline;
    }

    /** @copydoc EditorLocalAgentContext::IsCurrent */
    bool EditorLocalAgentContext::IsCurrent() const {
        return preview_.IsCurrent(*policy_, project_, current_);
    }

    /** @copydoc EditorLocalAgentContext::Approve */
    bool EditorLocalAgentContext::Approve() {
        return IsCurrent() && preview_.Grant(*policy_).Allowed();
    }

    /** @copydoc EditorLocalAgentContext::Dispatch */
    Agent::AgentTurnContext EditorLocalAgentContext::Dispatch() noexcept {
        return {preview_, *policy_, project_, current_, Residence};
    }
}  // namespace Horo::Editor
