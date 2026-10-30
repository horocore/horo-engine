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
                record.autosaveElapsed = record.autosaveRetry = 0;
                record.autosaveRevision = 0;
                continue;
            }
            if (record.autosaveRevision != record.root->revision) {
                record.autosaveRevision = record.root->revision;
                record.autosaveDirty = SourceDocumentSnapshot{record.root}.Dirty();
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
        const ErrorCodeDescriptor ReadOnly{.domain = ErrorDomainId{"horo.editor.source"},
                                           .code = ErrorCode{"save.read_only"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "The source destination is read-only.",
                                           .remediationHint = "Change destination permissions or choose Save As.",
                                           .userActionable = true};

        /** @brief Holds admission closed while filesystem/provider callbacks execute. */
        struct PublicationGuard final {
            bool *busy;

            explicit PublicationGuard(bool &value) noexcept : busy(&value) {
                *busy = true;
            }

            ~PublicationGuard() noexcept {
                *busy = false;
            }

            PublicationGuard(const PublicationGuard &) = delete;
            PublicationGuard &operator=(const PublicationGuard &) = delete;
        };

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
                                     Detail::SourceDocumentRoot &prepared, SourceSaveResult &result, Error fallback) {
            AtomicFileReplacementReceipt receipt;
            try {
                auto replaced = files.AtomicReplaceTracked(cleanup.path, path, receipt);
                if (!receipt.WasCommitted()) {
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
            const bool absentAllowed = path != record.path && !request.approvedDiskBytes;
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
            PublicationGuard publication{storage_->saving};
            if (auto lock = files.TryAcquireExclusive(lockPath, "horo.source_document"); lock.HasError()) {
                return Result<SourceSaveResult>::Failure(lock.ErrorValue());
            } else {
                TemporaryGuard cleanup{files, temporary};
                if (const auto written = PrepareBytes(files, path, cleanup, SaveBytes{prepared->text->bytes, expected}, absentAllowed,
                                                      storage_->limits.maximumDocumentBytes, cancellation);
                    written.HasError())
                    return Result<SourceSaveResult>::Failure(written.ErrorValue());
                if (const auto replaced = ReplacePrepared(files, path, cleanup, *prepared, result, std::move(fallback));
                    replaced.HasError())
                    return Result<SourceSaveResult>::Failure(replaced.ErrorValue());
                record.path.swap(path);
                record.root = std::move(prepared);
                return Result<SourceSaveResult>::Success(std::move(result));
            }
        } catch (const std::bad_alloc &) {
            return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::TooLarge));
        } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738): ADR-008 requires every provider exception to become a typed error.
            return Result<SourceSaveResult>::Failure(MakeError(SourceDocumentErrors::ReadFailed));
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
