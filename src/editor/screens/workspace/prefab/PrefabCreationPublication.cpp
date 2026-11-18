#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Utf8.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <new>
#include <string_view>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        constexpr std::array ForbiddenFilenameCharacters{'<', '>', ':', '"', '/', '\\', '|', '?', '*'};

        /** @brief Validates one portable UTF-8 path component without consulting the filesystem. */
        Result<void> ValidatePathPart(const std::filesystem::path &part) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            };
            const auto bytes = part.generic_u8string();
            if (bytes.empty() || bytes.size() > 128)
                return invalid();
            const std::string text{bytes.begin(), bytes.end()};
            if (text.front() == '.' || text.back() == '.' || text.back() == ' ' || !IsValidUtf8ScalarSequence(text) ||
                std::ranges::any_of(text, [](const unsigned char value) {
                return value < 32U ||
                       std::ranges::find(ForbiddenFilenameCharacters, static_cast<char>(value)) != ForbiddenFilenameCharacters.end();
            }))
                return invalid();
            std::string name{text.substr(0, text.find('.'))};
            std::ranges::transform(name, name.begin(), [](const unsigned char c) {
                return static_cast<char>(std::toupper(c));
            });
            if (name == "CON" || name == "PRN" || name == "AUX" || name == "NUL" ||
                (name.size() == 4 && (name.starts_with("COM") || name.starts_with("LPT")) && name[3] >= '1' && name[3] <= '9'))
                return invalid();
            return Result<void>::Success();
        }

        /** @brief Admits only portable filenames in existing ordinary project asset directories. */
        Result<void> ValidateDestination(const std::filesystem::path &root, const std::filesystem::path &destination,
                                         const DurableFileSystem &files) {
            const auto invalid = [] {
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::InvalidPath));
            };
            if (!root.is_absolute() || !destination.is_absolute() || root != root.lexically_normal() ||
                destination != destination.lexically_normal() || destination.extension() != ".prefab")
                return invalid();
            const auto relative = destination.lexically_relative(root);
            if (const auto assetRoot = ProjectLayout::AssetRoot(root); relative.empty() || *relative.begin() != assetRoot.filename())
                return invalid();
            for (const auto &part : relative) {
                if (auto admitted = ValidatePathPart(part); admitted.HasError())
                    return admitted;
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
            if (auto lock = Assets::AssetSourcePublicationLockPath(root, destination); lock.HasError())
                return Result<void>::Failure(lock.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Hashes complete prepared bytes before any durable writes occur. */
        Assets::AssetSourceIdentity Content(const std::string_view bytes) {
            return {true, bytes.size(), ComputeSha256(std::as_bytes(std::span{bytes.data(), bytes.size()}))};
        }

        /** @brief Captures exact object/mutation evidence immediately after exclusive creation, retaining uncertainty explicitly. */
        Result<void> CaptureCreated(PrefabCreationReceipt &receipt, const DurableFileSystem &files) {
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
        Result<void> ValidateCreatedFiles(const PrefabCreationOutcome &outcome, const DurableFileSystem &files) {
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
                if (const auto &bytes = captured.Value();
                    bytes.size() != receipt.content.bytes || ComputeSha256(std::as_bytes(std::span{bytes})) != receipt.content.digest)
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
                    if (auto after = files.InspectEntry(receipt.path);
                        bytes.size() != receipt.content.bytes || ComputeSha256(std::as_bytes(std::span{bytes})) != receipt.content.digest ||
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
        Result<void> ValidateSceneSource(const PrefabCreationRequest &request, const DurableFileSystem &files) {
            if (!request.sceneSource)
                return Result<void>::Success();
            const auto &source = *request.sceneSource;
            if (!source.accepted.exists)
                return Result<void>::Failure(MakeError(Assets::AssetSourceErrors::Conflict));
            const auto captured = files.ReadFileBounded(source.path, MaximumSceneObjectFingerprintBytes);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            if (const auto &bytes = captured.Value();
                bytes.size() != source.accepted.bytes || ComputeSha256(std::as_bytes(std::span{bytes})) != source.accepted.digest)
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

        /** @brief Prepares and locks the one canonical prefab source/sidecar pair for either creation route. */
        Result<std::array<std::string, 2>> PreparePair(const Prefab::PrefabDocument &source, const std::filesystem::path &root,
                                                       const std::filesystem::path &destination, PrefabCreationOutcome &outcome,
                                                       DurableFileSystem &files, std::array<std::optional<ExclusiveFileLock>, 3> &locks) {
            using Return = Result<std::array<std::string, 2>>;
            auto encoded = source.SerializeCanonical();
            if (encoded.HasError())
                return Return::Failure(encoded.ErrorValue());
            std::array<std::string, 2> bytes{std::move(encoded).Value(),
                                             std::format(R"({{"schemaVersion":1,"assetId":"{}","assetType":"core.prefab"}})"
                                                         "\n",
                                                         source.Data().assetId.ToString())};
            outcome.files[0] = {.path = destination, .content = Content(bytes[0])};
            auto metadata = destination;
            metadata += ".horo";
            outcome.files[1] = {.path = std::move(metadata), .content = Content(bytes[1])};
            if (const auto locked = LockDestinations(root, outcome, files, locks); locked.HasError())
                return Return::Failure(locked.ErrorValue());
            return Return::Success(std::move(bytes));
        }

        /** @brief Completes schema/reference/path/state validation and reserves canonical publication before file writes. */
        Result<PreparedCreation> PrepareCreation(const SceneDocumentTransactions &scene, const PrefabCreationRequest &request,
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
            const auto settings = Application::LoadPrefabProjectSettings(request.projectRoot);
            if (settings.HasError())
                return Return::Failure(settings.ErrorValue());
            const auto source =
                PreparePrefabSelectionSource(snapshot.Value(), request.root, request.asset, request.version, request.registry,
                                             request.behaviors, outcome.boundaries, &settings.Value().settings->Limits());
            if (source.HasError())
                return Return::Failure(source.ErrorValue());
            auto replacement =
                scene.PreparePrefabReplacement(context, request.root, Prefab::PrefabAssetReference::Create(request.asset).Value());
            if (replacement.HasError())
                return Return::Failure(replacement.ErrorValue());
            auto bytes = PreparePair(source.Value(), request.projectRoot, request.destination, outcome, files, locks);
            if (bytes.HasError())
                return Return::Failure(bytes.ErrorValue());
            outcome.instance = replacement.Value().Instance();
            return Return::Success({std::move(replacement).Value(), std::move(bytes).Value()});
        }

        /** @brief Performs only exclusive creations and retains ownership receipts even on native/provider failure. */
        Result<void> WritePreparedFiles(const std::array<std::string, 2> &prepared, const CancellationToken &cancellation,
                                        PrefabCreationOutcome &outcome, DurableFileSystem &files) {
            for (std::size_t index = 0; index != outcome.files.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SceneTransactionError(SceneTransactionFailure::Cancelled)));
                auto &receipt = outcome.files[index];
                const auto bytes = std::as_bytes(std::span{prepared[index].data(), prepared[index].size()});
                if (const auto written = files.WritePrivateDurable(receipt.path, bytes, receipt.created); written.HasError())
                    return Result<void>::Failure(written.ErrorValue());
                if (const auto captured = CaptureCreated(receipt, files); captured.HasError())
                    return Result<void>::Failure(captured.ErrorValue());
            }
            return ValidateCreatedFiles(outcome, files);
        }

        /** @brief Retains project and file leases across both exclusive creation and ownership-checked rollback. */
        template <typename Attempt>
        PrefabCreationOutcome PublishCreation(const std::filesystem::path &root, const Assets::AssetId asset,
                                              const ProjectMutationCoordinator &mutations, DurableFileSystem &files, Attempt attempt) {
            PrefabCreationOutcome outcome;
            outcome.asset = asset;
            std::optional<Error> exceptionError{MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed)};
            std::optional<Error> providerError{
                MakeError(Assets::AssetSourceErrors::ReadFailed,
                          "Filesystem provider failed during prefab creation. Retained file receipts identify publication.")};
            std::array<Error, 2> recoveryFailures;
            for (auto &failure : recoveryFailures)
                failure = MakeError(Assets::AssetSourceErrors::Conflict,
                                    "Created prefab file needs explicit reconciliation. Current ownership or content cannot be proven.");

            auto acquired = mutations.TryAcquire({root, ProjectMutationOwner::Asset, "editor.prefab.create"});
            if (acquired.HasError()) {
                outcome.error = acquired.ErrorValue();
                return outcome;
            }
            [[maybe_unused]] auto projectLease = std::move(acquired).Value();
            std::array<std::optional<ExclusiveFileLock>, 3> locks;
            try {
                auto committed = attempt(outcome, locks);
                if (committed.HasError())
                    outcome.error = std::move(committed).ErrorValue();
                else
                    outcome.committed = committed.Value();
            } catch (const std::bad_alloc &) {
                outcome.error = std::move(exceptionError);
            } catch (...) {
                outcome.error = std::move(providerError);
            }
            if (!outcome.committed)
                Rollback(outcome, files, recoveryFailures);
            return outcome;
        }
    }  // namespace

    /** @copydoc CreatePrefabFromSelection */
    PrefabCreationOutcome CreatePrefabFromSelection(SceneDocumentTransactions &scene, const PrefabCreationRequest &request,
                                                    const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        return PublishCreation(request.projectRoot, request.asset, mutations, files, [&](auto &outcome, auto &locks) -> Result<bool> {
            auto prepared = PrepareCreation(scene, request, outcome, files, locks);
            if (prepared.HasError())
                return Result<bool>::Failure(prepared.ErrorValue());
            auto candidate = std::move(prepared).Value();
            if (const auto written = WritePreparedFiles(candidate.bytes, request.context.cancellation, outcome, files); written.HasError())
                return Result<bool>::Failure(written.ErrorValue());
            if (const auto source = ValidateSceneSource(request, files); source.HasError())
                return Result<bool>::Failure(source.ErrorValue());
            auto committed = scene.CommitPrefabReplacement(candidate.replacement);
            return committed.HasError() ? Result<bool>::Failure(committed.ErrorValue())
                                        : Result<bool>::Success(committed.Value().committed);
        });
    }

    /** @copydoc CreatePrefabFork */
    PrefabCreationOutcome CreatePrefabFork(const Prefab::PrefabDocument &draft, const std::filesystem::path &projectRoot,
                                           const std::filesystem::path &destination, const Assets::AssetId asset,
                                           const Assets::AssetRegistrySnapshot &registry, const ProjectMutationCoordinator &mutations,
                                           DurableFileSystem &files) {
        return PublishCreation(projectRoot, asset, mutations, files, [&](auto &outcome, auto &locks) -> Result<bool> {
            if (!asset.IsValid() || asset == draft.Data().assetId || registry.Find(asset))
                return Result<bool>::Failure(MakeError(Prefab::PrefabErrors::IdentityCollision));
            if (const auto path = ValidateDestination(projectRoot, destination, files); path.HasError())
                return Result<bool>::Failure(path.ErrorValue());
            const auto settings = Application::LoadPrefabProjectSettings(projectRoot);
            if (settings.HasError())
                return Result<bool>::Failure(settings.ErrorValue());
            auto content = draft.Data();
            content.assetId = asset;
            const auto source = Prefab::PrefabDocument::Create(std::move(content), settings.Value().settings->Limits());
            if (source.HasError())
                return Result<bool>::Failure(source.ErrorValue());
            const auto prepared = PreparePair(source.Value(), projectRoot, destination, outcome, files, locks);
            if (prepared.HasError())
                return Result<bool>::Failure(prepared.ErrorValue());
            const auto written = WritePreparedFiles(prepared.Value(), {}, outcome, files);
            return written.HasError() ? Result<bool>::Failure(written.ErrorValue()) : Result<bool>::Success(true);
        });
    }
}  // namespace Horo::Editor
