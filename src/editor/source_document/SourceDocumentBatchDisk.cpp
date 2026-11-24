#include "Horo/Foundation/Platform.h"
#include "SourceDocumentInternal.h"

#include <new>

namespace Horo::Editor {
    struct SourceBatchDiskEvidence::Data final {
        struct Item final {
            SourceDocumentSnapshot source;
            std::filesystem::path path;
            FileEntryInfo entry;
        };

        std::filesystem::path root;
        const DurableFileSystem *authority{};
        std::vector<Item> items;
    };

    namespace {
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Reads only native regular-file evidence from the admitted canonical path. */
        Result<FileEntryInfo> ObserveBatchEntry(const DurableFileSystem &files, const std::filesystem::path &path) {
            const auto observed = files.InspectEntry(path);
            if (observed.HasError())
                return Failure<FileEntryInfo>(SourceDocumentErrors::ReadFailed);
            const auto &entry = observed.Value();
            if (!entry || entry->kind != FileEntryKind::RegularFile || !entry->identity)
                return Failure<FileEntryInfo>(SourceDocumentErrors::SaveConflict);
            return Result<FileEntryInfo>::Success(*entry);
        }

        /** @brief Compares captured identity, content metadata and link count. */
        bool SameBatchEntry(const FileEntryInfo &first, const FileEntryInfo &second) noexcept {
            return first.identity == second.identity && first.sizeBytes == second.sizeBytes && first.hardLinks == second.hardLinks;
        }

        /** @brief Captures exact saved bytes between two observations of the same native entry. */
        Result<FileEntryInfo> VerifyDiskCapture(const DurableFileSystem &files, const std::filesystem::path &path,
                                                const std::string_view saved, const CancellationToken cancellation) {
            const auto before = ObserveBatchEntry(files, path);
            if (before.HasError())
                return before;
            if (before.Value().sizeBytes != saved.size())
                return Failure<FileEntryInfo>(SourceDocumentErrors::SaveConflict);
            const auto text = Detail::LoadSourceText(path, SourceDocumentLimits{}.maximumDocumentBytes, cancellation);
            if (text.HasError())
                return Result<FileEntryInfo>::Failure(text.ErrorValue());
            const auto after = ObserveBatchEntry(files, path);
            if (after.HasError())
                return after;
            if (!SameBatchEntry(before.Value(), after.Value()) || text.Value()->bytes != saved)
                return Failure<FileEntryInfo>(SourceDocumentErrors::SaveConflict);
            return after;
        }
    }  // namespace

    /** @copydoc SourceBatchDiskEvidence::Capture */
    Result<SourceBatchDiskEvidence> SourceBatchDiskEvidence::Capture(const std::filesystem::path &root,
                                                                     const std::span<const SourceDocumentSnapshot> sources,
                                                                     const DurableFileSystem &files, const CancellationToken cancellation) {
        using Output = Result<SourceBatchDiskEvidence>;
        try {
            if (!root.is_absolute() || sources.empty() || sources.size() > SourceDocumentLimits{}.maximumDocuments)
                return Failure<SourceBatchDiskEvidence>(SourceDocumentErrors::Invalid);
            auto data = std::make_shared<Data>();
            std::error_code error;
            data->root = std::filesystem::canonical(root, error);
            if (error)
                return Failure<SourceBatchDiskEvidence>(SourceDocumentErrors::ReadFailed);
            data->authority = &files;
            data->items.reserve(sources.size());
            for (const auto &source : sources) {
                if (cancellation.IsCancellationRequested())
                    return Failure<SourceBatchDiskEvidence>(SourceDocumentErrors::Cancelled);
                if (!source.IsCurrent() || source.ExternalState() != SourceExternalState::InSync)
                    return Failure<SourceBatchDiskEvidence>(SourceDocumentErrors::Stale);
                const auto identity = source.Identity();
                const auto &name = identity.key.source.Value();
                const auto path = Detail::SourcePath(data->root, identity, data->root / std::u8string{name.begin(), name.end()});
                if (path.HasError())
                    return Output::Failure(path.ErrorValue());
                const auto entry = VerifyDiskCapture(files, path.Value(), source.DiskBase(), cancellation);
                if (entry.HasError())
                    return Output::Failure(entry.ErrorValue());
                data->items.push_back({source, path.Value(), entry.Value()});
            }
            SourceBatchDiskEvidence evidence;
            evidence.data_ = std::move(data);
            return Output::Success(std::move(evidence));
        } catch (const std::bad_alloc &) {
            return Failure<SourceBatchDiskEvidence>(SourceDocumentErrors::TooLarge);
        } catch (const std::filesystem::filesystem_error &) {
            return Failure<SourceBatchDiskEvidence>(SourceDocumentErrors::ReadFailed);
        }
    }

    /** @copydoc SourceBatchDiskEvidence::RecheckSingle */
    Result<std::filesystem::path> SourceBatchDiskEvidence::RecheckSingle(const std::filesystem::path &root,
                                                                         const SourceDocumentSnapshot &source,
                                                                         const DurableFileSystem &files,
                                                                         const CancellationToken cancellation) const {
        using Output = Result<std::filesystem::path>;
        if (cancellation.IsCancellationRequested())
            return Failure<std::filesystem::path>(SourceDocumentErrors::Cancelled);
        if (!data_ || data_->authority != &files || data_->root != root || data_->items.size() != 1 || !source.IsCurrent())
            return Failure<std::filesystem::path>(SourceDocumentErrors::Stale);
        const auto &item = data_->items.front();
        if (!item.source.SharesStateWith(source) || item.entry.hardLinks != 1)
            return Failure<std::filesystem::path>(SourceDocumentErrors::Stale);
        const auto path = Detail::SourcePath(root, source.Identity(), item.path);
        if (path.HasError() || path.Value() != item.path)
            return Failure<std::filesystem::path>(SourceDocumentErrors::SaveConflict);
        const auto current = ObserveBatchEntry(files, item.path);
        if (current.HasError())
            return Output::Failure(current.ErrorValue());
        if (!SameBatchEntry(item.entry, current.Value()))
            return Failure<std::filesystem::path>(SourceDocumentErrors::SaveConflict);
        return path;
    }

    /** @copydoc SourceDocumentService::Storage::VerifyBatchEvidence */
    Result<void> SourceDocumentService::Storage::VerifyBatchEvidence(const StagedBatch &batch, const SourceBatchDiskEvidence &evidence,
                                                                     const DurableFileSystem &files,
                                                                     const CancellationToken cancellation) const {
        const auto &data = evidence.data_;
        if (!data || data->authority != &files || data->root != projectRoot || data->items.size() != batch.items.size())
            return Failure<void>(SourceDocumentErrors::Stale);
        for (std::size_t index = 0; index < batch.items.size(); ++index) {
            if (cancellation.IsCancellationRequested())
                return Failure<void>(SourceDocumentErrors::Cancelled);
            const auto &record = *batch.items[index].record;
            const auto &expected = data->items[index];
            if (record.path != expected.path || !expected.source.SharesStateWith(SourceDocumentSnapshot{record.root}) ||
                record.root->saveUnconfirmed)
                return Failure<void>(SourceDocumentErrors::Stale);
            const auto path = Detail::SourcePath(projectRoot, record.root->identity, record.path);
            if (path.HasError() || path.Value() != expected.path)
                return Failure<void>(SourceDocumentErrors::SaveConflict);
            const auto current = ObserveBatchEntry(files, expected.path);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!SameBatchEntry(expected.entry, current.Value()))
                return Failure<void>(SourceDocumentErrors::SaveConflict);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
