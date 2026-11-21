#include "EditorLocalAgentContext.h"

#include "AgentHistoryReview.h"
#include "AgentTurnMcp.h"
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
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

        /** @brief Applies a short synchronous query deadline and removes the borrowed stop callback from the retained preview. */
        Platform::ProjectReadContext DiagnosticReadContext(Platform::ProjectReadContext &project, std::stop_token stop) {
            auto reading = project;
            reading.deadline = std::min(project.deadline, std::chrono::steady_clock::now() + std::chrono::seconds{2});
            reading.authorityStopped = [authority = std::exchange(project.authorityStopped, {}), stop] {
                return stop.stop_requested() || (authority && authority());
            };
            return reading;
        }

        /** @brief Reads at most one context item's bytes with both the workspace and worker stop conditions. */
        Result<Platform::ProjectReadTextSnapshot> ReadSavedText(const Platform::IProjectReadFiles &files, std::string_view path,
                                                                const Platform::ProjectReadContext &project, std::stop_token stop) {
            auto reading = project;
            reading.authorityStopped = [authority = project.authorityStopped, stop] {
                return stop.stop_requested() || (authority && authority());
            };
            constexpr Platform::ProjectReadLimits limits{.maximumFileBytes = Security::AgentPolicyLimits{}.maximumItemBytes};
            return files.Text(path, reading, limits);
        }

        /** @brief Matches the retained reader's content revision and exact workspace scope. */
        bool MatchesSavedText(const Platform::ProjectReadTextSnapshot &snapshot, const Platform::ProjectReadContext &project,
                              const Agent::AgentContextProvenance &item) {
            return snapshot.text && snapshot.projectIdentity == project.projectIdentity &&
                   snapshot.projectGeneration == project.projectGeneration && snapshot.path == item.origin &&
                   snapshot.revision == item.revision;
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

    /** @copydoc EditorLocalAgentContext::PrepareBuildDiagnostics */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::PrepareBuildDiagnostics(BuildOutputOperationTail tail,
                                                                                     Platform::ProjectReadContext project,
                                                                                     const std::uint64_t requestId,
                                                                                     std::shared_ptr<Security::SecureRandomSource> entropy,
                                                                                     std::stop_token stop) {
        using namespace Agent;
        const auto witness = tail.witness;
        const auto operation = tail.operation;
        const auto sequence = tail.lastSequence;
        const auto queryContext = DiagnosticReadContext(project, stop);
        const auto page = ReadLocalBuildDiagnostics(std::move(tail), queryContext, requestId, std::move(entropy));
        return CaptureDiagnosticPage(page, [witness] {
            return witness.IsCurrent();
        }, std::move(project), requestId, operation, {AgentHistoryReferenceKind::BuildOutput, operation, sequence});
    }

    /** @copydoc EditorLocalAgentContext::PrepareEditorDiagnostics */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::PrepareEditorDiagnostics(AgentEditorDiagnosticsSnapshot snapshot,
                                                                                      Platform::ProjectReadContext project,
                                                                                      const std::uint64_t requestId,
                                                                                      std::shared_ptr<Security::SecureRandomSource> entropy,
                                                                                      std::stop_token stop) {
        using namespace Agent;
        if (!snapshot.document || !snapshot.observation)
            return Result<EditorLocalAgentContext>::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        auto current = [lifetime = snapshot.lifetime] {
            return !lifetime.IsCancellationRequested();
        };
        const auto queryContext = DiagnosticReadContext(project, stop);
        const auto page = ReadLocalDiagnostics({std::format("editor:{}:{}", snapshot.document, snapshot.observation),
                                                std::move(snapshot.messages), snapshot.truncated, current},
                                               queryContext, requestId, std::move(entropy));
        return CaptureDiagnosticPage(page, std::move(current), std::move(project), requestId, {}, {});
    }

    /** @copydoc EditorLocalAgentContext::CaptureDiagnosticPage */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::CaptureDiagnosticPage(
        const Result<Application::CodeQueryPage> &page, std::function<bool()> current, Platform::ProjectReadContext project,
        const std::uint64_t requestId, const std::optional<OperationId> operation, const Agent::AgentHistoryReference reference) {
        using namespace Agent;
        using Output = Result<EditorLocalAgentContext>;
        if (page.HasError())
            return Output::Failure(page.ErrorValue());
        const auto &revision = page.Value().revision;
        const AgentContextSelection selected = AgentContextQueryRequest{revision,
                                                                        {.kind = Application::CodeQueryKind::Diagnostics,
                                                                         .limit = std::max(std::size_t{1}, page.Value().records.size())}};
        AgentContextReaders readers{.readOnlyQuery = [&page](const auto &, const auto &) {
            return page;
        }, .redact = RedactEvidence, .isCurrent = [current = std::move(current), revision](const AgentContextProvenance &item) {
            return current() && item.kind == AgentContextKind::Diagnostic && item.revision == revision;
        }};
        return Capture(selected, std::move(readers), std::move(project), requestId, {}, {}, operation, reference);
    }

    /** @copydoc EditorLocalAgentContext::PrepareSavedFile */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::PrepareSavedFile(std::shared_ptr<const Platform::IProjectReadFiles> files,
                                                                              const std::string_view path,
                                                                              Platform::ProjectReadContext project,
                                                                              const std::uint64_t requestId, std::stop_token stop) {
        using Output = Result<EditorLocalAgentContext>;
        using namespace Agent;
        if (!files)
            return Output::Failure(MakeError(Platform::ProjectReadErrors::Unavailable));
        if (path.size() > 256 || !Platform::IsSafeProjectReadPath(path))
            return Output::Failure(MakeError(Platform::ProjectReadErrors::UnsafePath));
        auto read = ReadSavedText(*files, path, project, stop);
        if (read.HasError())
            return Output::Failure(std::move(read).ErrorValue());
        // The caller's synchronous stop view may borrow capture state. Only the workspace token survives capture.
        project.authorityStopped = {};
        const auto &snapshot = read.Value();
        const AgentContextSelection selected = AgentContextTextRequest{AgentContextKind::File, std::string{path}, snapshot.revision};
        AgentContextReaders readers{.existingText = [&snapshot](std::string_view, const Platform::ProjectReadContext &) {
            return Result<std::optional<Platform::ProjectReadTextSnapshot>>::Success(snapshot);
        }, .redact = RedactEvidence, .isCurrent = [](const AgentContextProvenance &) {
            return true;  // The same rooted reader rechecks the exact revision immediately before dispatch.
        }};
        auto captured = Capture(selected, std::move(readers), std::move(project), requestId, {}, {}, {}, {});
        if (captured.HasError())
            return Output::Failure(std::move(captured).ErrorValue());
        auto preview = std::move(captured).Value();
        preview.savedFiles_ = std::move(files);
        return Output::Success(std::move(preview));
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
        if (tail.kind != BuildOutputTailKind::Messages || !tail.operation || !tail.lastSequence || tail.messages.empty() ||
            tail.messages.size() > 16)
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

    /** @copydoc EditorLocalAgentContext::PrepareSceneSelection */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::PrepareSceneSelection(const AgentSceneSelectionSnapshot &selection,
                                                                                   std::string projectIdentity,
                                                                                   const std::uint64_t workspaceGeneration,
                                                                                   CancellationToken lifetime,
                                                                                   const std::uint64_t requestId) {
        using Output = Result<EditorLocalAgentContext>;
        using namespace Agent;
        if (!selection.documentEpoch || !selection.selectionRevision || !selection.objectCount ||
            selection.objectCount > selection.objects.size())
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        const auto objects = std::span{selection.objects}.first(selection.objectCount);
        if (std::ranges::find(objects, 0) != objects.end())
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        const auto revision =
            std::format("scene:{}:{}:{}", selection.documentEpoch, selection.documentRevision, selection.selectionRevision);
        std::string text;
        for (const auto id : objects)
            text += std::format("Selected object ID {}\n", id);
        const AgentContextSelection selected = AgentContextAttachment{AgentContextAttachmentKind::Selection, "scene selection", revision,
                                                                      std::move(text), selection.truncated};
        AgentContextReaders readers{.redact = RedactEvidence,
                                    .isCurrent = [witness = selection.lifetime, revision](const AgentContextProvenance &item) {
            return !witness.IsCancellationRequested() && item.kind == AgentContextKind::Selection && item.revision == revision;
        }};
        return Capture(selected, std::move(readers), ProjectContext(std::move(projectIdentity), workspaceGeneration, std::move(lifetime)),
                       requestId, {}, {}, {}, {});
    }

    /** @copydoc EditorLocalAgentContext::PrepareClipboardAttachment */
    Result<EditorLocalAgentContext> EditorLocalAgentContext::PrepareClipboardAttachment(const Agent::AgentContextAttachmentKind kind,
                                                                                        std::string_view text, std::string projectIdentity,
                                                                                        const std::uint64_t workspaceGeneration,
                                                                                        CancellationToken lifetime,
                                                                                        const std::uint64_t requestId) {
        using namespace Agent;
        using Output = Result<EditorLocalAgentContext>;
        if ((kind != AgentContextAttachmentKind::Note && kind != AgentContextAttachmentKind::Report) || text.empty())
            return Output::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        if (text.size() > Security::AgentPolicyLimits{}.maximumItemBytes)
            return Output::Failure(MakeError(Application::CodeQueryErrors::Capacity));

        const auto revision = FormatSha256(ComputeSha256(std::as_bytes(std::span{text.data(), text.size()})));
        const AgentContextSelection selected = AgentContextAttachment{kind, "clipboard", revision, std::string{text}};
        const auto expectedKind = kind == AgentContextAttachmentKind::Note ? AgentContextKind::Note : AgentContextKind::Report;
        AgentContextReaders readers{.redact = RedactEvidence, .isCurrent = [revision, expectedKind](const AgentContextProvenance &item) {
            return item.kind == expectedKind && item.origin == "clipboard" && item.revision == revision;
        }};
        return Capture(selected, std::move(readers), ProjectContext(std::move(projectIdentity), workspaceGeneration, std::move(lifetime)),
                       requestId, {}, {}, {}, {});
    }

    /** @copydoc EditorLocalAgentContext::Matches */
    bool EditorLocalAgentContext::Matches(const SourceDocumentSnapshot &active) const noexcept {
        return source_.IsCurrent() && active.SharesStateWith(source_) && !project_.cancellation.IsCancellationRequested() &&
               std::chrono::steady_clock::now() < project_.deadline;
    }

    /** @copydoc EditorLocalAgentContext::PatchSource */
    SourceDocumentSnapshot EditorLocalAgentContext::PatchSource() const noexcept {
        const auto items = Items();
        if (selection_ || !source_.IsCurrent() || items.size() != 1 || items.front().provenance.kind != Agent::AgentContextKind::File)
            return {};
        return items.front().candidate.text.ends_with(source_.Text()) ? source_ : SourceDocumentSnapshot{};
    }

    /** @copydoc EditorLocalAgentContext::MatchesOpenSource */
    bool EditorLocalAgentContext::MatchesOpenSource(const std::span<const SourceDocumentSnapshot> open) const noexcept {
        return std::ranges::any_of(open, [this](const auto &snapshot) {
            return Matches(snapshot);
        });
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
    Agent::AgentTurnContext EditorLocalAgentContext::Dispatch(std::stop_token stop) {
        if (savedFiles_) {
            const auto &item = Items().front().provenance;
            const auto live = ReadSavedText(*savedFiles_, item.origin, project_, stop);
            if (live.HasError() || !MatchesSavedText(live.Value(), project_, item)) {
                static const std::function<bool(const Agent::AgentContextProvenance &)> stale = [](const Agent::AgentContextProvenance &) {
                    return false;
                };
                return {preview_, *policy_, project_, stale, Residence};
            }
        }
        return {preview_, *policy_, project_, current_, Residence};
    }
}  // namespace Horo::Editor
