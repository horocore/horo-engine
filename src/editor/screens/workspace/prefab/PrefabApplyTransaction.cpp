#include "editor/screens/workspace/prefab/PrefabApplyTransaction.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <array>

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
        auto admitted = prefab.Admit(request.source);
        if (admitted.HasError())
            return {.error = std::move(admitted).ErrorValue()};
        auto project = mutations.TryAcquire({prefab.root_, ProjectMutationOwner::Save, "prefab.apply"});
        if (project.HasError())
            return {.error = std::move(project).ErrorValue()};
        try {
            if (auto current = sources.inputs.VerifyUnchanged(request.scene.cancellation); current.HasError())
                return {.error = std::move(current).ErrorValue()};
            const auto commitments = sources.resolver.CanonicalSourceCommitments();
            const auto base =
                std::ranges::find(commitments, prefab.source_->Data().assetId, &Prefab::PrefabCanonicalSourceCommitment::asset);
            if (base == commitments.end() || base->digest != prefab.state_)
                return {.error = MakeError(Prefab::PrefabErrors::OverrideSourceStale)};
            const auto snapshot = scene.Snapshot();
            if (snapshot.HasError())
                return {.error = snapshot.ErrorValue()};
            const auto &current = snapshot.Value();
            SceneDocumentSnapshot view{{current.revision}, {current.state}, current.objects, current.prefabInstances};
            auto content = PreparePrefabApply(view, request.selection, registry, sources.resolver, std::move(schemas), limits,
                                              request.scene.cancellation);
            if (content.HasError())
                return {.error = std::move(content).ErrorValue()};
            auto prepared = std::move(content).Value();
            auto sceneState = scene.PreparePrefabValues(request.scene, prepared.placements, request.selection.expectedReplacementEpoch);
            if (sceneState.HasError())
                return {.error = std::move(sceneState).ErrorValue()};
            const auto next = scene.PreparedContext(*sceneState.Value());
            const PrefabApplyHistoryGroup group{next.session.owner, next.session.generation, next.state};
            auto sourceState = prefab.PrepareApply(request.source, std::move(prepared.source), group);
            if (sourceState.HasError())
                return {.error = std::move(sourceState).ErrorValue()};
            auto preparedSource = std::move(sourceState).Value();
            return Publish(scene, prefab, *sceneState.Value(), preparedSource,
                           {request.scene, request.source, request.scenePath, request.sceneIdentity, group}, storage);
        } catch (...) {
            return {.error = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply preparation failed.")};
        }
    }

    /** @copydoc PrefabApplyTransaction::NextSceneGroup */
    Result<std::optional<PrefabApplyHistoryGroup>> PrefabApplyTransaction::NextSceneGroup(const SceneDocumentTransactions &scene,
                                                                                          const SceneTransactionContext &context,
                                                                                          const bool redo) {
        auto state = scene.NextPrefabValueState(context, redo);
        if (state.HasError())
            return Result<std::optional<PrefabApplyHistoryGroup>>::Failure(std::move(state).ErrorValue());
        if (!state.Value())
            return Result<std::optional<PrefabApplyHistoryGroup>>::Success(std::nullopt);
        return Result<std::optional<PrefabApplyHistoryGroup>>::Success(
            PrefabApplyHistoryGroup{context.session.owner, context.session.generation, *state.Value()});
    }

    /** @copydoc PrefabApplyTransaction::ReconcileHistory */
    void PrefabApplyTransaction::ReconcileHistory(SceneDocumentTransactions &scene, const SceneTransactionContext &context,
                                                  const std::span<const std::unique_ptr<PrefabDocumentSession>> sources) {
        if (scene.Admit(context).HasError() || std::ranges::any_of(sources, [](const auto &source) {
            return source->busy_;
        }))
            return;
        const std::function<bool(std::uint64_t, bool)> unavailable = [&](const std::uint64_t state, const bool redo) {
            const PrefabApplyHistoryGroup group{context.session.owner, context.session.generation, state};
            return std::ranges::none_of(sources, [&](const auto &source) {
                return !source->closed_ && source->history_.Contains(redo, [&](const auto &entry) {
                    return entry.applyGroup == group;
                });
            });
        };
        bool retired;
        do {
            retired = scene.RetirePrefabValueHistory(unavailable);
            for (const auto &source : sources) {
                const bool retiredSource = source->history_.RetireUnavailable([&](const auto &entry, const bool redo) {
                    const auto &group = entry.applyGroup;
                    return group && (group->sceneOwner != context.session.owner || group->sceneOpening != context.session.generation ||
                                     !scene.HasPrefabValueHistory(group->sceneState, redo));
                });
                retired = retired || retiredSource;
            }
        } while (retired);
    }

    /** @copydoc PrefabApplyTransaction::InspectSource */
    Result<void> PrefabApplyTransaction::InspectSource(PrefabDocumentSession &source) {
        auto inspected = source.InspectExternal();
        if (inspected.HasValue() && source.conflict_)
            source.history_.RetireUnavailable([](const auto &entry, bool) {
                return entry.applyGroup.has_value();
            });
        return inspected;
    }

    /** @copydoc PrefabApplyTransaction::Replay */
    PrefabApplyOutcome PrefabApplyTransaction::Replay(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                      const PrefabApplyReplayRequest &request, const ProjectMutationCoordinator &mutations,
                                                      DurableFileSystem &storage) {
        auto admitted = prefab.Admit(request.source);
        if (admitted.HasError())
            return {.error = std::move(admitted).ErrorValue()};
        auto project = mutations.TryAcquire({prefab.root_, ProjectMutationOwner::Save, "prefab.apply.replay"});
        if (project.HasError())
            return {.error = std::move(project).ErrorValue()};
        try {
            const auto group = NextSceneGroup(scene, request.scene, request.redo);
            if (group.HasError())
                return {.error = group.ErrorValue()};
            if (group.Value() != request.group)
                return {.error = MakeError(Prefab::PrefabErrors::OverrideSourceStale)};
            auto sceneState = scene.PrepareReplay(request.scene, request.redo);
            if (sceneState.HasError())
                return {.error = std::move(sceneState).ErrorValue()};
            auto sourceState = prefab.PrepareApplyReplay(request.source, request.group, request.redo);
            if (sourceState.HasError())
                return {.error = std::move(sourceState).ErrorValue()};
            auto preparedSource = std::move(sourceState).Value();
            return Publish(scene, prefab, *sceneState.Value(), preparedSource, request, storage);
        } catch (...) {
            return {.error = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply replay preparation failed.")};
        }
    }

    /** @copydoc PrefabApplyTransaction::Publish */
    PrefabApplyOutcome PrefabApplyTransaction::Publish(SceneDocumentTransactions &scene, PrefabDocumentSession &prefab,
                                                       SceneDocumentTransactions::Prepared &preparedScene,
                                                       PrefabDocumentSession::PreparedApply &preparedSource,
                                                       const PrefabApplyReplayRequest &request, DurableFileSystem &storage) {
        if (request.scenePath == prefab.path_)
            return {.error = MakeError(Prefab::PrefabErrors::DocumentInvalid)};
        PrefabApplyOutcome outcome;
        std::array<Error, 2> rollbackFailure{MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed),
                                             MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)};
        auto exception = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply publication failed.");
        std::array<ApplyFile, 2> files{{{prefab.path_, prefab.accepted_}, {request.scenePath, request.sceneIdentity}}};
        try {
            const auto run = [&]() -> Result<void> {
                auto sourceBytes = preparedSource.source->SerializeCanonical();
                if (sourceBytes.HasError())
                    return Result<void>::Failure(std::move(sourceBytes).ErrorValue());
                files[0].replacement = std::move(sourceBytes).Value();
                files[1].replacement = scene.EncodePreparedScene(preparedScene);
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
                if (auto valid = scene.ValidatePrepared(preparedScene); valid.HasError())
                    return valid;
                if (auto valid = prefab.Admit(request.source); valid.HasError())
                    return valid;
                outcome.sceneChecksum = FormatSha256(files[1].installed->digest);
                auto resultingScene = scene.PreparedContext(preparedScene);
                resultingScene.cancellation = {};
                prefab.PublishApply(preparedSource, *files[0].installed);
                outcome.committed = scene.PublishPrepared(preparedScene);
                outcome.group = request.group;
                outcome.sceneIdentity = *files[1].installed;
                return scene.MarkSaved(resultingScene);
            };
            if (auto applied = run(); applied.HasError())
                outcome.error = std::move(applied).ErrorValue();
        } catch (...) {
            outcome.error = std::move(exception);
        }
        if (!outcome.committed) {
            const SourceWriteGuard guard(prefab.busy_);
            Compensate(files, prefab.root_, storage, outcome, rollbackFailure);
        } else
            prefab.ReleaseStage();
        return outcome;
    }
}  // namespace Horo::Editor
