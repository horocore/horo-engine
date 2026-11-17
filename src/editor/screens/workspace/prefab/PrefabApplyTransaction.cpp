#include "editor/screens/workspace/prefab/PrefabApplyTransaction.h"

#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Editor {
    namespace {
        /** @brief Prevents a borrowed filesystem provider from reentering the source authoring owner. */
        class SourceWriteGuard final {
        public:
            explicit SourceWriteGuard(bool &busy) noexcept : busy_(busy) {
                busy_ = true;
            }

            ~SourceWriteGuard() {
                busy_ = false;
            }

            SourceWriteGuard(const SourceWriteGuard &) = delete;
            SourceWriteGuard &operator=(const SourceWriteGuard &) = delete;

        private:
            bool &busy_;
        };

        /** @brief Owns the exact old bytes and proof of any replacement until all owners commit. */
        struct ApplyFile final {
            std::filesystem::path path;
            Assets::AssetSourceIdentity expected;
            std::string replacement;
            std::optional<ExclusiveFileLock> lock;
            Assets::AssetSourceCapture original;
            std::optional<Assets::AssetSourceIdentity> installed;
        };

        /** @brief Admits both canonical file destinations before either is changed. */
        Result<void> CaptureFiles(std::array<ApplyFile, 2> &files, const std::filesystem::path &root, DurableFileSystem &storage,
                                  const CancellationToken &cancellation) {
            for (auto &file : files) {
                const auto lockPath = Assets::AssetSourcePublicationLockPath(root, file.path);
                if (lockPath.HasError())
                    return Result<void>::Failure(lockPath.ErrorValue());
                auto lock = storage.TryAcquireExclusive(lockPath.Value(), "prefab.apply");
                if (lock.HasError())
                    return Result<void>::Failure(std::move(lock).ErrorValue());
                file.lock.emplace(std::move(lock).Value());
                auto current = Assets::CaptureAssetSource(root, file.path, 64U * 1024U * 1024U, cancellation);
                if (current.HasError())
                    return Result<void>::Failure(std::move(current).ErrorValue());
                if (!file.expected.exists || current.Value().identity != file.expected)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                file.original = std::move(current).Value();
            }
            return Result<void>::Success();
        }

        /** @brief Publishes the tracked replacement while retaining post-rename failure evidence. */
        Result<void> PublishFile(ApplyFile &file, const std::filesystem::path &root, DurableFileSystem &storage,
                                 const CancellationToken &cancellation) {
            auto receipt = Assets::PublishAssetSource(root, file.path, file.expected, std::as_bytes(std::span{file.replacement}),
                                                      *file.lock, storage, cancellation);
            if (receipt.HasError())
                return Result<void>::Failure(std::move(receipt).ErrorValue());
            file.installed = receipt.Value().identity;
            if (receipt.Value().durabilityError)
                return Result<void>::Failure(std::move(*receipt.Value().durabilityError));
            if (receipt.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }

        /** @brief Checks both completed receipts again before publishing either authored owner. */
        Result<void> VerifyInstalledFiles(const std::array<ApplyFile, 2> &files, const std::filesystem::path &root,
                                          const CancellationToken &cancellation) {
            for (const auto &file : files) {
                auto current = Assets::CaptureAssetSource(root, file.path, 64U * 1024U * 1024U, cancellation);
                if (current.HasError())
                    return Result<void>::Failure(std::move(current).ErrorValue());
                if (current.Value().identity != *file.installed)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }

        /** @brief Compensates only still-owned installed bytes and leaves explicit evidence on uncertainty. */
        void Compensate(std::array<ApplyFile, 2> &files, const std::filesystem::path &root, DurableFileSystem &storage,
                        PrefabApplyOutcome &outcome, std::array<Error, 2> &fallback) noexcept {
            for (std::size_t index = files.size(); index > 0; --index) {
                auto &file = files[index - 1];
                if (!file.installed)
                    continue;
                auto failure = std::move(fallback[index - 1]);
                std::optional<Assets::AssetSourceIdentity> restoredUnconfirmed;
                try {
                    auto restored = Assets::PublishAssetSource(root, file.path, *file.installed, file.original.bytes, *file.lock, storage);
                    if (restored.HasValue() && restored.Value().status == Assets::AssetSourcePublicationStatus::Durable)
                        continue;
                    if (restored.HasError())
                        failure = std::move(restored).ErrorValue();
                    else {
                        restoredUnconfirmed = restored.Value().identity;
                        if (restored.Value().durabilityError)
                            failure = std::move(*restored.Value().durabilityError);
                    }
                } catch (...) {
                    // The preallocated recovery error preserves the original attempt failure.
                }
                outcome.recovery[index - 1].emplace(std::move(file.path), std::move(file.original), *file.installed, restoredUnconfirmed,
                                                    std::move(failure));
            }
        }
    }  // namespace

    /** @copydoc PrefabApplyTransaction::Execute */
    PrefabApplyOutcome PrefabApplyTransaction::Execute(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                       const PrefabApplyTransactionRequest &request,
                                                       const Assets::AssetRegistrySnapshot &registry, const PrefabHierarchySources &sources,
                                                       std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas,
                                                       const Prefab::PrefabLimitProfile &limits,
                                                       const ProjectMutationCoordinator &mutations, DurableFileSystem &storage) {
        PrefabApplyOutcome outcome;
        if (auto admitted = prefab.Admit(request.source); admitted.HasError()) {
            outcome.error = std::move(admitted).ErrorValue();
            return outcome;
        }
        std::array<Error, 2> rollbackFailure{MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed),
                                             MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)};
        auto exception = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply publication failed.");
        std::array<ApplyFile, 2> files{{{prefab.path_, prefab.accepted_}, {request.scenePath, request.sceneIdentity}}};
        auto project = mutations.TryAcquire({prefab.root_, ProjectMutationOwner::Save, "prefab.apply"});
        if (project.HasError()) {
            outcome.error = std::move(project).ErrorValue();
            return outcome;
        }
        const auto fail = [&outcome](Error error) {
            outcome.error = std::move(error);
        };
        try {
            const auto run = [&]() -> Result<void> {
                if (request.scenePath == prefab.path_)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DocumentInvalid));
                if (auto current = sources.inputs.VerifyUnchanged(request.scene.cancellation); current.HasError())
                    return current;
                const auto commitments = sources.resolver.CanonicalSourceCommitments();
                const auto base =
                    std::ranges::find(commitments, prefab.source_->Data().assetId, &Prefab::PrefabCanonicalSourceCommitment::asset);
                if (base == commitments.end() || base->digest != prefab.state_)
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::OverrideSourceStale));
                const auto snapshot = scene.Snapshot();
                if (snapshot.HasError())
                    return Result<void>::Failure(snapshot.ErrorValue());
                const auto &current = snapshot.Value();
                SceneDocumentSnapshot view{{current.revision}, {current.state}, current.objects, current.prefabInstances};
                auto content = PreparePrefabApply(view, request.selection, registry, sources.resolver, std::move(schemas), limits,
                                                  request.scene.cancellation);
                if (content.HasError())
                    return Result<void>::Failure(std::move(content).ErrorValue());
                auto prepared = std::move(content).Value();
                auto sceneState = scene.PreparePrefabValues(request.scene, prepared.placements, request.selection.expectedReplacementEpoch);
                if (sceneState.HasError())
                    return Result<void>::Failure(std::move(sceneState).ErrorValue());
                auto sourceState = prefab.PrepareApply(request.source, std::move(prepared.source));
                if (sourceState.HasError())
                    return Result<void>::Failure(std::move(sourceState).ErrorValue());
                auto preparedSource = std::move(sourceState).Value();
                auto sourceBytes = preparedSource.source->SerializeCanonical();
                if (sourceBytes.HasError())
                    return Result<void>::Failure(std::move(sourceBytes).ErrorValue());
                files[0].replacement = std::move(sourceBytes).Value();
                files[1].replacement = SceneSource::EncodeSceneSource({view.objects, prepared.placements});
                {
                    const SourceWriteGuard guard(prefab.busy_);
                    if (auto captured = CaptureFiles(files, prefab.root_, storage, request.scene.cancellation); captured.HasError())
                        return captured;
                    for (auto &file : files)
                        if (auto published = PublishFile(file, prefab.root_, storage, request.scene.cancellation); published.HasError())
                            return published;
                    if (auto current = VerifyInstalledFiles(files, prefab.root_, request.scene.cancellation); current.HasError())
                        return current;
                }
                if (auto valid = scene.ValidatePrepared(*sceneState.Value()); valid.HasError())
                    return valid;
                if (auto valid = prefab.Admit(request.source); valid.HasError())
                    return valid;
                auto resultingScene = scene.PreparedContext(*sceneState.Value());
                resultingScene.cancellation = {};
                prefab.PublishApply(preparedSource, *files[0].installed);
                outcome.committed = scene.PublishPrepared(*sceneState.Value());
                outcome.sceneIdentity = *files[1].installed;
                return scene.MarkSaved(resultingScene);
            };
            if (auto applied = run(); applied.HasError())
                fail(std::move(applied).ErrorValue());
        } catch (...) {
            fail(std::move(exception));
        }
        if (!outcome.committed) {
            const SourceWriteGuard guard(prefab.busy_);
            Compensate(files, prefab.root_, storage, outcome, rollbackFailure);
        } else
            prefab.ReleaseStage();
        return outcome;
    }
}  // namespace Horo::Editor
