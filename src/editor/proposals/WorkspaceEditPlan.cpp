#include "ProposedPatchInternal.h"

#include <algorithm>
#include <map>
#include <new>

namespace Horo::Editor::Proposals {
    namespace {
        using PlanResult = Result<WorkspaceEditPlan>;
        using PlanEntry = std::variant<FilePreview, PlanConflict>;
        using ObservationIndex = std::map<std::string, const SourcePathObservation *>;

        /** @brief Presents the source, result and original-byte edits of one canonical operation. */
        struct Operation final {
            const FileBase *base{};
            const SourceDocumentId *destination{};
            std::span<const TextHunk> hunks;
            const std::string *createdText{};
        };

        /** @brief Borrows immutable operation fields without ordering or transforming the edit. */
        Operation Describe(const FileOperation &file) {
            return std::visit([](const auto &value) -> Operation {
                using Kind = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Kind, CreateFile>)
                    return {nullptr, &value.path, {}, &value.text};
                else if constexpr (std::is_same_v<Kind, EditFile>)
                    return {&value.base, &value.base.path, value.hunks};
                else if constexpr (std::is_same_v<Kind, RenameFile>)
                    return {&value.base, &value.destination, value.hunks};
                else
                    return {&value.base, nullptr, {}};
            }, file);
        }

        /** @brief Verifies shape and rejects ambiguous observations for the same portable path. */
        bool IndexObservations(const std::span<const SourcePathObservation> observations, ObservationIndex &index) {
            for (const auto &observation : observations) {
                if (!observation.path.IsValid())
                    return false;
                if (const auto *snapshot = std::get_if<SourceDocumentSnapshot>(&observation.evidence)) {
                    const auto identity = snapshot->Identity();
                    if (identity.key.kind != DocumentKind::Source || identity.key.source != observation.path || !identity.IsValid())
                        return false;
                } else if (std::get<SourcePathState>(observation.evidence) > SourcePathState::Moved) {
                    return false;
                }
                if (!index.emplace(PatchDetail::Fold(observation.path.Value()), &observation).second)
                    return false;
            }
            return true;
        }

        /** @brief Finds the observation for a portable path spelling. */
        const SourcePathObservation *Find(const ObservationIndex &index, const SourceDocumentId &path) {
            const auto it = index.find(PatchDetail::Fold(path.Value()));
            return it == index.end() ? nullptr : it->second;
        }

        /** @brief Resolves a current exact source or reports why the workspace cannot supply it. */
        std::variant<SourceDocumentSnapshot, PlanConflict> Source(const ObservationIndex &index, const SourceDocumentId &path) {
            const auto *observation = Find(index, path);
            if (!observation)
                return PlanConflict{path, PlanConflictReason::Unobserved};
            if (observation->path != path)
                return PlanConflict{path, PlanConflictReason::SourceMoved};
            if (const auto *snapshot = std::get_if<SourceDocumentSnapshot>(&observation->evidence))
                return *snapshot;
            switch (std::get<SourcePathState>(observation->evidence)) {
                case SourcePathState::Absent:
                    return PlanConflict{path, PlanConflictReason::SourceMissing};
                case SourcePathState::Moved:
                    return PlanConflict{path, PlanConflictReason::SourceMoved};
                case SourcePathState::Occupied:
                    return PlanConflict{path, PlanConflictReason::SourceUnavailable};
            }
            return PlanConflict{path, PlanConflictReason::SourceUnavailable};
        }

        /** @brief Requires affirmative absence evidence for creates and rename destinations. */
        std::optional<PlanConflict> Destination(const ObservationIndex &index, const SourceDocumentId &path) {
            const auto *observation = Find(index, path);
            if (!observation)
                return PlanConflict{path, PlanConflictReason::Unobserved};
            if (const auto *state = std::get_if<SourcePathState>(&observation->evidence); state && *state == SourcePathState::Absent) {
                if (observation->path == path)
                    return {};
                return PlanConflict{path, PlanConflictReason::Unobserved};
            }
            return PlanConflict{path, PlanConflictReason::DestinationOccupied};
        }

        /** @brief Checks path ancestry at segment boundaries across the finite proposal. */
        std::optional<PlanConflict> Overlap(const SourceDocumentId &path, const std::vector<std::string> &paths) {
            const auto key = PatchDetail::Fold(path.Value());
            const auto ancestor = [](const std::string_view prefix, const std::string_view nested) {
                return nested.size() > prefix.size() && nested.starts_with(prefix) && nested[prefix.size()] == '/';
            };
            if (std::ranges::any_of(paths, [&](const auto &candidate) {
                return ancestor(key, candidate) || ancestor(candidate, key);
            }))
                return PlanConflict{path, PlanConflictReason::OverlappingPaths};
            return {};
        }

        /** @brief Collects portable collision keys for both sides of every file operation. */
        std::vector<std::string> AffectedPaths(const std::span<const FileOperation> files) {
            std::vector<std::string> paths;
            paths.reserve(2 * files.size());
            for (const auto &file : files) {
                const auto operation = Describe(file);
                if (operation.base)
                    paths.push_back(PatchDetail::Fold(operation.base->path.Value()));
                if (operation.destination)
                    paths.push_back(PatchDetail::Fold(operation.destination->Value()));
            }
            return paths;
        }

        /** @brief Reports source freshness and observed external changes before inspecting text. */
        std::optional<PlanConflictReason> SourceProblem(const SourceDocumentSnapshot &snapshot) {
            if (!snapshot.IsCurrent())
                return PlanConflictReason::SourceStale;
            switch (snapshot.ExternalState()) {
                case SourceExternalState::InSync:
                    return {};
                case SourceExternalState::Removed:
                    return PlanConflictReason::SourceMissing;
                case SourceExternalState::Unreadable:
                    return PlanConflictReason::SourceUnavailable;
                case SourceExternalState::Changed:
                    return PlanConflictReason::SourceChangedOnDisk;
            }
            return PlanConflictReason::SourceUnavailable;
        }

        struct Budget final {
            const Limits &limits;
            ReviewSourceMatch sourceMatch;
            std::size_t input{};
            std::size_t output{};

            bool Charge(const std::size_t bytes, std::size_t &used) const {
                if (bytes > limits.maximumTextBytes || bytes > limits.maximumTotalTextBytes - used)
                    return false;
                used += bytes;
                return true;
            }
        };

        /** @brief Concatenates unchanged spans and replacements in one bounded forward copy. */
        std::string Render(const std::string_view text, const std::span<const TextHunk> hunks, const std::size_t bytes) {
            std::string result;
            result.reserve(bytes);
            std::size_t copied{};
            for (const auto &hunk : hunks) {
                const auto offset = static_cast<std::size_t>(hunk.offset);
                result.append(text.substr(copied, offset - copied));
                result.append(hunk.after);
                copied = offset + hunk.before.size();
            }
            result.append(text.substr(copied));
            return result;
        }

        /** @brief Admits exact source evidence and charges work before hashing source or allocating a preview. */
        Result<PlanEntry> PrepareSource(const Operation &operation, const ObservationIndex &index, Budget &budget) {
            using Entry = Result<PlanEntry>;
            const auto &path = operation.base->path;
            auto source = Source(index, path);
            if (const auto *conflict = std::get_if<PlanConflict>(&source))
                return Entry::Success(*conflict);
            auto snapshot = std::get<SourceDocumentSnapshot>(std::move(source));
            if (const auto problem = SourceProblem(snapshot))
                return Entry::Success(PlanConflict{path, *problem});
            if (!budget.Charge(snapshot.Text().size(), budget.input) || !budget.Charge(snapshot.DiskBase().size(), budget.input))
                return Entry::Failure(MakeError(Errors::Bounds));
            FilePreview preview{std::move(snapshot), {}, budget.sourceMatch};
            const auto validation = preview.ValidateSource(*operation.base, operation.hunks);
            if (validation.HasError()) {
                if (validation.ErrorValue().code.Value() == Errors::Bounds.code.Value())
                    return Entry::Failure(validation.ErrorValue());
                return Entry::Success(PlanConflict{path, PlanConflictReason::SourceStale});
            }
            if (!operation.destination)
                return Entry::Success(std::move(preview));
            const auto text = preview.before.Text();
            auto bytes = text.size();
            for (const auto &hunk : operation.hunks)
                bytes = bytes - hunk.before.size() + hunk.after.size();
            if (!budget.Charge(bytes, budget.output))
                return Entry::Failure(MakeError(Errors::Bounds));
            preview.after = Render(text, operation.hunks, bytes);
            return Entry::Success(std::move(preview));
        }

        /** @brief Rejects conflicting operands before any text is copied. */
        std::optional<PlanConflict> CheckPaths(const Operation &operation, const ObservationIndex &index,
                                               const std::vector<std::string> &paths) {
            if (operation.base) {
                if (auto overlap = Overlap(operation.base->path, paths))
                    return overlap;
            }
            if (operation.destination && (!operation.base || *operation.destination != operation.base->path)) {
                if (auto overlap = Overlap(*operation.destination, paths))
                    return overlap;
                return Destination(index, *operation.destination);
            }
            return {};
        }

        /** @brief Projects one operation, leaving conflicting entries available for review. */
        Result<PlanEntry> Prepare(const Operation &operation, const ObservationIndex &index, const std::vector<std::string> &paths,
                                  Budget &budget) {
            using Entry = Result<PlanEntry>;
            if (operation.hunks.size() > budget.limits.maximumHunksPerFile)
                return Entry::Failure(MakeError(Errors::Bounds));
            if (auto conflict = CheckPaths(operation, index, paths))
                return Entry::Success(std::move(*conflict));
            if (operation.base)
                return PrepareSource(operation, index, budget);
            if (!budget.Charge(operation.createdText->size(), budget.output))
                return Entry::Failure(MakeError(Errors::Bounds));
            return Entry::Success(FilePreview{{}, *operation.createdText});
        }

        /** @brief Drops a preview whose retained source was superseded while planning the batch. */
        void Refresh(PlanEntry &entry) {
            const auto *preview = std::get_if<FilePreview>(&entry);
            if (preview && preview->before.Revision() && !preview->before.IsCurrent())
                entry = PlanConflict{preview->before.Identity().key.source, PlanConflictReason::SourceStale};
        }

        /** @brief Builds a read-only plan using the evidence required by the caller's review flow. */
        PlanResult Plan(const ChangeSet &proposal, const ProjectPolicy &policy, const std::span<const SourcePathObservation> observations,
                        const Limits &limits, const ReviewSourceMatch sourceMatch) {
            if (!limits.IsValid() || proposal.Files().size() > limits.maximumFiles || observations.size() > 2 * Limits{}.maximumFiles)
                return PlanResult::Failure(MakeError(Errors::Bounds));
            if (proposal.PolicyDigest() != policy.Digest())
                return PlanResult::Failure(MakeError(Errors::PolicyDenied));
            try {
                ObservationIndex index;
                if (!IndexObservations(observations, index))
                    return PlanResult::Failure(MakeError(Errors::Invalid));
                const auto paths = AffectedPaths(proposal.Files());
                Budget budget{limits, sourceMatch};
                WorkspaceEditPlan plan{proposal.Digest(), {}};
                plan.entries.reserve(proposal.Files().size());
                for (const auto &file : proposal.Files()) {
                    auto entry = Prepare(Describe(file), index, paths, budget);
                    if (entry.HasError())
                        return PlanResult::Failure(entry.ErrorValue());
                    plan.entries.push_back(std::move(entry).Value());
                }
                for (auto &entry : plan.entries)
                    Refresh(entry);
                return PlanResult::Success(std::move(plan));
            } catch (const std::bad_alloc &) {
                return PlanResult::Failure(MakeError(Errors::Bounds));
            }
        }
    }  // namespace

    /** @copydoc PlanWorkspaceEdits */
    Result<WorkspaceEditPlan> PlanWorkspaceEdits(const ChangeSet &proposal, const ProjectPolicy &policy,
                                                 const std::span<const SourcePathObservation> observations, const Limits &limits) {
        return Plan(proposal, policy, observations, limits, ReviewSourceMatch::CurrentRevision);
    }

    /** @copydoc PlanRecoveredWorkspaceEdits */
    Result<WorkspaceEditPlan> PlanRecoveredWorkspaceEdits(const ChangeSet &proposal, const ProjectPolicy &policy,
                                                          const std::span<const SourcePathObservation> observations, const Limits &limits) {
        return Plan(proposal, policy, observations, limits, ReviewSourceMatch::RecoveredContent);
    }
}  // namespace Horo::Editor::Proposals
