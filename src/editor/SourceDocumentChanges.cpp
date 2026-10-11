#include "SourceDocumentInternal.h"

#include <cstddef>
#include <new>

namespace Horo::Editor {
    namespace {
        template <class T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Prevents edit boundaries from splitting a scalar or the retained leading BOM. */
        bool IsBoundary(const std::string_view text, const std::size_t offset) {
            if (offset > text.size())
                return false;
            if (text.starts_with("\xef\xbb\xbf") && offset > 0 && offset < 3)
                return false;
            return offset == text.size() || (static_cast<std::byte>(text[offset]) & std::byte{0xc0}) != std::byte{0x80};
        }

        /** @brief Validates patch arithmetic and reserves a complete new byte buffer before copying borrowed input. */
        Result<std::size_t> EditedSize(const Detail::SourceDocumentRoot &root, const SourceTextEdit &edit, const std::size_t maximum) {
            const auto &text = root.text->bytes;
            if (edit.expectedRevision != root.revision)
                return Failure<std::size_t>(SourceDocumentErrors::Stale);
            if (edit.offset > text.size() || edit.eraseBytes > text.size() - edit.offset || !IsBoundary(text, edit.offset) ||
                !IsBoundary(text, edit.offset + edit.eraseBytes))
                return Failure<std::size_t>(SourceDocumentErrors::Invalid);
            const auto retained = text.size() - edit.eraseBytes;
            if (edit.insert.size() > maximum - retained)
                return Failure<std::size_t>(SourceDocumentErrors::TooLarge);
            return Result<std::size_t>::Success(retained + edit.insert.size());
        }

        /** @brief Maps disk failures into observation metadata without silently accepting unreadable bytes. */
        SourceExternalState FailedObservation(const Error &error) {
            if (error.code.Value() == SourceDocumentErrors::Removed.code.Value())
                return SourceExternalState::Removed;
            if (error.code.Value() == SourceDocumentErrors::DiskChanged.code.Value())
                return SourceExternalState::Changed;
            return SourceExternalState::Unreadable;
        }

        /** @brief Classifies one disk observation against the immutable disk base. */
        SourceExternalState ObservedState(const Result<std::shared_ptr<const Detail::SourceText>> &loaded, const Detail::SourceText &base) {
            if (loaded.HasError())
                return FailedObservation(loaded.ErrorValue());
            return loaded.Value()->bytes == base.bytes ? SourceExternalState::InSync : SourceExternalState::Changed;
        }

        /** @brief Recognizes exact already-clean disk bytes without advancing a revision. */
        bool MatchesCurrentDisk(const Detail::SourceDocumentRoot &root, const Detail::SourceText &disk) {
            return disk.bytes == root.text->bytes && disk.bytes == root.base->bytes && root.external == SourceExternalState::InSync &&
                   !root.saveUnconfirmed;
        }
    }  // namespace

    /** @copydoc SourceDocumentService::Storage::PrepareEdit */
    Result<std::shared_ptr<const Detail::SourceText>> SourceDocumentService::Storage::PrepareEdit(
        const Record &record, const SourceTextEdit &edit, const CancellationToken cancellation) const {
        const auto size = EditedSize(*record.root, edit, limits.maximumDocumentBytes);
        if (size.HasError())
            return Result<std::shared_ptr<const Detail::SourceText>>::Failure(size.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<std::shared_ptr<const Detail::SourceText>>(SourceDocumentErrors::Cancelled);
        if (const auto admitted = AdmitBytes(size.Value()); admitted.HasError())
            return Result<std::shared_ptr<const Detail::SourceText>>::Failure(admitted.ErrorValue());
        const auto &before = record.root->text->bytes;
        std::string bytes;
        bytes.reserve(size.Value());
        bytes.append(before, 0, edit.offset);
        bytes.append(edit.insert);
        bytes.append(before, edit.offset + edit.eraseBytes, std::string::npos);
        return Detail::CaptureSourceText(std::move(bytes), limits.maximumDocumentBytes, cancellation);
    }

    /** @copydoc SourceDocumentService::Storage::ReadRecord */
    Result<std::shared_ptr<const Detail::SourceText>> SourceDocumentService::Storage::ReadRecord(
        const Record &record, const CancellationToken cancellation) const {
        const auto path = Detail::SourcePath(projectRoot, record.root->identity, record.path);
        if (path.HasError())
            return Result<std::shared_ptr<const Detail::SourceText>>::Failure(path.ErrorValue());
        if (const auto admitted = AdmitBytes(limits.maximumDocumentBytes); admitted.HasError())
            return Result<std::shared_ptr<const Detail::SourceText>>::Failure(admitted.ErrorValue());
        return Detail::LoadSourceText(path.Value(), limits.maximumDocumentBytes, cancellation);
    }

    /** @copydoc SourceDocumentService::Edit */
    Result<SourceDocumentSnapshot> SourceDocumentService::Edit(const DocumentInstanceId instance, const SourceTextEdit &edit,
                                                               const CancellationToken cancellation) {
        try {
            const auto current = Snapshot(instance);
            if (current.HasError())
                return current;
            auto &record = *storage_->Find(instance);
            const auto &before = record.root->text->bytes;
            auto text = storage_->PrepareEdit(record, edit, cancellation);
            if (text.HasError())
                return Result<SourceDocumentSnapshot>::Failure(text.ErrorValue());
            if (text.Value()->bytes == before)
                return current;
            auto root = Detail::NextRoot(*record.root);
            if (root.HasError())
                return Result<SourceDocumentSnapshot>::Failure(root.ErrorValue());
            root.Value()->text = text.Value()->bytes == record.root->base->bytes ? record.root->base : std::move(text).Value();
            if (cancellation.IsCancellationRequested())
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
            record.root = std::move(root).Value();
            return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{record.root});
        } catch (const std::bad_alloc &) {
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::InspectDisk */
    Result<SourceDocumentSnapshot> SourceDocumentService::InspectDisk(const DocumentInstanceId instance,
                                                                      const CancellationToken cancellation) {
        try {
            if (const auto current = Snapshot(instance); current.HasError())
                return current;
            auto &record = *storage_->Find(instance);
            if (const auto admitted = storage_->AdmitBytes(storage_->limits.maximumDocumentBytes); admitted.HasError())
                return Result<SourceDocumentSnapshot>::Failure(admitted.ErrorValue());
            const auto path = Detail::SourcePath(storage_->projectRoot, record.root->identity, record.path);
            auto loaded = path.HasError() ? Result<std::shared_ptr<const Detail::SourceText>>::Failure(path.ErrorValue())
                                          : Detail::LoadSourceText(path.Value(), storage_->limits.maximumDocumentBytes, cancellation);
            if (cancellation.IsCancellationRequested())
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
            if (const auto disposition = ObservedState(loaded, *record.root->base); disposition != record.root->external) {
                auto next = Detail::NextRoot(*record.root);
                if (next.HasError())
                    return Result<SourceDocumentSnapshot>::Failure(next.ErrorValue());
                next.Value()->external = disposition;
                if (cancellation.IsCancellationRequested())
                    return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
                record.root = std::move(next).Value();
            }
            if (loaded.HasError())
                return Result<SourceDocumentSnapshot>::Failure(loaded.ErrorValue());
            return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{record.root});
        } catch (const std::bad_alloc &) {
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::Reload */
    Result<SourceDocumentSnapshot> SourceDocumentService::Reload(const DocumentInstanceId instance, const std::uint64_t expectedRevision,
                                                                 const CancellationToken cancellation) {
        try {
            const auto current = Snapshot(instance);
            if (current.HasError())
                return current;
            auto &record = *storage_->Find(instance);
            if (expectedRevision != record.root->revision)
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Stale);
            auto loaded = storage_->ReadRecord(record, cancellation);
            if (loaded.HasError())
                return Result<SourceDocumentSnapshot>::Failure(loaded.ErrorValue());
            if (MatchesCurrentDisk(*record.root, *loaded.Value()))
                return current;
            auto next = Detail::NextRoot(*record.root);
            if (next.HasError())
                return Result<SourceDocumentSnapshot>::Failure(next.ErrorValue());
            next.Value()->text = std::move(loaded).Value();
            next.Value()->base = next.Value()->text;
            next.Value()->baseRevision = next.Value()->revision;
            next.Value()->saveUnconfirmed = false;
            next.Value()->external = SourceExternalState::InSync;
            if (cancellation.IsCancellationRequested())
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
            record.root = std::move(next).Value();
            return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{record.root});
        } catch (const std::bad_alloc &) {
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::TooLarge);
        }
    }
}  // namespace Horo::Editor
