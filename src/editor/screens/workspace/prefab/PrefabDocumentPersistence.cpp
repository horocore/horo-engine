#include "Horo/Editor/SourceDocumentService.h"
#include "Horo/Editor/WorkspacePanelHost.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <chrono>

namespace Horo::Editor {
    namespace {
        /** @brief Blocks reentrant source operations while native persistence invokes borrowed services. */
        struct PublicationGuard final {
            bool &active;

            explicit PublicationGuard(bool &value) : active(value) {
                active = true;
            }

            ~PublicationGuard() {
                active = false;
            }
        };
    }  // namespace

    /** @copydoc PrefabDocumentSession::Save */
    Result<Assets::AssetSourcePublication> PrefabDocumentSession::Save(const PrefabDocumentFence fence,
                                                                       const ProjectMutationCoordinator &mutations,
                                                                       DurableFileSystem &files) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return Result<Assets::AssetSourcePublication>::Failure(admitted.ErrorValue());
        if (conflict_)
            return Result<Assets::AssetSourcePublication>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
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

    /** @copydoc PrefabDocumentSession::Autosave */
    Result<void> PrefabDocumentSession::Autosave(const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return admitted;
        if (state_ == saved_)
            return Result<void>::Success();
        auto encoded = source_->SerializeCanonical();
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
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
        const auto bytes = std::as_bytes(std::span(encoded.Value()));
        record.payload.assign(bytes.begin(), bytes.end());
        PublicationGuard guard(busy_);
        DocumentRecoveryService recovery(root_, mutations, files);
        auto result = acceptedRecovery_ ? recovery.ReplaceAccepted(record, *acceptedRecovery_) : recovery.Write(record);
        if (result.HasValue()) {
            recoveryRevision_ = revision_;
            acceptedRecovery_.reset();
        }
        return result;
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
        const bool accepted = acceptedRecovery_ && *record.Value() == *acceptedRecovery_;
        if (!owned && !accepted)
            return Result<void>::Success();
        auto discarded = recovery.Discard(*record.Value());
        if (discarded.HasValue()) {
            acceptedRecovery_.reset();
            recoveryRevision_ = 0;
        }
        return discarded;
    }

    /** @copydoc PrefabDocumentSession::Recover */
    Result<void> PrefabDocumentSession::Recover(const PrefabDocumentFence fence, const DocumentRecoveryRecord &record,
                                                const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (auto admitted = Admit(fence); admitted.HasError())
            return admitted;
        if (state_ != saved_)
            return Result<void>::Failure(MakeError(WorkspaceDocumentErrors::DirtyDocument));
        if (record.asset != source_->Data().assetId || record.document != identity_.key || record.acceptedSource != accepted_ ||
            record.payload.empty())
            return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        const std::string_view text{reinterpret_cast<const char *>(record.payload.data()), record.payload.size()};
        auto parsed = Prefab::PrefabDocument::Parse(text, limits_, {.expectedProjectVersion = source_->Data().projectVersion});
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        auto encoded = parsed.Value().SerializeCanonical();
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (parsed.Value().Data().assetId != record.asset || ComputeSha256(std::as_bytes(std::span(encoded.Value()))) != record.state)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DocumentInvalid));
        auto replacement = std::make_shared<const Prefab::PrefabDocument>(std::move(parsed).Value());
        std::optional<DocumentRecoveryRecord> acceptedRecord{record};
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
        history_.Clear();
        ++revision_;
        recoveryRevision_ = revision_;
        ReleaseStage();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
