#include "EditorLocalAgentPatch.h"

#include <array>
#include <format>

namespace Horo::Editor {
    namespace {
        using ReviewResult = Result<EditorLocalAgentPatchReview>;

        struct PatchReply final {
            std::string_view body;
            bool unified{};
        };

        /** @brief Reads one fenced patch, keeping the last diff-record delimiter before the fence. */
        PatchReply ReadFence(std::string_view answer) {
            const auto line = answer.find('\n');
            if (line == std::string_view::npos)
                return {};
            auto language = answer.substr(0, line);
            if (language.ends_with('\r'))
                language.remove_suffix(1);
            if (language != "diff" && language != "json")
                return {};
            answer = answer.substr(line + 1);
            const auto closing = answer.find_last_not_of(" \t\r\n");
            if (closing == std::string_view::npos || !answer.substr(0, closing + 1).ends_with("\n```"))
                return {};
            return {answer.substr(0, closing - 2), language == "diff"};
        }

        /** @brief Recognizes a complete standalone patch or one fenced patch. */
        PatchReply Extract(std::string_view answer) {
            const auto start = answer.find_first_not_of(" \t\r\n");
            if (start == std::string_view::npos)
                return {};
            answer.remove_prefix(start);
            if (answer.starts_with('{'))
                return {answer, false};
            if (answer.starts_with("diff --git ") || answer.starts_with("--- "))
                return {answer, true};
            return answer.starts_with("```") ? ReadFence(answer.substr(3)) : PatchReply{};
        }
    }  // namespace

    /** @copydoc IsEditorLocalPatchReply */
    bool IsEditorLocalPatchReply(const std::string_view answer) {
        return !Extract(answer).body.empty();
    }

    /** @copydoc PrepareEditorLocalPatchReview */
    Result<EditorLocalAgentPatchReview> PrepareEditorLocalPatchReview(const std::string_view answer,
                                                                      const EditorLocalAgentPatchSource &offered,
                                                                      const std::string_view project, const std::uint64_t generation) {
        using namespace Proposals;
        if (!offered.source.IsCurrent())
            return ReviewResult::Failure(MakeError(Errors::StaleBase));
        const auto reply = Extract(answer);
        if (reply.body.empty())
            return ReviewResult::Failure(MakeError(Errors::Malformed));
        const auto path = offered.source.Identity().key.source;
        auto policy = ProjectPolicy::Capture(std::string{project}, generation, {{path, false}});
        if (policy.HasError())
            return ReviewResult::Failure(policy.ErrorValue());
        const auto model = FormatSha256(ComputeSha256(std::as_bytes(std::span{offered.model})));
        ProposalIdentity identity{std::format("local.{}.{}", generation, offered.turn), offered.turn, {"ollama", model, "source-review"}};
        const auto sources = std::span{&offered.source, 1};
        auto proposal = reply.unified ? DecodeUnifiedPatch(reply.body, policy.Value(), std::move(identity), sources)
                                      : DecodeStructuredPatch(reply.body, policy.Value(), std::move(identity), sources);
        if (proposal.HasError())
            return ReviewResult::Failure(proposal.ErrorValue());
        const auto files = proposal.Value().Files();
        if (files.size() != 1 || !std::holds_alternative<EditFile>(files.front()))
            return ReviewResult::Failure(MakeError(Errors::PolicyDenied));
        const std::array observations{SourcePathObservation{path, offered.source}};
        auto plan = PlanWorkspaceEdits(proposal.Value(), policy.Value(), observations);
        if (plan.HasError())
            return ReviewResult::Failure(plan.ErrorValue());
        if (!std::holds_alternative<FilePreview>(plan.Value().entries.front()))
            return ReviewResult::Failure(MakeError(Errors::StaleBase));
        return ReviewResult::Success({std::move(proposal).Value(), std::move(plan).Value()});
    }
}  // namespace Horo::Editor
