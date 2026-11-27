#include "Horo/Foundation/Platform.h"
#include "SourceDocumentInternal.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

namespace Horo::Editor {
    /** @copydoc SourceDocumentService::UpdateAutosave */
    std::optional<Result<SourceSaveResult>> SourceDocumentService::UpdateAutosave(const float elapsedSeconds, const int delaySeconds,
                                                                                  DurableFileSystem &files) {
        if (const auto checked = storage_->Check(); checked.HasError())
            return Result<SourceSaveResult>::Failure(checked.ErrorValue());
        if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F)
            return std::nullopt;
        for (auto &record : storage_->records) {
            if (delaySeconds <= 0) {
                record.autosaveRetry = 0;
                record.autosaveElapsed = 0;
                record.autosaveRevision = 0;
                continue;
            }
            if (record.autosaveRevision != record.root->revision) {
                record.autosaveRevision = record.root->revision;
                record.autosaveDirty = !record.root->draft && SourceDocumentSnapshot{record.root}.Dirty();
                record.autosaveElapsed = 0;
            } else if (record.autosaveDirty) {
                record.autosaveElapsed = std::min(300.0F, record.autosaveElapsed + elapsedSeconds);
            }
            record.autosaveRetry = std::max(0.0F, record.autosaveRetry - elapsedSeconds);
        }
        if (delaySeconds <= 0)
            return std::nullopt;
        for (auto &record : storage_->records) {
            if (!record.autosaveDirty || record.autosaveRetry > 0 || record.autosaveElapsed < std::clamp(delaySeconds, 1, 300))
                continue;
            record.autosaveElapsed = 0;
            auto result = Save({record.root->identity.instance, record.root->revision, std::nullopt}, files);
            record.autosaveRetry = result.HasError() || result.Value().disposition != SourceSaveDisposition::Durable ? 30.0F : 0.0F;
            return result;
        }
        return std::nullopt;
    }

    static_assert(std::is_nothrow_move_constructible_v<SourceSaveResult>);
    static_assert(std::is_nothrow_move_assignable_v<Result<SourceSaveResult>>);
    static_assert(std::is_nothrow_move_assignable_v<Error>);
    static_assert(std::is_nothrow_move_constructible_v<Error>);
    static_assert(std::is_nothrow_destructible_v<Error>);

    namespace {
        /** @brief Admits only a payload in a real attempt directory directly below this project's Trash. */
        bool IsSourceTrashPayload(const std::filesystem::path &root, const std::filesystem::path &payload) {
            const auto attempt = payload.parent_path();
            std::error_code error;
            return payload.filename() == "payload" && attempt.parent_path() == root / ".horo/local/trash" &&
                   attempt.filename().generic_u8string().starts_with(u8"source-review-") &&
                   std::filesystem::canonical(attempt, error) == attempt && !error;
        }

        const ErrorCodeDescriptor ReadOnly{.domain = ErrorDomainId{"horo.editor.source"},
                                           .code = ErrorCode{"save.read_only"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The source destination is read-only.",
                                           .remediationHint = "Change destination permissions or choose Save As.",
                                           .userActionable = true};

        /** @brief Reports cleanup failure without replacing the primary save outcome or throwing during unwind. */
        bool TryRemoveTemporary(DurableFileSystem &files, const std::filesystem::path &path) noexcept {
            try {
                return files.RemoveDurable(path).HasValue();
            } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): ADR-008 forbids callback exceptions escaping destructor cleanup.
                return false;
            }
        }

        /** @brief Removes only an exclusively created temporary owned by this transaction. */
        struct TemporaryGuard final {
            DurableFileSystem &files;
            const std::filesystem::path &path;
            bool created{};

            TemporaryGuard(DurableFileSystem &owner, const std::filesystem::path &temporary) noexcept : files(owner), path(temporary) {}

            TemporaryGuard(const TemporaryGuard &) = delete;
            TemporaryGuard &operator=(const TemporaryGuard &) = delete;

            ~TemporaryGuard() noexcept {
                if (!created)
                    return;
                static_cast<void>(TryRemoveTemporary(files, path));
            }
        };

        /** @brief Compares disk against exact user-approved bytes; absent Save As targets are distinct from empty files. */
        Result<void> CheckDisk(const std::filesystem::path &path, const std::string_view expected, const bool allowAbsent,
                               const std::size_t maximum, const CancellationToken cancellation) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (status.type() == std::filesystem::file_type::not_found && allowAbsent)
                return Result<void>::Success();
            if (error || !std::filesystem::is_regular_file(status))
                return Result<void>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            // Save As without overwrite consent requires absence, even when a newly
            // appeared destination happens to contain the original source bytes.
            if (allowAbsent)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::SaveConflict));
            auto disk = Detail::LoadSourceText(path, maximum, cancellation);
            if (disk.HasError())
                return Result<void>::Failure(disk.ErrorValue());
            if (disk.Value()->bytes != expected)
                return Result<void>::Failure(MakeError(SourceDocumentErrors::SaveConflict));
            return Result<void>::Success();
        }

        /** @brief Captures destination permissions without granting overwrite authority. */
        Result<std::filesystem::perms> DestinationPermissions(const std::filesystem::path &path) {
            using enum std::filesystem::perms;
            std::error_code error;
            if (!std::filesystem::exists(path, error)) {
                if (error)
                    return Result<std::filesystem::perms>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
                return Result<std::filesystem::perms>::Success(unknown);
            }
            const auto permissions = std::filesystem::status(path, error).permissions();
            if (error)
                return Result<std::filesystem::perms>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
            if (constexpr auto writable = owner_write | group_write | others_write; (permissions & writable) == none)
                return Result<std::filesystem::perms>::Failure(MakeError(ReadOnly));
            return Result<std::filesystem::perms>::Success(permissions);
        }

        /** @brief Borrows the captured revision and its exact expected disk base for one save operation. */
        struct SaveBytes final {
            std::string_view text;
            std::string_view expected;
        };

        /** @brief Prepares exclusive bytes between two exact conflict checks, then fences cancellation. */
        Result<void> PrepareBytes(DurableFileSystem &files, const std::filesystem::path &path, TemporaryGuard &cleanup,
                                  const SaveBytes &contents, const bool absentAllowed, const std::size_t maximum,
                                  const CancellationToken cancellation) {
            auto observed = CheckDisk(path, contents.expected, absentAllowed, maximum, cancellation);
            if (observed.HasError())
                return observed;
            const auto permissions = DestinationPermissions(path);
            if (permissions.HasError())
                return Result<void>::Failure(permissions.ErrorValue());
            const auto bytes = std::span{contents.text.data(), contents.text.size()};
            if (auto written = files.WritePrivateDurable(cleanup.path, std::as_bytes(bytes), cleanup.created, permissions.Value());
                written.HasError())
                return written;
            observed = CheckDisk(path, contents.expected, absentAllowed, maximum, cancellation);
            if (observed.HasError())
                return observed;
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(SourceDocumentErrors::Cancelled));
            return Result<void>::Success();
        }

        /** @brief Records visible publication using only preallocated state; false leaves the original exception active. */
        bool PreserveVisibleFailure(const AtomicFileReplacementReceipt &receipt, TemporaryGuard &cleanup, SourceSaveResult &result,
                                    Error &fallback) noexcept {
            if (!receipt.WasCommitted())
                return false;
            cleanup.created = false;
            result.disposition = SourceSaveDisposition::VisibleDurabilityUnconfirmed;
            result.diagnostic.emplace(std::move(fallback));
            return true;
        }

        /** @brief Uses the native commit receipt to preserve visible state even when durability or callbacks fail. */
        Result<void> ReplacePrepared(DurableFileSystem &files, const std::filesystem::path &path, TemporaryGuard &cleanup,
                                     Detail::SourceDocumentRoot &prepared, SourceSaveResult &result, Error fallback,
                                     const bool requireAbsent) {
            AtomicFileReplacementReceipt receipt;
            try {
                auto replaced = requireAbsent ? files.PublishFileNoReplaceTracked(cleanup.path, path, receipt)
                                              : files.AtomicReplaceTracked(cleanup.path, path, receipt);
                if (!receipt.WasCommitted()) {
                    if (requireAbsent) {
                        const auto current = files.InspectEntry(path);
                        if (current.HasValue() && current.Value())
                            return Result<void>::Failure(MakeError(SourceDocumentErrors::SaveConflict));
                    }
                    if (replaced.HasError())
                        return replaced;
                    return Result<void>::Failure(MakeError(SourceDocumentErrors::Invalid));
                }
                cleanup.created = false;
                if (replaced.HasError()) {
                    result.disposition = SourceSaveDisposition::VisibleDurabilityUnconfirmed;
                    result.diagnostic = std::move(replaced).ErrorValue();
                } else {
                    prepared.base = prepared.text;
                    prepared.baseRevision = prepared.revision;
                    prepared.draft = false;
                    prepared.external = SourceExternalState::InSync;
                    prepared.saveUnconfirmed = false;
                }
            } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): the Platform receipt must preserve any postcommit exception.
                if (!PreserveVisibleFailure(receipt, cleanup, result, fallback))
                    throw;
            }
            return Result<void>::Success();
        }

        /** @brief Keeps a preallocated per-document failure if constructing a later save outcome runs out of memory. */
        void CompleteSaveItem(SourceDocumentService &documents, SourceSaveAllItem &item, const SourceSaveRequest &request,
                              DurableFileSystem &files, const CancellationToken &cancellation) {
            try {
                item.result = documents.Save(request, files, cancellation);
            } catch (const std::bad_alloc &) {
                // Earlier publications and this item's preallocated error remain observable.
                return;
            }
        }
    }  // namespace

    /** @copydoc SourceDocumentService::Save */
    Result<SourceSaveResult> SourceDocumentService::Save(const SourceSaveRequest &request, DurableFileSystem &files,
                                                         const CancellationToken cancellation) {
        try {
            if (const auto current = Snapshot(request.instance); current.HasError())
                return Result<SourceSaveResult>::Failure(current.ErrorValue());
            const auto &record = *storage_->Find(request.instance);
            return SaveTo(request, files, record.root->identity, record.path, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc SourceDocumentService::Storage::PrepareSavePath */
    Result<std::filesystem::path> SourceDocumentService::Storage::PrepareSavePath(const Record &record, const SourceSaveRequest &request,
                                                                                  const DocumentIdentity &identity,
                                                                                  std::filesystem::path path,
                                                                                  const CancellationToken cancellation) const {
        if (cancellation.IsCancellationRequested())
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::Cancelled));
        if (record.root->revision != request.expectedRevision || identity.instance != request.instance)
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::Stale));
        if (record.root->saveUnconfirmed && !request.approvedDiskBytes)
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::SaveOutcomeUnknown));
        if (record.root->draft && !record.root->saveUnconfirmed && request.approvedDiskBytes)
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::SaveConflict));
        if (request.approvedDiskBytes && request.approvedDiskBytes->size() > limits.maximumDocumentBytes)
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        auto safe = Detail::SourcePath(projectRoot, identity, path);
        if (safe.HasError())
            return Result<std::filesystem::path>::Failure(safe.ErrorValue());
        path = std::move(safe).Value();
        std::error_code error;
        if (!std::filesystem::is_directory(path.parent_path(), error) || error)
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
        if (const bool relocating = path != record.path; relocating && !request.approvedDiskBytes && std::filesystem::exists(path, error))
            return Result<std::filesystem::path>::Failure(MakeError(SourceDocumentErrors::SaveConflict));
        if (const auto admitted = AdmitBytes(limits.maximumDocumentBytes); admitted.HasError())
            return Result<std::filesystem::path>::Failure(admitted.ErrorValue());
        return Result<std::filesystem::path>::Success(std::move(path));
    }

    /** @copydoc SourceDocumentService::SaveTo */
    Result<SourceSaveResult> SourceDocumentService::SaveTo(const SourceSaveRequest &request, DurableFileSystem &files,
                                                           DocumentIdentity identity, std::filesystem::path path,
                                                           const CancellationToken cancellation) {
        try {
            if (const auto current = Snapshot(request.instance); current.HasError())
                return Result<SourceSaveResult>::Failure(current.ErrorValue());
            auto &record = *storage_->Find(request.instance);
            auto safe = storage_->PrepareSavePath(record, request, identity, std::move(path), cancellation);
            if (safe.HasError())
                return Result<SourceSaveResult>::Failure(safe.ErrorValue());
            path = std::move(safe).Value();
            const std::string_view expected = request.approvedDiskBytes ? *request.approvedDiskBytes : record.root->base->bytes;
            const bool absentAllowed = (path != record.path || record.root->draft) && !request.approvedDiskBytes;
            auto next = Detail::NextRoot(*record.root);
            if (next.HasError())
                return Result<SourceSaveResult>::Failure(next.ErrorValue());
            auto prepared = std::move(next).Value();
            prepared->identity = std::move(identity);
            prepared->saveUnconfirmed = true;
            Error fallback = MakeError(SourceDocumentErrors::SaveOutcomeUnknown);
            SourceSaveResult result{SourceDocumentSnapshot{prepared}};
            auto lockPath = path;
            lockPath += ".horo-source.lock";
            auto temporary = path;
            temporary += ".horo-source.temporary";
            Detail::SourcePublicationGuard publication{storage_->saving};
            if (auto lock = files.TryAcquireExclusive(lockPath, "horo.source_document"); lock.HasError()) {
                return Result<SourceSaveResult>::Failure(lock.ErrorValue());
            } else {
                TemporaryGuard cleanup{files, temporary};
                if (const auto written = PrepareBytes(files, path, cleanup, SaveBytes{prepared->text->bytes, expected}, absentAllowed,
                                                      storage_->limits.maximumDocumentBytes, cancellation);
                    written.HasError())
                    return Result<SourceSaveResult>::Failure(written.ErrorValue());
                if (const auto replaced = ReplacePrepared(files, path, cleanup, *prepared, result, std::move(fallback), absentAllowed);
                    replaced.HasError())
                    return Result<SourceSaveResult>::Failure(replaced.ErrorValue());
                if (result.disposition == SourceSaveDisposition::Durable && path == record.path &&
                    prepared->identity == record.root->identity)
                    record.root->savedInPlace.store(true);
                record.path.swap(path);
                record.Publish(std::move(prepared));
                return Result<SourceSaveResult>::Success(std::move(result));
            }
        } catch (const std::bad_alloc &) {
            return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): ADR-008 requires every provider exception to become a typed error.
            return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
        }
    }

    namespace {
        /** @brief Checks portable filename aliases while the caller holds the destination lease. */
        Result<void> RequireFreeRenameDestination(const std::filesystem::path &path, const DurableFileSystem &files,
                                                  const CancellationToken &stop) {
            constexpr auto MaximumDirectoryEntries = 8192;
            const auto entries = files.ListDirectory(path.parent_path(), MaximumDirectoryEntries, stop);
            if (entries.HasError())
                return Result<void>::Failure(entries.ErrorValue());
            const auto name = path.filename().generic_u8string();
            const auto fold = [](const char8_t byte) {
                return byte >= u8'A' && byte <= u8'Z' ? static_cast<char8_t>(byte + (u8'a' - u8'A')) : byte;
            };
            if (std::ranges::any_of(entries.Value(), [&](const auto &entry) {
                return std::ranges::equal(entry.name.generic_u8string(), name, {}, fold, fold);
            }))
                return Result<void>::Failure(MakeError(SourceDocumentErrors::SaveConflict));
            return Result<void>::Success();
        }

        /** @brief Retains native visibility even when directory synchronization fails or a provider throws. */
        const ErrorCodeDescriptor *MoveReviewedPath(DurableFileSystem &files, const std::filesystem::path &from,
                                                    const std::filesystem::path &to, AtomicFileReplacementReceipt &receipt) noexcept {
            try {
                const auto moved = files.MoveFileNoReplaceTracked(from, to, receipt);
                if (receipt.WasCommitted())
                    return moved.HasValue() ? nullptr : &SourceDocumentErrors::ReadFailed;
                if (moved.HasError()) {
                    const auto code = moved.ErrorValue().code.Value();
                    if (code == DurableFileErrors::CrossVolume.code.Value())
                        return &DurableFileErrors::CrossVolume;
                    if (code == DurableFileErrors::MoveUnsupported.code.Value())
                        return &DurableFileErrors::MoveUnsupported;
                }
            } catch (...) {
                // The receipt below still records a visible move if a provider threw after publication.
            }
            return receipt.WasCommitted() ? &SourceDocumentErrors::ReadFailed : &SourceDocumentErrors::SaveConflict;
        }

    }  // namespace

    /** @copydoc SourceDocumentService::Storage::StageRename */
    Result<SourceDocumentService::Storage::StagedMove> SourceDocumentService::Storage::StageRename(const RenameRequest &request,
                                                                                                   const CancellationToken cancellation,
                                                                                                   const std::size_t reservedBytes) {
        using Output = Result<StagedMove>;
        auto *record = FindCurrent(request.from);
        if (!record || record->root->identity.instance != request.to.instance || record->root->draft || record->root->saveUnconfirmed)
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto destination = Detail::SourcePath(projectRoot, request.to, request.destination);
        if (destination.HasError() || destination.Value() != request.destination || record->path == request.destination)
            return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
        const SourceTextEdit edit{request.from.Revision(), 0, request.from.Text().size(), request.text};
        auto stage = StageBatchEdit(*record, edit, cancellation, reservedBytes);
        if (stage.HasError())
            return Output::Failure(stage.ErrorValue());
        auto prepared = std::move(stage).Value();
        auto next = Detail::NextRoot(*record->root);
        if (next.HasError())
            return Output::Failure(next.ErrorValue());
        auto renamed = std::move(next).Value();
        renamed->identity = request.to;
        renamed->text = prepared.next->text;
        prepared.next = std::move(renamed);
        return Output::Success({std::move(prepared), request.from, &request.disk, request.destination, {}, {}});
    }

    /** @copydoc SourceDocumentService::Storage::StageDelete */
    Result<SourceDocumentService::Storage::StagedMove> SourceDocumentService::Storage::StageDelete(const DeleteRequest &request) {
        using Output = Result<StagedMove>;
        auto *record = FindCurrent(request.from);
        if (!record || request.from.Dirty() || record->root->draft)
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        if (!IsSourceTrashPayload(projectRoot, request.payload))
            return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
        return Output::Success({{record, {}, {}}, request.from, &request.disk, request.payload, {}, {}});
    }

    /** @copydoc SourceDocumentService::Storage::StageTrashRestore */
    Result<SourceDocumentService::Storage::StagedMove> SourceDocumentService::Storage::StageTrashRestore(
        const SourceReviewTrashRestore &restore) const {
        using Output = Result<StagedMove>;
        const auto original = restore.original.Identity();
        if (restore.original.IsCurrent() || !original.IsValid() || !IsSourceTrashPayload(projectRoot, restore.payload))
            return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
        const auto &name = original.key.source.Value();
        auto target = projectRoot / std::u8string{name.begin(), name.end()};
        const auto allowed = Detail::SourcePath(projectRoot, original, target);
        if (allowed.HasError() || allowed.Value() != target)
            return Output::Failure(MakeError(SourceDocumentErrors::SaveConflict));
        return Output::Success({{}, restore.original, nullptr, std::move(target), {}, {}, &restore});
    }

    /** @copydoc SourceDocumentService::Storage::LockMove */
    Result<void> SourceDocumentService::Storage::LockMove(StagedMove &rename, DurableFileSystem &files,
                                                          const CancellationToken cancellation) const {
        const auto free = RequireFreeRenameDestination(rename.destination, files, cancellation);
        if (free.HasError())
            return free;
        if (!rename.restore) {
            auto sourceLock = rename.From();
            sourceLock += ".horo-source.lock";
            auto source = files.TryAcquireExclusive(sourceLock, "horo.source_document");
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            rename.sourceLease = std::move(source).Value();
        }
        // A Trash attempt is privately reserved by the project mutation owner.
        if (!rename.edit.next && !rename.restore)
            return Result<void>::Success();
        auto targetLock = rename.destination;
        targetLock += ".horo-source.lock";
        auto target = files.TryAcquireExclusive(targetLock, "horo.source_document");
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        rename.destinationLease = std::move(target).Value();
        return Result<void>::Success();
    }

    /** @copydoc SourceDocumentService::Storage::TrySourceMove */
    const ErrorCodeDescriptor *SourceDocumentService::Storage::TrySourceMove(const StagedMove &move, DurableFileSystem &files,
                                                                             AtomicFileReplacementReceipt &receipt,
                                                                             const CancellationToken cancellation) const noexcept {
        using namespace SourceDocumentErrors;
        try {
            if (cancellation.IsCancellationRequested())
                return &Cancelled;
            if (move.edit.next || move.restore) {
                const auto identity = move.restore ? move.source.Identity() : move.edit.next->identity;
                const auto destination = Detail::SourcePath(projectRoot, identity, move.destination);
                if (destination.HasError() || destination.Value() != move.destination)
                    return &SaveConflict;
            } else if (!IsSourceTrashPayload(projectRoot, move.destination)) {
                return &SaveConflict;
            }
            if (RequireFreeRenameDestination(move.destination, files, cancellation).HasError())
                return cancellation.IsCancellationRequested() ? &Cancelled : &SaveConflict;
            if (move.restore) {
                if (!IsSourceTrashPayload(projectRoot, move.From()))
                    return &SaveConflict;
                const auto entry = files.InspectEntry(move.From());
                if (entry.HasError() || !entry.Value() || entry.Value()->kind != FileEntryKind::RegularFile ||
                    entry.Value()->hardLinks != 1 || entry.Value()->identity != move.restore->identity)
                    return &SaveConflict;
            } else if (move.disk->RecheckSingle(projectRoot, move.source, files, cancellation).HasError()) {
                return cancellation.IsCancellationRequested() ? &Cancelled : &SaveConflict;
            }
            return MoveReviewedPath(files, move.From(), move.destination, receipt);
        } catch (const std::bad_alloc &) {
            return &TooLarge;
        } catch (...) {
            return &ReadFailed;
        }
    }

    /** @copydoc SourceDocumentService::Storage::MoveSources */
    Result<void> SourceDocumentService::Storage::MoveSources(const std::span<StagedMove> renames, DurableFileSystem &files,
                                                             const CancellationToken cancellation, const SourceBatchDiskEvidence *evidence,
                                                             const StagedBatch *edits) const {
        std::size_t visible{};
        const ErrorCodeDescriptor *failure{};
        for (const auto &move : renames) {
            AtomicFileReplacementReceipt receipt;
            failure = TrySourceMove(move, files, receipt, cancellation);
            visible += static_cast<std::size_t>(receipt.WasCommitted());
            if (failure)
                break;
        }
        if (!failure && edits && !edits->items.empty()) {
            try {
                if (VerifyBatchEvidence(*edits, *evidence, files, cancellation).HasError())
                    failure =
                        cancellation.IsCancellationRequested() ? &SourceDocumentErrors::Cancelled : &SourceDocumentErrors::SaveConflict;
            } catch (...) {
                failure = &SourceDocumentErrors::ReadFailed;
            }
        }
        if (!failure && cancellation.IsCancellationRequested())
            failure = &SourceDocumentErrors::Cancelled;
        if (!failure)
            return Result<void>::Success();
        while (visible > 0) {
            const auto &move = renames[--visible];
            AtomicFileReplacementReceipt restored;
            if (MoveReviewedPath(files, move.destination, move.From(), restored))
                failure = &SourceDocumentErrors::SaveOutcomeUnknown;
        }
        return Result<void>::Failure(MakeError(*failure));
    }

    /** @copydoc SourceDocumentService::Storage::PublishMoves */
    void SourceDocumentService::Storage::PublishMoves(const std::span<StagedMove> moves) noexcept {
        bool deleted{};
        for (auto &rename : moves) {
            if (rename.restore)
                continue;
            auto &edit = rename.edit;
            if (!edit.next) {
                deleted = true;
                continue;
            }
            if (edit.history)
                CommitHistory(*edit.record, std::move(*edit.history));
            edit.record->path.swap(rename.destination);
            edit.record->Publish(std::move(edit.next));
        }
        if (deleted) {
            static_assert(std::is_nothrow_move_assignable_v<Record>);
            std::erase_if(records, [&](const auto &record) noexcept {
                const auto snapshot = SourceDocumentSnapshot{record.root};
                if (!std::ranges::any_of(moves, [&](const auto &move) {
                    return move.source.SharesStateWith(snapshot);
                }))
                    return false;
                record.Revoke();
                return true;
            });
        }
    }

    /** @copydoc SourceDocumentService::RenameReviewed */
    Result<SourceDocumentSnapshot> SourceDocumentService::RenameReviewed(const RenameRequest &request, DurableFileSystem &files,
                                                                         const CancellationToken cancellation) {
        using Output = Result<SourceDocumentSnapshot>;
        try {
            if (const auto access = CheckAccess(); access.HasError())
                return Output::Failure(access.ErrorValue());
            auto staged = storage_->StageRename(request, cancellation, 0);
            if (staged.HasError())
                return Output::Failure(staged.ErrorValue());
            auto rename = std::move(staged).Value();
            auto result = Output::Success(SourceDocumentSnapshot{rename.edit.next});
            Detail::SourcePublicationGuard guard{storage_->saving};
            if (const auto locked = storage_->LockMove(rename, files, cancellation); locked.HasError())
                return Output::Failure(locked.ErrorValue());
            const std::span group{&rename, 1};
            if (const auto moved = storage_->MoveSources(group, files, cancellation); moved.HasError())
                return Output::Failure(moved.ErrorValue());
            storage_->PublishMoves(group);
            return result;
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }

    /** @copydoc SourceDocumentService::SaveAll */
    Result<std::vector<SourceSaveAllItem>> SourceDocumentService::SaveAll(DurableFileSystem &files, const CancellationToken cancellation) {
        try {
            if (auto valid = CheckAccess(); valid.HasError())
                return Result<std::vector<SourceSaveAllItem>>::Failure(valid.ErrorValue());
            std::vector<SourceSaveRequest> requests;
            std::vector<SourceSaveAllItem> results;
            requests.reserve(storage_->records.size());
            results.reserve(storage_->records.size());
            for (const auto &record : storage_->records)
                if (record.root->saveUnconfirmed || record.root->text->bytes != record.root->base->bytes)
                    requests.emplace_back(record.root->identity.instance, record.root->revision, std::nullopt);
            // Reserve every result/error before any disk publication, so a later
            // allocation failure cannot erase evidence of earlier successful saves.
            for (const auto &request : requests)
                results.emplace_back(request.instance, Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::TooLarge)));
            for (std::size_t index = 0; index < requests.size(); ++index)
                CompleteSaveItem(*this, results[index], requests[index], files, cancellation);
            return Result<std::vector<SourceSaveAllItem>>::Success(std::move(results));
        } catch (const std::bad_alloc &) {
            return Result<std::vector<SourceSaveAllItem>>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        }
    }
}  // namespace Horo::Editor
