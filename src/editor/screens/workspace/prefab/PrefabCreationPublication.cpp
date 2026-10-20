#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include <algorithm>
#include <cctype>
#include <new>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        /** @brief Admits only portable filenames in existing ordinary project asset directories. */
        Result<void> ValidateDestination(const std::filesystem::path &root, const std::filesystem::path &destination,
                                         DurableFileSystem &files) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            };
            if (!root.is_absolute() || !destination.is_absolute() || root != root.lexically_normal() ||
                destination != destination.lexically_normal() || destination.extension() != ".prefab")
                return invalid();
            const auto relative = destination.lexically_relative(root);
            const auto assetRoot = ProjectLayout::AssetRoot(root);
            if (relative.empty() || *relative.begin() != assetRoot.filename())
                return invalid();
            for (const auto &part : relative) {
                const auto bytes = part.generic_u8string();
                const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
                if (text.empty() || text.size() > 128 || text.front() == '.' || text.back() == '.' || text.back() == ' ' ||
                    !IsValidUtf8ScalarSequence(text) || std::ranges::any_of(text, [](const unsigned char value) {
                    return value < 32U || std::string_view{"<>:\"/\\|?*"}.find(static_cast<char>(value)) != std::string_view::npos;
                }))
                    return invalid();
                std::string name{text.substr(0, text.find('.'))};
                std::ranges::transform(name, name.begin(), [](const unsigned char c) {
                    return static_cast<char>(std::toupper(c));
                });
                if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" ||
                    (name.size() == 4 && (name.starts_with("COM") || name.starts_with("LPT")) && name[3] >= '1' && name[3] <= '9'))
                    return invalid();
            }
            auto current = root;
            for (auto part = relative.begin(); part != relative.end(); ++part) {
                if (std::next(part) == relative.end())
                    break;
                current /= *part;
                auto entry = files.InspectEntry(current);
                if (entry.HasError())
                    return Result<void>::Failure(entry.ErrorValue());
                if (!entry.Value() || entry.Value()->kind != FileEntryKind::Directory)
                    return invalid();
            }
            auto lock = Assets::AssetSourcePublicationLockPath(root, destination);
            if (lock.HasError())
                return Result<void>::Failure(lock.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Hashes complete prepared bytes before any durable writes occur. */
        Assets::AssetSourceIdentity Content(const std::string &bytes) {
            return {true, bytes.size(), ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}))};
        }

        /** @brief Captures exact object/mutation evidence immediately after exclusive creation, retaining uncertainty explicitly. */
        Result<void> CaptureCreated(PrefabCreationReceipt &receipt, DurableFileSystem &files) {
            auto entry = files.InspectEntry(receipt.path);
            if (entry.HasError())
                return Result<void>::Failure(entry.ErrorValue());
            if (!entry.Value() || entry.Value()->kind != FileEntryKind::RegularFile || entry.Value()->hardLinks != 1 ||
                !entry.Value()->identity || entry.Value()->sizeBytes != receipt.content.bytes)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            receipt.identity = std::move(entry).Value();
            return Result<void>::Success();
        }

        /** @brief Rechecks both published creations before exposing a Scene reference to them. */
        Result<void> ValidateCreatedFiles(const PrefabCreationOutcome &outcome, DurableFileSystem &files) {
            for (const auto &receipt : outcome.files) {
                auto current = files.InspectEntry(receipt.path);
                if (current.HasError())
                    return Result<void>::Failure(current.ErrorValue());
                if (!receipt.identity || !current.Value() || !receipt.identity->identity ||
                    receipt.identity->identity != current.Value()->identity || current.Value()->kind != FileEntryKind::RegularFile ||
                    current.Value()->hardLinks != 1)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                auto captured = files.ReadFileBounded(receipt.path, Prefab::PrefabHardLimits::SourceDocumentBytes);
                if (captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
                const auto &bytes = captured.Value();
                if (bytes.size() != receipt.content.bytes || ComputeSha256(std::as_bytes(std::span{bytes})) != receipt.content.digest)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
                auto after = files.InspectEntry(receipt.path);
                if (after.HasError() || !after.Value() || receipt.identity->identity != after.Value()->identity)
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }

        /** @brief Removes only an unchanged owned creation under the host's existing cooperative writer leases.
         * @details Identity/content comparison is not an OS compare-delete against noncooperating namespace mutation.
         */
        void Rollback(PrefabCreationOutcome &outcome, DurableFileSystem &files, std::array<Error, 2> &recoveryFailures) noexcept {
            static_assert(std::is_nothrow_move_constructible_v<Error> && std::is_nothrow_move_assignable_v<Error>);
            for (auto entry = outcome.files.rbegin(); entry != outcome.files.rend(); ++entry) {
                auto &receipt = *entry;
                if (!receipt.created || receipt.removed)
                    continue;
                const auto rejected = [&receipt, &outcome, &recoveryFailures]() noexcept {
                    const auto index = static_cast<std::size_t>(&receipt - outcome.files.data());
                    receipt.recoveryError = std::move(recoveryFailures[index]);
                };
                try {
                    auto current = files.InspectEntry(receipt.path);
                    if (current.HasError()) {
                        receipt.recoveryError = current.ErrorValue();
                        continue;
                    }
                    if (!current.Value()) {
                        receipt.removed = true;
                        continue;
                    }
                    if (!receipt.identity || !receipt.identity->identity || !current.Value()->identity ||
                        receipt.identity->identity != current.Value()->identity || current.Value()->kind != FileEntryKind::RegularFile ||
                        current.Value()->hardLinks != 1 || current.Value()->sizeBytes != receipt.content.bytes) {
                        rejected();
                        continue;
                    }
                    auto captured = files.ReadFileBounded(receipt.path, Prefab::PrefabHardLimits::SourceDocumentBytes);
                    if (captured.HasError()) {
                        receipt.recoveryError = captured.ErrorValue();
                        continue;
                    }
                    const auto &bytes = captured.Value();
                    auto after = files.InspectEntry(receipt.path);
                    if (bytes.size() != receipt.content.bytes || ComputeSha256(std::as_bytes(std::span{bytes})) != receipt.content.digest ||
                        after.HasError() || !after.Value() || after.Value()->identity != receipt.identity->identity) {
                        rejected();
                        continue;
                    }
                    if (auto removed = files.RemoveDurable(receipt.path); removed.HasError())
                        receipt.recoveryError = removed.ErrorValue();
                    else
                        receipt.removed = true;
                } catch (...) {  // Bounded cleanup cannot replace the original publication failure or delete unproven content.
                    rejected();
                }
            }
        }

        /** @brief Rechecks the last accepted persisted Scene source without comparing it to unsaved working content. */
        Result<void> ValidateSceneSource(const PrefabCreationRequest &request, DurableFileSystem &files) {
            if (!request.sceneSource)
                return Result<void>::Success();
            const auto &source = *request.sceneSource;
            if (!source.accepted.exists)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto captured = files.ReadFileBounded(source.path, MaximumSceneObjectFingerprintBytes);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            const auto &bytes = captured.Value();
            if (bytes.size() != source.accepted.bytes || ComputeSha256(std::as_bytes(std::span{bytes})) != source.accepted.digest)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict,
                                                       "The persisted Scene source changed after this document accepted its disk base."));
            return Result<void>::Success();
        }

        /** @brief Keeps the accepted Scene source under the same cooperative publication authority through commit and cleanup. */
        Result<void> LockSceneSource(const PrefabCreationRequest &request, DurableFileSystem &files,
                                     std::array<std::optional<ExclusiveFileLock>, 3> &locks) {
            if (!request.sceneSource)
                return Result<void>::Success();
            auto path = Assets::AssetSourcePublicationLockPath(request.projectRoot, request.sceneSource->path);
            if (path.HasError())
                return Result<void>::Failure(path.ErrorValue());
            auto locked = files.TryAcquireExclusive(path.Value(), "editor.prefab.scene_fence");
            if (locked.HasError())
                return Result<void>::Failure(locked.ErrorValue());
            locks[2].emplace(std::move(locked).Value());
            return ValidateSceneSource(request, files);
        }

        /** @brief Fully owned source bytes and canonical Scene/history reservation; all allocations precede source writes. */
        struct PreparedCreation final {
            SceneDocumentTransactions::PrefabReplacement replacement;
            std::array<std::string, 2> bytes;
        };

        /** @brief Reserves both source leases and verifies exact destination absence before either creation. */
        Result<void> LockDestinations(const std::filesystem::path &root, const PrefabCreationOutcome &outcome, DurableFileSystem &files,
                                      std::array<std::optional<ExclusiveFileLock>, 3> &locks) {
            for (std::size_t index = 0; index != outcome.files.size(); ++index) {
                auto lockPath = Assets::AssetSourcePublicationLockPath(root, outcome.files[index].path);
                if (lockPath.HasError())
                    return Result<void>::Failure(lockPath.ErrorValue());
                auto locked = files.TryAcquireExclusive(lockPath.Value(), "editor.prefab.create_selection");
                if (locked.HasError())
                    return Result<void>::Failure(locked.ErrorValue());
                locks[index].emplace(std::move(locked).Value());
                const auto existing = files.InspectEntry(outcome.files[index].path);
                if (existing.HasError())
                    return Result<void>::Failure(existing.ErrorValue());
                if (existing.Value())
                    return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            }
            return Result<void>::Success();
        }

        /** @brief Completes schema/reference/path/state validation and reserves canonical publication before file writes. */
        Result<PreparedCreation> PrepareCreation(SceneDocumentTransactions &scene, const PrefabCreationRequest &request,
                                                 PrefabCreationOutcome &outcome, DurableFileSystem &files,
                                                 std::array<std::optional<ExclusiveFileLock>, 3> &locks) {
            using Return = Result<PreparedCreation>;
            if (const auto path = ValidateDestination(request.projectRoot, request.destination, files); path.HasError())
                return Return::Failure(path.ErrorValue());
            if (const auto source = LockSceneSource(request, files, locks); source.HasError())
                return Return::Failure(source.ErrorValue());
            if (!request.asset.IsValid() || request.registry.Find(request.asset))
                return Return::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
            const auto snapshot = scene.Snapshot();
            if (snapshot.HasError())
                return Return::Failure(snapshot.ErrorValue());
            const auto &context = request.context;
            if (snapshot.Value().session != context.session || snapshot.Value().revision != context.revision ||
                snapshot.Value().state != context.state || !context.canEdit || context.cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Stale)));
            const auto source = PreparePrefabSelectionSource(snapshot.Value(), request.root, request.asset, request.version,
                                                             request.registry, request.behaviors, outcome.boundaries);
            if (source.HasError())
                return Return::Failure(source.ErrorValue());
            auto encoded = source.Value().SerializeCanonical();
            if (encoded.HasError())
                return Return::Failure(encoded.ErrorValue());
            auto replacement =
                scene.PreparePrefabReplacement(context, request.root, Prefab::PrefabAssetReference::Create(request.asset).Value());
            if (replacement.HasError())
                return Return::Failure(replacement.ErrorValue());
            PreparedCreation prepared{std::move(replacement).Value(),
                                      {std::move(encoded).Value(), "{\"schemaVersion\":1,\"assetId\":\"" + request.asset.ToString() +
                                                                       "\",\"assetType\":\"core.prefab\"}\n"}};
            outcome.instance = prepared.replacement.Instance();
            outcome.files[0] = {.path = request.destination, .content = Content(prepared.bytes[0])};
            auto metadata = request.destination;
            metadata += ".horo";
            outcome.files[1] = {.path = std::move(metadata), .content = Content(prepared.bytes[1])};
            if (const auto locked = LockDestinations(request.projectRoot, outcome, files, locks); locked.HasError())
                return Return::Failure(locked.ErrorValue());
            return Return::Success(std::move(prepared));
        }

        /** @brief Performs only exclusive creations and retains ownership receipts even on native/provider failure. */
        Result<void> WritePreparedFiles(const PreparedCreation &prepared, const CancellationToken &cancellation,
                                        PrefabCreationOutcome &outcome, DurableFileSystem &files) {
            for (std::size_t index = 0; index != outcome.files.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Cancelled)));
                auto &receipt = outcome.files[index];
                const auto bytes = std::as_bytes(std::span{prepared.bytes[index].data(), prepared.bytes[index].size()});
                if (const auto written = files.WritePrivateDurable(receipt.path, bytes, receipt.created); written.HasError())
                    return Result<void>::Failure(written.ErrorValue());
                if (const auto captured = CaptureCreated(receipt, files); captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
            }
            return ValidateCreatedFiles(outcome, files);
        }
    }  // namespace

    /** @copydoc CreatePrefabFromSelection */
    PrefabCreationOutcome CreatePrefabFromSelection(SceneDocumentTransactions &scene, const PrefabCreationRequest &request,
                                                    ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        PrefabCreationOutcome outcome;
        outcome.asset = request.asset;
        auto exceptionError = MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed);
        auto providerError =
            MakeError(Assets::AssetSourceErrors::ReadFailed,
                      "Filesystem provider failed during prefab creation; retained file receipts identify any unresolved publication.");
        std::array<Error, 2> recoveryFailures;
        for (auto &failure : recoveryFailures)
            failure = MakeError(Assets::AssetSourceErrors::Conflict,
                                "Created prefab file needs explicit reconciliation; current ownership/content cannot be proven.");
        // The lease and all source locks remain resident across cleanup and final in-memory commit.
        auto acquired = mutations.TryAcquire({request.projectRoot, ProjectMutationOwner::Asset, "editor.prefab.create_selection"});
        if (acquired.HasError()) {
            outcome.error = acquired.ErrorValue();
            return outcome;
        }
        [[maybe_unused]] auto projectLease = std::move(acquired).Value();
        // These leases deliberately outlive the try/catch: failure cleanup below the catch
        // must remain serialized with cooperating project/source writers until rollback finishes.
        std::array<std::optional<ExclusiveFileLock>, 3> locks;
        try {
            auto prepared = PrepareCreation(scene, request, outcome, files, locks);
            if (prepared.HasError()) {
                outcome.error = prepared.ErrorValue();
                return outcome;
            }
            auto candidate = std::move(prepared).Value();
            if (const auto written = WritePreparedFiles(candidate, request.context.cancellation, outcome, files); written.HasError()) {
                outcome.error = written.ErrorValue();
            } else if (const auto source = ValidateSceneSource(request, files); source.HasError()) {
                outcome.error = source.ErrorValue();
            } else if (const auto committed = scene.CommitPrefabReplacement(candidate.replacement); committed.HasError()) {
                outcome.error = committed.ErrorValue();
            } else {
                outcome.committed = committed.Value().committed;
            }
        } catch (const std::bad_alloc &) {
            outcome.error = std::move(exceptionError);
        } catch (...) {
            outcome.error = std::move(providerError);
        }
        if (!outcome.committed)
            Rollback(outcome, files, recoveryFailures);
        return outcome;
    }
}  // namespace Horo::Editor
