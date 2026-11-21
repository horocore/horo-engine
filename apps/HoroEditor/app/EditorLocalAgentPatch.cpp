#include "EditorLocalAgentPatch.h"

#include "Horo/Foundation/Platform.h"

#include <algorithm>
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

        /** @brief Collects the exact model ranges, without rendering or retaining their text. */
        EditorPatchFileCard DescribeFile(const Proposals::FileOperation &operation) {
            using namespace Proposals;
            return std::visit([](const auto &file) -> EditorPatchFileCard {
                using File = std::decay_t<decltype(file)>;
                if constexpr (std::is_same_v<File, CreateFile>) {
                    return {file.path, {}, EditorPatchFileKind::Create, 0, 0, file.text.size()};
                } else if constexpr (std::is_same_v<File, DeleteFile>) {
                    return {file.base.path, {}, EditorPatchFileKind::Delete, 0, file.base.bytes, 0};
                } else {
                    EditorPatchFileCard card{file.base.path, {}, EditorPatchFileKind::Edit, file.hunks.size()};
                    if constexpr (std::is_same_v<File, RenameFile>) {
                        card.kind = EditorPatchFileKind::Rename;
                        card.destination = file.destination;
                    }
                    for (const auto &hunk : file.hunks) {
                        card.removedBytes += hunk.before.size();
                        card.addedBytes += hunk.after.size();
                    }
                    return card;
                }
            }, operation);
        }

        /** @brief Finds the distinct destination operand of a structural operation. */
        const SourceDocumentId *DestinationPath(const EditorPatchFileCard &card) {
            if (card.kind == EditorPatchFileKind::Create)
                return &card.path;
            return card.destination ? &*card.destination : nullptr;
        }
    }  // namespace

    /** @copydoc DescribeEditorPatchFiles */
    std::vector<EditorPatchFileCard> DescribeEditorPatchFiles(const Proposals::ChangeSet &proposal) {
        std::vector<EditorPatchFileCard> cards;
        cards.reserve(proposal.Files().size());
        for (const auto &operation : proposal.Files())
            cards.push_back(DescribeFile(operation));
        return cards;
    }

    /** @copydoc EditorPatchApplyKind */
    std::optional<EditorPatchFileKind> EditorPatchApplyKind(const EditorLocalAgentPatchReview &review) {
        using enum EditorPatchFileKind;
        std::optional<EditorPatchFileKind> selected;
        for (std::size_t index = 0; index < review.files.size(); ++index) {
            if (!review.decisions.HasAcceptedChanges(index))
                continue;
            const auto kind = review.files[index].kind;
            if (selected && (*selected == Delete || kind == Delete))
                return std::nullopt;
            if (!selected || kind == Rename || (*selected == Edit && kind == Create))
                selected = kind;
        }
        return selected;
    }

    /** @copydoc ObserveEditorPatchDestinations */
    void ObserveEditorPatchDestinations(const std::span<const EditorPatchFileCard> cards, const std::filesystem::path &root,
                                        const DurableFileSystem &files, std::vector<Proposals::SourcePathObservation> &observations,
                                        const CancellationToken &stop) {
        for (const auto &card : cards) {
            const auto *path = DestinationPath(card);
            if (!path || std::ranges::any_of(observations, [path](const auto &entry) {
                return entry.path == *path;
            }))
                continue;
            observations.push_back({*path, Proposals::ObserveSourceDestination(*path, root, files, stop)});
        }
    }

    /** @copydoc IsEditorLocalPatchReply */
    bool IsEditorLocalPatchReply(const std::string_view answer) {
        return !Extract(answer).body.empty();
    }

    /** @copydoc PrepareEditorLocalPatchReview */
    Result<EditorLocalAgentPatchReview> PrepareEditorLocalPatchReview(const std::string_view answer,
                                                                      const EditorLocalAgentPatchSource &offered,
                                                                      const std::string_view project, const std::uint64_t generation,
                                                                      const EditorPatchDestination *destination) {
        using namespace Proposals;
        if (offered.sources.empty())
            return ReviewResult::Failure(MakeError(Errors::StaleBase));
        const auto reply = Extract(answer);
        if (reply.body.empty())
            return ReviewResult::Failure(MakeError(Errors::Malformed));
        std::vector<PathRule> rules;
        std::vector<SourcePathObservation> observations;
        std::vector<SourceDocumentId> offeredPaths;
        for (const auto &source : offered.sources) {
            const auto path = source.Identity().key.source;
            rules.push_back({path, false});
            observations.push_back({path, source});
            offeredPaths.push_back(path);
        }
        if (destination && std::ranges::find(offeredPaths, destination->path) == offeredPaths.end()) {
            rules.push_back({destination->path, false});
            offeredPaths.push_back(destination->path);
        }
        auto policy = ProjectPolicy::Capture(std::string{project}, generation, std::move(rules));
        if (policy.HasError())
            return ReviewResult::Failure(policy.ErrorValue());
        const auto model = FormatSha256(ComputeSha256(std::as_bytes(std::span{offered.model})));
        ProposalIdentity identity{std::format("local.{}.{}", generation, offered.turn), offered.turn, {"ollama", model, "source-review"}};
        const auto sources = std::span{offered.sources};
        auto proposal = reply.unified ? DecodeUnifiedPatch(reply.body, policy.Value(), std::move(identity), sources)
                                      : DecodeStructuredPatch(reply.body, policy.Value(), std::move(identity), sources);
        if (proposal.HasError())
            return ReviewResult::Failure(proposal.ErrorValue());
        auto cards = DescribeEditorPatchFiles(proposal.Value());
        if (destination) {
            const bool matches = std::ranges::any_of(cards, [&](const auto &card) {
                const auto *path = DestinationPath(card);
                return path && *path == destination->path;
            });
            if (!matches)
                return ReviewResult::Failure(MakeError(Errors::PolicyDenied));
            ObserveEditorPatchDestinations(cards, destination->root, destination->files, observations, destination->stop);
        }
        auto plan = PlanWorkspaceEdits(proposal.Value(), policy.Value(), observations);
        if (plan.HasError())
            return ReviewResult::Failure(plan.ErrorValue());
        ReviewDecisions decisions{proposal.Value()};
        return ReviewResult::Success({std::move(proposal).Value(),
                                      std::make_shared<const WorkspaceEditPlan>(std::move(plan).Value()),
                                      std::move(cards),
                                      std::move(decisions),
                                      std::move(offeredPaths),
                                      {},
                                      0});
    }
}  // namespace Horo::Editor
