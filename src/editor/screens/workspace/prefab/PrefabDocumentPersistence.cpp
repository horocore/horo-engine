#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <chrono>
#include <map>

namespace Horo::Editor {
    namespace {
        /** @brief Blocks reentrant source operations while native persistence invokes borrowed services. */
        struct PublicationGuard final {
            bool *active;

            explicit PublicationGuard(bool &value) : active(&value) {
                *active = true;
            }

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;

            ~PublicationGuard() {
                *active = false;
            }
        };
    }  // namespace

    /** @copydoc ComparePrefabExternal */
    Result<std::shared_ptr<const PrefabExternalComparison>> ComparePrefabExternal(const PrefabExternalInspection &request,
                                                                                  const CancellationToken &cancellation) {
        using Return = Result<std::shared_ptr<const PrefabExternalComparison>>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(JobCancelled().ErrorValue());
        const auto captured = Assets::CaptureAssetSource(request.root, request.path, Prefab::PrefabHardLimits::SourceDocumentBytes);
        if (captured.HasError()) {
            if (request.previous && request.previous->diskError &&
                request.previous->diskError->code.Value() == captured.ErrorValue().code.Value())
                return Return::Success(request.previous);
            return Return::Success(
                std::make_shared<const PrefabExternalComparison>(PrefabExternalComparison{.diskError = captured.ErrorValue()}));
        }
        if (captured.Value().identity == request.accepted)
            return Return::Success({});
        if (request.previous && request.previous->observed == captured.Value().identity)
            return Return::Success(request.previous);
        PrefabExternalComparison result{.observed = captured.Value().identity};
        if (!result.observed.exists) {
            result.diskError = MakeError(Assets::AssetSourceErrors::ReadFailed);
            return Return::Success(std::make_shared<const PrefabExternalComparison>(std::move(result)));
        }
        const auto &draft = request.draft->Data();
        const auto &bytes = captured.Value().bytes;
        const auto disk = Prefab::PrefabDocument::Parse({reinterpret_cast<const char *>(bytes.data()), bytes.size()}, request.limits,
                                                        {.expectedProjectVersion = draft.projectVersion});
        if (disk.HasError())
            result.diskError = disk.ErrorValue();
        else if (disk.Value().Data().assetId != draft.assetId)
            result.diskError = MakeError(Prefab::PrefabErrors::IdentityInvalid);
        if (result.diskError)
            return Return::Success(std::make_shared<const PrefabExternalComparison>(std::move(result)));
        const auto &external = disk.Value().Data();
        result.compositionChanged = draft.composition != external.composition || draft.referencedAssets != external.referencedAssets;
        std::map<Prefab::LocalObjectId, std::pair<const Prefab::PrefabObjectNode *, const Prefab::PrefabObjectNode *>> objects;
        for (const auto &object : draft.objects)
            objects[object.localId].first = &object;
        for (const auto &object : external.objects)
            objects[object.localId].second = &object;
        for (const auto &[id, sides] : objects) {
            if (cancellation.IsCancellationRequested())
                return Return::Failure(JobCancelled().ErrorValue());
            const auto &[local, stored] = sides;
            if (local && stored && *local == *stored)
                continue;
            const auto change =
                local ? (stored ? PrefabExternalChange::Modified : PrefabExternalChange::DraftOnly) : PrefabExternalChange::DiskOnly;
            PrefabExternalMemberDifference difference{id, change, local ? local->name : std::string{},
                                                      stored ? stored->name : std::string{}};
            if (local && stored) {
                difference.parentChanged = local->parentLocalId != stored->parentLocalId;
                difference.transformChanged = local->localTransform != stored->localTransform;
                difference.payloadChanged = local->components != stored->components || local->behaviors != stored->behaviors;
            }
            result.members.push_back(std::move(difference));
        }
        return Return::Success(std::make_shared<const PrefabExternalComparison>(std::move(result)));
    }

    /** @copydoc PrefabDocumentSession::Save */
    Result<Assets::AssetSourcePublication> PrefabDocumentSession::Save(const PrefabDocumentFence &fence,
                                                                       const ProjectMutationCoordinator &mutations,
                                                                       DurableFileSystem &files) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(admitted.ErrorValue());
        if (conflict_)
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        if (const auto validated = source_->ValidateLimits(limits_); validated.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(validated.ErrorValue());
        auto encoded = source_->SerializeCanonical();
        if (encoded.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(encoded.ErrorValue());
        PublicationGuard guard(busy_);
        auto project = mutations.TryAcquire({root_, ProjectMutationOwner::Save, "prefab.source.save"});
        if (project.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(project.ErrorValue());
        const auto heldProject = std::move(project).Value();
        auto lock = Assets::AssetSourcePublicationLockPath(root_, path_);
        if (lock.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(lock.ErrorValue());
        auto lease = files.TryAcquireExclusive(lock.Value(), "prefab.source.save");
        if (lease.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(lease.ErrorValue());
        auto published =
            Assets::PublishAssetSource(root_, path_, accepted_, std::as_bytes(std::span(encoded.Value())), lease.Value(), files);
        if (published.HasError()) {
            if (published.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value())
                conflict_ = true;
            return published;
        }
        if (published.Value().status == Assets::AssetSourcePublicationStatus::Durable) {
            accepted_ = published.Value().identity;
            saved_ = state_;
        } else
            conflict_ = true;
        return published;
    }

    /** @copydoc PrefabDocumentSession::CaptureRecovery */
    Result<std::optional<PrefabRecoveryCheckpoint>> PrefabDocumentSession::CaptureRecovery() const {
        using Return = Result<std::optional<PrefabRecoveryCheckpoint>>;
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return Return::Failure(admitted.ErrorValue());
        if (state_ == saved_ || (recoveryRevision_ == revision_ && !acceptedRecovery_))
            return Return::Success(std::nullopt);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        DocumentRecoveryRecord record{source_->Data().assetId,
                                      identity_.key,
                                      owner_,
                                      identity_.instance.Value(),
                                      revision_,
                                      static_cast<std::uint64_t>(seconds),
                                      accepted_,
                                      state_,
                                      {}};
        return Return::Success(PrefabRecoveryCheckpoint{std::move(record), source_, limits_, acceptedRecovery_});
    }

    /** @copydoc WritePrefabRecovery */
    Result<std::shared_ptr<const DocumentRecoveryRecord>> WritePrefabRecovery(const PrefabRecoveryCheckpoint &checkpoint,
                                                                              DocumentRecoveryService &recovery,
                                                                              const CancellationToken &cancellation) {
        using Return = Result<std::shared_ptr<const DocumentRecoveryRecord>>;
        if (cancellation.IsCancellationRequested())
            return Return::Failure(JobCancelled().ErrorValue());
        if (auto validated = checkpoint.source->ValidateLimits(checkpoint.limits); validated.HasError())
            return Return::Failure(validated.ErrorValue());
        auto encoded = checkpoint.source->SerializeCanonical();
        if (encoded.HasError())
            return Return::Failure(encoded.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Return::Failure(JobCancelled().ErrorValue());
        auto record = std::make_shared<DocumentRecoveryRecord>(checkpoint.record);
        const auto bytes = std::as_bytes(std::span(encoded.Value()));
        record->payload.assign(bytes.begin(), bytes.end());
        const auto published = checkpoint.acceptedRecovery ? recovery.ReplaceAccepted(*record, *checkpoint.acceptedRecovery, cancellation)
                                                           : recovery.Write(*record, cancellation);
        return published.HasError() ? Return::Failure(published.ErrorValue()) : Return::Success(std::move(record));
    }

    /** @copydoc PrefabDocumentSession::AcknowledgeRecovery */
    bool PrefabDocumentSession::AcknowledgeRecovery(const PrefabRecoveryCheckpoint &checkpoint) noexcept {
        const auto &record = checkpoint.record;
        if (closed_ || thread_ != std::this_thread::get_id() || record.sessionOwner != owner_ ||
            record.sessionGeneration != identity_.instance.Value() || record.document != identity_.key ||
            record.acceptedSource != accepted_ || record.revision < recoveryRevision_ || record.revision > revision_ ||
            checkpoint.acceptedRecovery != acceptedRecovery_)
            return false;
        recoveryRevision_ = record.revision;
        acceptedRecovery_.reset();
        return true;
    }

    /** @copydoc PrefabDocumentSession::Autosave */
    Result<void> PrefabDocumentSession::Autosave(const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        auto captured = CaptureRecovery();
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        if (!captured.Value())
            return Result<void>::Success();
        const auto &checkpoint = *captured.Value();
        PublicationGuard guard(busy_);
        DocumentRecoveryService recovery(root_, mutations, files);
        auto result = WritePrefabRecovery(checkpoint, recovery);
        if (result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        static_cast<void>(AcknowledgeRecovery(checkpoint));
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::DiscardOwnedRecovery */
    Result<void> PrefabDocumentSession::DiscardOwnedRecovery(const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return admitted;
        PublicationGuard guard(busy_);
        DocumentRecoveryService recovery(root_, mutations, files);
        auto record = recovery.Inspect(source_->Data().assetId, identity_.key);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        if (!record.Value())
            return Result<void>::Success();
        const bool owned = record.Value()->sessionOwner == owner_ && record.Value()->sessionGeneration == identity_.instance.Value();
        if (const bool accepted = acceptedRecovery_ && *record.Value() == *acceptedRecovery_; !owned && !accepted)
            return Result<void>::Success();
        auto discarded = recovery.Discard(*record.Value());
        if (discarded.HasValue()) {
            acceptedRecovery_.reset();
            recoveryRevision_ = 0;
        }
        return discarded;
    }

    /** @copydoc PrefabDocumentSession::DiscardRecoveryCandidate */
    Result<void> PrefabDocumentSession::DiscardRecoveryCandidate(const PrefabDocumentFence &fence, const DocumentRecoveryRecord &record,
                                                                 const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (record.asset != source_->Data().assetId || record.document != identity_.key)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        PublicationGuard guard(busy_);
        DocumentRecoveryService recovery(root_, mutations, files);
        if (auto discarded = recovery.Discard(record); discarded.HasError())
            return discarded;
        if ((record.sessionOwner == owner_ && record.sessionGeneration == identity_.instance.Value()) ||
            (acceptedRecovery_ && *acceptedRecovery_ == record)) {
            acceptedRecovery_.reset();
            recoveryRevision_ = 0;
        }
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::Recover */
    Result<void> PrefabDocumentSession::Recover(const PrefabDocumentFence &fence, const DocumentRecoveryRecord &record,
                                                const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (state_ != saved_)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        if (record.asset != source_->Data().assetId || record.document != identity_.key || record.acceptedSource != accepted_ ||
            record.payload.empty())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto text = ReadSourceText(record.payload);
        if (text.HasError())
            return Result<void>::Failure(text.ErrorValue());
        auto parsed = Prefab::PrefabDocument::Parse(text.Value(), limits_, {.expectedProjectVersion = source_->Data().projectVersion});
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        auto encoded = parsed.Value().SerializeCanonical();
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (parsed.Value().Data().assetId != record.asset || ComputeSha256(std::as_bytes(std::span(encoded.Value()))) != record.state)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DocumentInvalid));
        auto replacement = std::make_shared<const Prefab::PrefabDocument>(std::move(parsed).Value());
        auto acceptedRecord = std::make_shared<const DocumentRecoveryRecord>(record);
        PublicationGuard guard(busy_);
        DocumentRecoveryService recovery(root_, mutations, files);
        const auto currentRecord = recovery.Inspect(record.asset, record.document);
        if (currentRecord.HasError())
            return Result<void>::Failure(currentRecord.ErrorValue());
        if (!currentRecord.Value() || *currentRecord.Value() != record)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        auto capture = Assets::CaptureAssetSource(root_, path_, Prefab::PrefabHardLimits::SourceDocumentBytes);
        if (capture.HasError())
            return Result<void>::Failure(capture.ErrorValue());
        if (capture.Value().identity != record.acceptedSource)
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        source_.swap(replacement);
        acceptedRecovery_.swap(acceptedRecord);
        state_ = record.state;
        conflict_ = false;
        externalComparison_.reset();
        keepExternalDraft_ = false;
        history_.Clear();
        ++revision_;
        recoveryRevision_ = revision_;
        ReleaseStage();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
