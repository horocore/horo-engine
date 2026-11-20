#include "AgentContext.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <format>
#include <iterator>
#include <new>

namespace Horo::Agent {
    namespace {
        using namespace Application;
        using namespace Security;
        constexpr AgentPolicyLimits ContextLimits{};
        constexpr std::size_t MaximumQueryRows = 32;

        struct CapturedContext final {
            AgentContextProvenance provenance;
            AgentDataClass dataClass;
            std::string text;
        };

        using CaptureResult = Result<CapturedContext>;

        /** @brief Allows textual whitespace while rejecting UTF-8 errors and ASCII binary/control bytes. */
        bool IsText(const std::string_view text) {
            return IsValidUtf8ScalarSequence(text) && std::ranges::none_of(text, [](const unsigned char character) {
                return (character < 32 && character != '\t' && character != '\r' && character != '\n') || character == 127;
            });
        }

        /** @brief Validates the display/dispatch metadata as a single bounded line. */
        bool IsLabel(const std::string_view text) {
            return !text.empty() && text.size() <= 256 && IsText(text) && text.find_first_of("\r\n\t") == std::string_view::npos;
        }

        /** @brief Reads only the caller-selected existing snapshot, or a file through the explicit rooted capability. */
        Result<Platform::ProjectReadTextSnapshot> ReadText(const AgentContextTextRequest &request, const AgentContextReaders &readers,
                                                           const Platform::ProjectReadContext &context) {
            using Reading = Result<Platform::ProjectReadTextSnapshot>;
            if (readers.existingText) {
                auto existing = readers.existingText(request.path, context);
                if (existing.HasError())
                    return Reading::Failure(existing.ErrorValue());
                if (existing.Value())
                    return Reading::Success(std::move(*std::move(existing).Value()));
            }
            if (request.kind == AgentContextKind::File && readers.files)
                return readers.files->Text(request.path, context, {});
            return Reading::Failure(MakeError(CodeQueryErrors::Unavailable));
        }

        /** @brief Validates the exact snapshot before selecting a complete UTF-8 byte range or complete file. */
        CaptureResult CaptureItem(const AgentContextTextRequest &request, const AgentContextReaders &readers,
                                  const Platform::ProjectReadContext &context) {
            using enum AgentContextKind;
            const bool range = request.kind == SourceRange;
            if (!IsLabel(request.path) || !Platform::IsSafeProjectReadPath(request.path))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::UnsafePath));
            if (!IsLabel(request.revision) || (range ? request.length == 0 : request.kind != File || request.offset || request.length))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Invalid));
            const auto snapshot = ReadText(request, readers, context);
            if (snapshot.HasError())
                return CaptureResult::Failure(snapshot.ErrorValue());
            const auto &source = snapshot.Value();
            if (source.projectIdentity != context.projectIdentity || source.projectGeneration != context.projectGeneration ||
                source.path != request.path || source.revision != request.revision || !source.text)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Stale));
            if (source.text->size() > Platform::ProjectReadLimits{}.maximumFileBytes)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Capacity));
            if (!IsText(*source.text))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Binary));
            const auto bytes = range ? request.length : source.text->size();
            if (request.offset > source.text->size() || bytes > source.text->size() - request.offset)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Invalid));
            if (bytes > ContextLimits.maximumItemBytes)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Capacity));
            const auto text = std::string_view{*source.text}.substr(request.offset, bytes);
            if (!IsText(text))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Encoding));
            return CaptureResult::Success(
                {{request.kind, request.path, request.revision, request.offset, bytes}, AgentDataClass::ProjectFile, std::string{text}});
        }

        /** @brief Reads one explicitly requested bounded MCP diagnostic/build page and preserves its omission evidence. */
        CaptureResult CaptureItem(const AgentContextQueryRequest &request, const AgentContextReaders &readers,
                                  const Platform::ProjectReadContext &context) {
            const auto &query = request.query;
            const bool diagnostic = query.kind == CodeQueryKind::Diagnostics;
            if ((!diagnostic && query.kind != CodeQueryKind::BuildStatus) || !query.pattern.empty() || !IsLabel(request.revision) ||
                (!query.path.empty() && (!IsLabel(query.path) || !Platform::IsSafeProjectReadPath(query.path))) || query.limit == 0 ||
                query.limit > MaximumQueryRows || (query.expectedRevision && *query.expectedRevision != request.revision))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Invalid));
            if (!readers.readOnlyQuery)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Unavailable));
            auto exact = query;
            exact.expectedRevision = request.revision;
            const auto result = readers.readOnlyQuery(exact, context);
            if (result.HasError())
                return CaptureResult::Failure(result.ErrorValue());
            const auto &page = result.Value();
            if (page.projectGeneration != context.projectGeneration || page.revision != request.revision)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Stale));
            if (page.records.size() > query.limit || !page.text.empty() || (page.nextOffset && *page.nextOffset <= query.offset))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Invalid));
            std::string text;
            for (const auto &row : page.records) {
                if (row.kind != (diagnostic ? CodeQueryRecordKind::Diagnostic : CodeQueryRecordKind::Build) || !IsText(row.label) ||
                    (!row.path.empty() && (!IsLabel(row.path) || !Platform::IsSafeProjectReadPath(row.path))))
                    return CaptureResult::Failure(MakeError(CodeQueryErrors::Invalid));
                if (row.label.size() > ContextLimits.maximumItemBytes)
                    return CaptureResult::Failure(MakeError(CodeQueryErrors::Capacity));
                text += std::format("{} {}:{} {}\n", row.path, row.line, row.column, row.label);
                if (text.size() > ContextLimits.maximumItemBytes)
                    return CaptureResult::Failure(MakeError(CodeQueryErrors::Capacity));
            }
            const auto kind = diagnostic ? AgentContextKind::Diagnostic : AgentContextKind::BuildStatus;
            const bool truncated = page.nextOffset.has_value() || page.droppedRecords != 0 || query.offset != 0;
            return CaptureResult::Success(
                {{kind, query.path.empty() ? "project" : query.path, page.revision, query.offset, text.size(), truncated},
                 AgentDataClass::ToolResult,
                 std::move(text)});
        }

        /** @brief Maps only the closed set of trusted host attachment kinds. */
        std::optional<AgentContextKind> AttachmentKind(const AgentContextAttachmentKind kind) {
            using enum AgentContextAttachmentKind;
            switch (kind) {
                case Selection:
                    return AgentContextKind::Selection;
                case BuildOutput:
                    return AgentContextKind::BuildOutput;
                case Note:
                    return AgentContextKind::Note;
                case Report:
                    return AgentContextKind::Report;
            }
            return std::nullopt;
        }

        /** @brief Admits owned typed text attachments without acquiring any additional editor or file data. */
        CaptureResult CaptureItem(const AgentContextAttachment &attachment, const AgentContextReaders &,
                                  const Platform::ProjectReadContext &) {
            const auto kind = AttachmentKind(attachment.kind);
            if (!kind || !IsLabel(attachment.origin) || !IsLabel(attachment.revision))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Invalid));
            if (attachment.text.size() > ContextLimits.maximumItemBytes)
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Capacity));
            if (!IsText(attachment.text))
                return CaptureResult::Failure(MakeError(CodeQueryErrors::Binary));
            const auto dataClass = *kind == AgentContextKind::Selection ? AgentDataClass::SceneData : AgentDataClass::SensitiveProjectData;
            return CaptureResult::Success({{*kind, attachment.origin, attachment.revision, 0, attachment.text.size(), attachment.truncated},
                                           dataClass,
                                           attachment.text});
        }

        /** @brief Stable labels used in the exact rendered context payload. */
        std::string_view KindName(const AgentContextKind kind) noexcept {
            using enum AgentContextKind;
            switch (kind) {
                case File:
                    return "file";
                case SourceRange:
                    return "source range";
                case Diagnostic:
                    return "diagnostic";
                case BuildStatus:
                    return "build status";
                case BuildOutput:
                    return "build output";
                case Selection:
                    return "selection";
                case Note:
                    return "attached note";
                case Report:
                    return "attached report";
            }
            return {};
        }

        /** @brief Copies the same immutable projected payload for both consent and dispatch. */
        std::vector<AgentContextCandidate> Candidates(const std::span<const AgentContextPreviewItem> items) {
            std::vector<AgentContextCandidate> result;
            result.reserve(items.size());
            for (const auto &item : items)
                result.push_back(item.candidate);
            return result;
        }
    }  // namespace

    /** @copydoc AgentContextPreview::Capture */
    Result<AgentContextPreview> AgentContextPreview::Capture(const std::span<const AgentContextSelection> selection,
                                                             const AgentContextReaders &readers,
                                                             const Platform::ProjectReadContext &context,
                                                             const Security::AgentProviderResidence provider, const std::uint64_t requestId,
                                                             const std::uint64_t trustRevision) {
        using Preview = Result<AgentContextPreview>;
        using enum Security::AgentProviderResidence;
        if (selection.empty() || selection.size() > ContextLimits.maximumContextItems || !IsLabel(context.projectIdentity) ||
            context.projectGeneration == 0 || requestId == 0 || trustRevision == 0 || (provider != Local && provider != Cloud) ||
            !readers.redact || !readers.isCurrent)
            return Preview::Failure(MakeError(Application::CodeQueryErrors::Invalid));
        try {
            AgentContextPreview preview;
            preview.items_.reserve(selection.size());
            preview.project_ = context.projectIdentity;
            preview.generation_ = context.projectGeneration;
            preview.requestId_ = requestId;
            preview.trustRevision_ = trustRevision;
            preview.provider_ = provider;
            std::size_t totalBytes{};
            for (const auto &selected : selection) {
                if (const auto active = Platform::CheckProjectReadContext(context); active.HasError())
                    return Preview::Failure(active.ErrorValue());
                auto captured = std::visit([&](const auto &item) {
                    return CaptureItem(item, readers, context);
                }, selected);
                if (captured.HasError())
                    return Preview::Failure(captured.ErrorValue());
                auto item = std::move(captured).Value();
                if (!readers.isCurrent(item.provenance))
                    return Preview::Failure(MakeError(Application::CodeQueryErrors::Stale));
                const auto raw = std::format("Editor context evidence ({})\nOrigin: {}\nRevision: {}\nOffset: {}\nPartial: {}\n\n{}",
                                             KindName(item.provenance.kind), item.provenance.origin, item.provenance.revision,
                                             item.provenance.offset, item.provenance.truncated ? "yes" : "no", item.text);
                auto redacted = readers.redact(item.provenance, raw);
                if (redacted.HasError())
                    return Preview::Failure(redacted.ErrorValue());
                auto payload = std::move(redacted).Value();
                if (payload.empty() || !IsText(payload))
                    return Preview::Failure(MakeError(Application::CodeQueryErrors::Invalid));
                if (payload.size() > ContextLimits.maximumItemBytes || payload.size() > ContextLimits.maximumContextBytes - totalBytes)
                    return Preview::Failure(MakeError(Application::CodeQueryErrors::Capacity));
                totalBytes += payload.size();
                preview.items_.push_back(
                    {std::move(item.provenance), {preview.items_.size() + 1, item.dataClass, std::move(payload), true, true}});
            }
            if (const auto active = Platform::CheckProjectReadContext(context); active.HasError())
                return Preview::Failure(active.ErrorValue());
            return Preview::Success(std::move(preview));
        } catch (const std::bad_alloc &) {
            return Preview::Failure(MakeError(Application::CodeQueryErrors::Capacity));
        }
    }

    /** @copydoc AgentContextPreview::Grant */
    Security::AgentPolicyDecision AgentContextPreview::Grant(Security::AgentPolicy &policy) const {
        return policy.GrantContextConsent(provider_, requestId_, Candidates(items_), trustRevision_);
    }

    /** @copydoc AgentContextPreview::IsCurrent */
    bool AgentContextPreview::IsCurrent(const Security::AgentPolicy &policy, const Platform::ProjectReadContext &context,
                                        const std::function<bool(const AgentContextProvenance &)> &isCurrent) const {
        return policy.ProjectRevision() == trustRevision_ && isCurrent && context.projectIdentity == project_ &&
               context.projectGeneration == generation_ && Platform::CheckProjectReadContext(context).HasValue() &&
               std::ranges::all_of(items_, [&](const auto &item) {
            return isCurrent(item.provenance);
        });
    }

    /** @copydoc AgentContextPreview::InsertBeforeDispatch */
    Security::AgentPolicyDecision AgentContextPreview::InsertBeforeDispatch(
        Security::AgentPolicy &policy, const Platform::ProjectReadContext &context,
        const std::function<bool(const AgentContextProvenance &)> &isCurrent, const Security::AgentProviderResidence provider,
        const std::uint64_t requestId, ModelRequest &request) const {
        using enum Security::AgentPolicyReason;
        if (provider != provider_ || requestId != requestId_ || request.messages.empty() || request.messages.back().role != ModelRole::User)
            return {InvalidRequest, trustRevision_};
        if (!IsCurrent(policy, context, isCurrent)) {
            policy.DenyContext(requestId_);
            return {StaleAuthority, trustRevision_};
        }
        try {
            auto candidates = Candidates(items_);
            std::vector<ModelMessage> messages;
            messages.reserve(candidates.size());
            for (const auto &candidate : candidates)
                messages.push_back({.role = ModelRole::User, .text = candidate.text});
            constexpr std::size_t MaximumModelMessages = 128;
            if (request.messages.size() > MaximumModelMessages - messages.size())
                return {ContextLimitExceeded, trustRevision_};
            request.messages.reserve(request.messages.size() + messages.size());
            auto [envelope, built] = policy.BuildContext(provider_, requestId_, candidates, trustRevision_);
            if (!built.Allowed())
                return built;
            const auto allowed = policy.AuthorizeContextDispatch(envelope);
            if (allowed.Allowed())
                request.messages.insert(request.messages.end() - 1, std::make_move_iterator(messages.begin()),
                                        std::make_move_iterator(messages.end()));
            return allowed;
        } catch (const std::bad_alloc &) {
            policy.DenyContext(requestId_);
            return {ContextLimitExceeded, trustRevision_};
        }
    }
}  // namespace Horo::Agent
