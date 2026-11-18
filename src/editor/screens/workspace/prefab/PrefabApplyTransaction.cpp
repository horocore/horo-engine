#include "editor/screens/workspace/prefab/PrefabApplyTransaction.h"

#include "Horo/Editor/DocumentRecoveryService.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <array>
#include <cctype>

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

        constexpr std::size_t MaximumCapsuleBytes = 64U * 1024U * 1024U;
        constexpr std::string_view CapsuleMagic = "HoroPrefabApplyRecoveryV1";
        constexpr std::size_t MaximumRecoveryPathBytes = 4096;

        /** @brief Stable private recovery namespace for one unresolved paired operation per project. */
        std::filesystem::path RecoveryPath(const std::filesystem::path &root) {
            return root / ".horo/local/recovery/prefab-apply-pending.bin";
        }

        /** @brief A recovery capsule never authorizes workspace control files or different asset types. */
        bool ValidRecoveryPath(const std::string_view relative, const std::size_t index) {
            if (relative.empty() || relative.size() > MaximumRecoveryPathBytes || !IsValidUtf8ScalarSequence(relative) ||
                relative.find_first_of(":\\") != std::string_view::npos || relative.find('\0') != std::string_view::npos)
                return false;
            const auto normalized = ProjectPath::Parse(relative);
            if (normalized.HasError() || normalized.Value().String() != relative)
                return false;
            std::string first{relative.substr(0, relative.find('/'))};
            std::ranges::transform(first, first.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            const auto path = std::filesystem::path{std::u8string(relative.begin(), relative.end())};
            if (first == ".horo" || std::ranges::any_of(path, [](const auto &part) {
                const auto &name = part.native();
                return name.ends_with('.') || name.ends_with(' ');
            }))
                return false;
            return path.extension() == (index == 0 ? ".prefab" : ".horo");
        }

        /** @brief Appends a bounded canonical raw field. */
        template <typename Range> void Append(std::vector<std::byte> &output, const Range &value) {
            const auto bytes = std::as_bytes(std::span{value});
            output.insert(output.end(), bytes.begin(), bytes.end());
        }

        /** @brief Uses fixed big-endian widths independent of native size_t. */
        void AppendSize(std::vector<std::byte> &output, const std::size_t value) {
            for (unsigned shift = 32; shift > 0; shift -= 8)
                output.push_back(static_cast<std::byte>((value >> (shift - 8)) & 0xff));
        }

        /** @brief Reads only checked fields from bounded local recovery bytes. */
        struct RecoveryReader final {
            std::span<const std::byte> remaining;
            bool valid{true};

            std::span<const std::byte> Take(const std::size_t size) {
                if (!valid || size > remaining.size()) {
                    valid = false;
                    return {};
                }
                const auto field = remaining.first(size);
                remaining = remaining.subspan(size);
                return field;
            }

            std::size_t Size() {
                std::size_t value{};
                for (const auto byte : Take(4))
                    value = (value << 8) | std::to_integer<std::uint8_t>(byte);
                return value;
            }

            Sha256Digest Digest() {
                Sha256Digest result;
                const auto bytes = Take(result.bytes.size());
                std::ranges::transform(bytes, result.bytes.begin(), [](const auto byte) {
                    return std::to_integer<std::uint8_t>(byte);
                });
                return result;
            }
        };

        /** @brief The recovery payload keeps exact original bytes and only identity of the installed pair. */
        Result<std::vector<std::byte>> EncodeRecovery(const std::filesystem::path &root, const std::array<ApplyFile, 2> &files,
                                                      const PrefabApplyOutcome &outcome) {
            using Return = Result<std::vector<std::byte>>;
            std::vector<std::byte> encoded;
            Append(encoded, CapsuleMagic);
            for (std::size_t index = 0; index < files.size(); ++index) {
                const auto &file = files[index];
                const auto &pending = outcome.recovery[index];
                const auto &original = pending ? pending->original : file.original;
                const auto &path = pending ? pending->path : file.path;
                const auto name = path.lexically_relative(root).generic_u8string();
                const std::string relative{name.begin(), name.end()};
                if (!original.identity.exists || !ValidRecoveryPath(relative, index))
                    return Return::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
                const auto &installed = pending ? pending->installed : original.identity;
                const auto payloadSize = installed != original.identity ? original.bytes.size() : 0;
                const auto overhead = 32U + 12U + 64U + relative.size();
                if (overhead > MaximumCapsuleBytes - encoded.size() || payloadSize > MaximumCapsuleBytes - encoded.size() - overhead)
                    return Return::Failure(MakeError(Assets::AssetSourceErrors::Limit));
                AppendSize(encoded, relative.size());
                AppendSize(encoded, original.identity.bytes);
                AppendSize(encoded, installed.bytes);
                Append(encoded, original.identity.digest.bytes);
                Append(encoded, installed.digest.bytes);
                Append(encoded, relative);
                if (installed != original.identity)
                    Append(encoded, original.bytes);
            }
            Append(encoded, ComputeSha256(encoded).bytes);
            return Return::Success(std::move(encoded));
        }

        /** @brief Decoded spans borrow the one checksummed capsule until the restart operation finishes. */
        struct RecoveryTarget final {
            std::filesystem::path path;
            std::span<const std::byte> originalBytes;
            Assets::AssetSourceIdentity original;
            Assets::AssetSourceIdentity installed;
            std::optional<ExclusiveFileLock> lock;
            bool needsRestore{};
        };

        /** @brief Validates wire shape, stable ownership paths and byte identities before any source admission. */
        Result<std::array<RecoveryTarget, 2>> DecodeRecovery(const std::filesystem::path &root, std::span<const std::byte> bytes) {
            using Return = Result<std::array<RecoveryTarget, 2>>;
            const auto invalid = [] {
                return Return::Failure(MakeError(Assets::AssetSourceErrors::ReadFailed));
            };
            if (bytes.size() < CapsuleMagic.size() + 32)
                return invalid();
            RecoveryReader tail{bytes.last(32)};
            const auto content = bytes.first(bytes.size() - 32);
            if (ComputeSha256(content) != tail.Digest())
                return invalid();
            RecoveryReader reader{content};
            if (!std::ranges::equal(reader.Take(CapsuleMagic.size()), std::as_bytes(std::span{CapsuleMagic})))
                return invalid();
            std::array<RecoveryTarget, 2> targets;
            for (std::size_t index = 0; index < targets.size(); ++index) {
                const auto nameSize = reader.Size();
                const auto originalSize = reader.Size();
                const auto installedSize = reader.Size();
                const Assets::AssetSourceIdentity originalIdentity{true, originalSize, reader.Digest()};
                const Assets::AssetSourceIdentity installed{true, installedSize, reader.Digest()};
                const auto name = reader.Take(nameSize);
                const auto original = reader.Take(originalIdentity == installed ? 0 : originalSize);
                if (!reader.valid || name.empty() || originalSize > MaximumCapsuleBytes || installedSize > MaximumCapsuleBytes ||
                    (originalIdentity != installed && ComputeSha256(original) != originalIdentity.digest))
                    return invalid();
                const std::string_view relative{reinterpret_cast<const char *>(name.data()), name.size()};
                if (!ValidRecoveryPath(relative, index))
                    return invalid();
                targets[index] = {.path = root / std::filesystem::path{std::u8string(relative.begin(), relative.end())},
                                  .originalBytes = original,
                                  .original = originalIdentity,
                                  .installed = installed};
            }
            if (!reader.remaining.empty() || targets.front().path == targets.back().path)
                return invalid();
            return Return::Success(std::move(targets));
        }

        /** @brief Publishes the existing unresolved compensation through the shared project-local recovery writer. */
        Result<void> PersistRecovery(const std::filesystem::path &root, const std::array<ApplyFile, 2> &files,
                                     const PrefabApplyOutcome &outcome, DurableFileSystem &storage) {
            const auto encoded = EncodeRecovery(root, files, outcome);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            auto publication = PublishDocumentRecovery(root, RecoveryPath(root), encoded.Value(), {}, storage,
                                                       {.maximumRecordBytes = MaximumCapsuleBytes});
            if (publication.HasError())
                return Result<void>::Failure(publication.ErrorValue());
            if (publication.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
            return Result<void>::Success();
        }

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

    /** @copydoc PrefabApplyTransaction::InspectPendingRecovery */
    Result<std::optional<PrefabApplyRecoveryCandidate>> PrefabApplyTransaction::InspectPendingRecovery(const std::filesystem::path &root) {
        using Return = Result<std::optional<PrefabApplyRecoveryCandidate>>;
        const auto capsule = Assets::CaptureAssetSource(root, RecoveryPath(root), MaximumCapsuleBytes);
        if (capsule.HasError())
            return Return::Failure(capsule.ErrorValue());
        const auto &identity = capsule.Value().identity;
        if (!identity.exists)
            return Return::Success(std::nullopt);
        const auto decoded = DecodeRecovery(root, capsule.Value().bytes);
        if (decoded.HasError())
            return Return::Failure(decoded.ErrorValue());
        const auto &targets = decoded.Value();
        return Return::Success(
            PrefabApplyRecoveryCandidate{identity, {targets[0].path, targets[0].original}, {targets[1].path, targets[1].original}});
    }

    /** @copydoc PrefabApplyTransaction::RestorePendingRecovery */
    Result<void> PrefabApplyTransaction::RestorePendingRecovery(const std::filesystem::path &root,
                                                                const Assets::AssetSourceIdentity &expected,
                                                                const ProjectMutationCoordinator &mutations, DurableFileSystem &storage) {
        using Return = Result<void>;
        const auto conflict = [] {
            return Return::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
        };
        if (!expected.exists)
            return conflict();
        auto project = mutations.TryAcquire({root, ProjectMutationOwner::Save, "prefab.apply.restore"});
        if (project.HasError())
            return Return::Failure(project.ErrorValue());
        const auto path = RecoveryPath(root);
        const auto lockPath = Assets::AssetSourcePublicationLockPath(root, path);
        if (lockPath.HasError())
            return Return::Failure(lockPath.ErrorValue());
        auto capsuleLock = storage.TryAcquireExclusive(lockPath.Value(), "prefab.apply.restore");
        if (capsuleLock.HasError())
            return Return::Failure(capsuleLock.ErrorValue());
        auto capsule = Assets::CaptureAssetSource(root, path, MaximumCapsuleBytes);
        if (capsule.HasError())
            return Return::Failure(capsule.ErrorValue());
        if (capsule.Value().identity != expected)
            return conflict();
        auto decoded = DecodeRecovery(root, capsule.Value().bytes);
        if (decoded.HasError())
            return Return::Failure(decoded.ErrorValue());
        auto targets = std::move(decoded).Value();
        for (auto &target : targets) {
            const auto lock = Assets::AssetSourcePublicationLockPath(root, target.path);
            if (lock.HasError())
                return Return::Failure(lock.ErrorValue());
            auto held = storage.TryAcquireExclusive(lock.Value(), "prefab.apply.restore");
            if (held.HasError())
                return Return::Failure(held.ErrorValue());
            target.lock.emplace(std::move(held).Value());
            const auto current = Assets::CaptureAssetSource(root, target.path, MaximumCapsuleBytes);
            if (current.HasError())
                return Return::Failure(current.ErrorValue());
            if (current.Value().identity == target.original)
                continue;
            if (current.Value().identity != target.installed)
                return conflict();
            target.needsRestore = true;
        }
        // Match the normal compensation order and leave the capsule retryable after either publication.
        for (auto target = targets.rbegin(); target != targets.rend(); ++target) {
            if (!target->needsRestore) {
                if (auto synced = storage.SyncDirectory(target->path.parent_path()); synced.HasError())
                    return synced;
                continue;
            }
            auto restored =
                Assets::PublishAssetSource(root, target->path, target->installed, target->originalBytes, *target->lock, storage);
            if (restored.HasError())
                return Return::Failure(restored.ErrorValue());
            if (restored.Value().durabilityError)
                return Return::Failure(*restored.Value().durabilityError);
            if (restored.Value().status != Assets::AssetSourcePublicationStatus::Durable)
                return Return::Failure(MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed));
        }
        for (const auto &target : targets) {
            const auto current = Assets::CaptureAssetSource(root, target.path, MaximumCapsuleBytes);
            if (current.HasError())
                return Return::Failure(current.ErrorValue());
            if (current.Value().identity != target.original)
                return conflict();
        }
        const auto current = Assets::CaptureAssetSource(root, path, MaximumCapsuleBytes);
        if (current.HasError())
            return Return::Failure(current.ErrorValue());
        if (current.Value().identity != expected)
            return conflict();
        return storage.RemoveDurable(path);
    }

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
            const auto pending = InspectPendingRecovery(prefab.root_);
            if (pending.HasError())
                return {.error = pending.ErrorValue()};
            if (pending.Value())
                return {.error = MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)};
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
            const auto pending = InspectPendingRecovery(prefab.root_);
            if (pending.HasError())
                return {.error = pending.ErrorValue()};
            if (pending.Value())
                return {.error = MakeError(Assets::AssetSourceErrors::DurabilityUnconfirmed)};
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
        auto recoveryException = MakeError(Assets::AssetSourceErrors::ReadFailed, "Prefab apply recovery publication failed.");
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
            if (std::ranges::any_of(outcome.recovery, [](const auto &item) {
                return item.has_value();
            })) {
                try {
                    if (auto recovery = PersistRecovery(prefab.root_, files, outcome, storage); recovery.HasError())
                        outcome.recoveryPersistenceError = std::move(recovery).ErrorValue();
                } catch (...) {
                    outcome.recoveryPersistenceError = std::move(recoveryException);
                }
            }
        } else
            prefab.ReleaseStage();
        return outcome;
    }
}  // namespace Horo::Editor
