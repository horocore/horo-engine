#include "ProposedPatchInternal.h"

#include <algorithm>
#include <map>
#include <new>
#include <type_traits>

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
                } else if (std::get<SourcePathState>(observation.evidence) > SourcePathState::Unavailable) {
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
                case SourcePathState::ParentMissing:
                case SourcePathState::Redirect:
                case SourcePathState::PortableAlias:
                case SourcePathState::Unavailable:
                    return PlanConflict{path, PlanConflictReason::SourceUnavailable};
            }
            return PlanConflict{path, PlanConflictReason::SourceUnavailable};
        }

        /** @brief Requires affirmative absence evidence for creates and rename destinations. */
        std::optional<PlanConflict> Destination(const ObservationIndex &index, const SourceDocumentId &path) {
            const auto *observation = Find(index, path);
            if (!observation)
                return PlanConflict{path, PlanConflictReason::Unobserved};
            const auto *state = std::get_if<SourcePathState>(&observation->evidence);
            if (!state)
                return PlanConflict{path, PlanConflictReason::DestinationOccupied};
            using enum PlanConflictReason;
            switch (*state) {
                case SourcePathState::Absent:
                    return observation->path == path ? std::nullopt : std::optional{PlanConflict{path, Unobserved}};
                case SourcePathState::ParentMissing:
                    return PlanConflict{path, DestinationParentMissing};
                case SourcePathState::Redirect:
                    return PlanConflict{path, DestinationRedirect};
                case SourcePathState::PortableAlias:
                    return PlanConflict{path, DestinationAlias};
                case SourcePathState::Unavailable:
                    return PlanConflict{path, DestinationUnavailable};
                default:
                    return PlanConflict{path, DestinationOccupied};
            }
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
        std::string Render(const std::string_view text, const std::span<const TextHunk> hunks, const std::size_t bytes,
                           const std::span<const ReviewState> selection = {}) {
            std::string result;
            result.reserve(bytes);
            std::size_t copied{};
            for (std::size_t index = 0; index < hunks.size(); ++index) {
                if (!selection.empty() && selection[index] != ReviewState::Accepted)
                    continue;
                const auto &hunk = hunks[index];
                const auto offset = static_cast<std::size_t>(hunk.offset);
                result.append(text.substr(copied, offset - copied));
                result.append(hunk.after);
                copied = offset + hunk.before.size();
            }
            result.append(text.substr(copied));
            return result;
        }

        /** @brief Estimates only the subset the user accepted, in original source coordinates. */
        std::size_t AcceptedSize(const std::size_t sourceBytes, const EditFile &file, const std::span<const ReviewState> selection) {
            auto bytes = sourceBytes;
            for (std::size_t index = 0; index < selection.size(); ++index) {
                if (selection[index] != ReviewState::Accepted)
                    continue;
                const auto &hunk = file.hunks[index];
                bytes = bytes - hunk.before.size() + hunk.after.size();
            }
            return bytes;
        }

        /** @brief Validates the accepted operation and reserves its output before reconstructing text. */
        Result<std::string> RenderAcceptedText(const ProjectPolicy &policy, const FileOperation &file, const std::string_view text,
                                               const std::span<const ReviewState> selection, Budget &budget) {
            using Output = Result<std::string>;
            const auto *edit = std::get_if<EditFile>(&file);
            if (!edit)
                return Output::Failure(MakeError(Errors::Invalid));
            if (!policy.Allows(edit->base.path))
                return Output::Failure(MakeError(Errors::PolicyDenied));
            const auto bytes = AcceptedSize(text.size(), *edit, selection);
            if (!budget.Charge(bytes, budget.output))
                return Output::Failure(MakeError(Errors::Bounds));
            return Output::Success(Render(text, edit->hunks, bytes, selection));
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
            if (!operation.destination && snapshot.Dirty())
                return Entry::Success(PlanConflict{path, PlanConflictReason::DirtyDelete});
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
                if (operation.base && PatchDetail::SamePortablePath(operation.base->path, *operation.destination))
                    return PlanConflict{*operation.destination, PlanConflictReason::CaseOnlyRename};
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

    /** @copydoc ReviewDecisions::BuildAudit */
    Result<std::shared_ptr<SourceReviewAudit>> ReviewDecisions::BuildAudit() const {
        using Output = Result<std::shared_ptr<SourceReviewAudit>>;
        auto redacted = Audit(proposal_);
        if (redacted.HasError())
            return Output::Failure(redacted.ErrorValue());
        auto audit = std::make_shared<SourceReviewAudit>();
        audit->proposal = std::move(redacted).Value();
        const auto decisions = Encode();
        audit->decisionDigest = ComputeSha256(std::as_bytes(std::span{decisions}));
        audit->choices = units_;
        return Output::Success(std::move(audit));
    }

    /** @copydoc ReviewDecisions::PrepareFileOperation */
    Result<std::optional<PreparedSourceFileOperation>> ReviewDecisions::PrepareFileOperation(const ProjectPolicy &policy,
                                                                                             const WorkspaceEditPlan &plan,
                                                                                             const CancellationToken cancellation) const {
        using Output = Result<std::optional<PreparedSourceFileOperation>>;
        std::optional<std::size_t> structural;
        std::size_t accepted{};
        for (std::size_t index = 0; index < units_.size(); ++index) {
            if (std::ranges::find(units_[index], ReviewState::Accepted) == units_[index].end())
                continue;
            ++accepted;
            const auto &file = proposal_.Files()[index];
            if (std::holds_alternative<CreateFile>(file) || std::holds_alternative<DeleteFile>(file))
                structural = index;
        }
        if (!structural)
            return Output::Success(std::nullopt);
        if (accepted != 1 || proposal_.PolicyDigest() != policy.Digest())
            return Output::Failure(MakeError(Errors::PolicyDenied));
        try {
            if (cancellation.IsCancellationRequested())
                return Output::Failure(MakeError(SourceDocumentErrors::Cancelled));
            if (!Matches(plan) || !CanAccept(plan, *structural))
                return Output::Failure(MakeError(Errors::StaleBase));
            auto attributed = BuildAudit();
            if (attributed.HasError())
                return Output::Failure(attributed.ErrorValue());
            auto audit = std::move(attributed).Value();
            const auto ordinal = static_cast<std::uint32_t>(*structural);
            if (const auto *file = std::get_if<CreateFile>(&proposal_.Files()[*structural])) {
                audit->revisions.push_back({ordinal, 0, 1});
                return Output::Success(PreparedSourceFileOperation{PreparedSourceCreate{*file, std::move(audit)}});
            }
            const auto &source = std::get<FilePreview>(plan.entries[*structural]).before;
            audit->revisions.push_back({ordinal, source.Revision(), 0});
            return Output::Success(PreparedSourceFileOperation{PreparedSourceDelete{source, std::move(audit), {}}});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Errors::Bounds));
        }
    }

    /** @copydoc ReviewDecisions::PrepareAccepted */
    Result<PreparedSourceReview> ReviewDecisions::PrepareAccepted(const ProjectPolicy &policy, const WorkspaceEditPlan &plan,
                                                                  const CancellationToken cancellation) const {
        using Output = Result<PreparedSourceReview>;
        if (cancellation.IsCancellationRequested())
            return Output::Failure(MakeError(SourceDocumentErrors::Cancelled));
        if (proposal_.PolicyDigest() != policy.Digest())
            return Output::Failure(MakeError(Errors::PolicyDenied));
        if (!Matches(plan) || !AcceptancesCurrent(plan, units_))
            return Output::Failure(MakeError(Errors::StaleBase));
        try {
            PreparedSourceReview prepared;
            prepared.edits_.reserve(units_.size());
            auto attributed = BuildAudit();
            if (attributed.HasError())
                return Output::Failure(attributed.ErrorValue());
            auto audit = std::move(attributed).Value();
            audit->revisions.reserve(units_.size());
            const Limits limits;
            Budget budget{limits, ReviewSourceMatch::CurrentRevision};
            for (std::size_t file = 0; file < units_.size(); ++file) {
                if (cancellation.IsCancellationRequested())
                    return Output::Failure(MakeError(SourceDocumentErrors::Cancelled));
                const auto &selection = units_[file];
                if (std::ranges::find(selection, ReviewState::Accepted) == selection.end())
                    continue;
                const auto &source = std::get<FilePreview>(plan.entries[file]).before;
                auto replacement = RenderAcceptedText(policy, proposal_.Files()[file], source.Text(), selection, budget);
                if (replacement.HasError())
                    return Output::Failure(replacement.ErrorValue());
                prepared.edits_.push_back({source, std::move(replacement).Value()});
                audit->revisions.push_back({static_cast<std::uint32_t>(file), source.Revision(), 0});
            }
            if (prepared.edits_.empty())
                return Output::Failure(MakeError(Errors::Invalid));
            prepared.audit_ = std::move(audit);
            return Output::Success(std::move(prepared));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Errors::Bounds));
        }
    }

    /** @copydoc PreparedSourceReview::ObserveDisk */
    Result<void> PreparedSourceReview::ObserveDisk(const std::filesystem::path &root, const DurableFileSystem &files,
                                                   const CancellationToken cancellation) {
        disk_ = {};
        try {
            std::vector<SourceDocumentSnapshot> sources;
            sources.reserve(edits_.size());
            for (const auto &edit : edits_)
                sources.push_back(edit.source);
            auto observed = SourceBatchDiskEvidence::Capture(root, sources, files, cancellation);
            if (observed.HasError())
                return Result<void>::Failure(observed.ErrorValue());
            disk_ = std::move(observed).Value();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Errors::Bounds));
        }
    }

    /** @copydoc PreparedSourceReview::Publish */
    Result<SourceEditBatch> PreparedSourceReview::Publish(SourceDocumentService &documents, const DurableFileSystem *files,
                                                          const CancellationToken cancellation) const {
        try {
            std::vector<SourceBatchEdit> edits;
            edits.reserve(edits_.size());
            for (const auto &edit : edits_)
                edits.push_back({edit.source, {edit.source.Revision(), 0, edit.source.Text().size(), edit.replacement}});
            if (files)
                return documents.EditBatchWithEvidence(edits, disk_, *files, cancellation);
            return documents.EditBatch(edits, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<SourceEditBatch>::Failure(MakeError(Errors::Bounds));
        }
    }

    /** @copydoc PreparedSourceReview::Commit */
    Result<SourceEditBatch> PreparedSourceReview::Commit(SourceDocumentService &documents, const CancellationToken cancellation) const {
        return Publish(documents, nullptr, cancellation);
    }

    /** @copydoc PreparedSourceReview::CommitObserved */
    Result<SourceReviewApplied> PreparedSourceReview::CommitObserved(SourceDocumentService &documents, const DurableFileSystem &files,
                                                                     const CancellationToken cancellation) const {
        using Output = Result<SourceReviewApplied>;
        static_assert(std::is_nothrow_move_constructible_v<Output>);
        try {
            if (!audit_)
                return Output::Failure(MakeError(Errors::Invalid));
            auto audit = std::make_shared<SourceReviewAudit>(*audit_);
            auto applied = Publish(documents, &files, cancellation);
            if (applied.HasError())
                return Output::Failure(std::move(applied).ErrorValue());
            auto batch = std::move(applied).Value();
            const auto items = batch.Items();
            for (std::size_t index = 0; index < items.size(); ++index)
                audit->revisions[index].after = items[index].snapshot.Revision();
            return Output::Success({std::move(batch), std::move(audit)});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(Errors::Bounds));
        }
    }

    /** @copydoc ReviewDecisions::ApplyAccepted */
    Result<SourceEditBatch> ReviewDecisions::ApplyAccepted(SourceDocumentService &documents, const ProjectPolicy &policy,
                                                           const WorkspaceEditPlan &plan, const CancellationToken cancellation) const {
        const auto prepared = PrepareAccepted(policy, plan, cancellation);
        if (prepared.HasError())
            return Result<SourceEditBatch>::Failure(prepared.ErrorValue());
        return prepared.Value().Commit(documents, cancellation);
    }
}  // namespace Horo::Editor::Proposals
