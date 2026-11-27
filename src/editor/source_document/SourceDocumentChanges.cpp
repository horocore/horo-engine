#include "SourceDocumentInternal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <new>
#include <type_traits>

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
    Result<std::shared_ptr<const Detail::SourceText>> SourceDocumentService::Storage::PrepareEdit(const Record &record,
                                                                                                  const SourceTextEdit &edit,
                                                                                                  const CancellationToken cancellation,
                                                                                                  const std::size_t reservedBytes) const {
        const auto size = EditedSize(*record.root, edit, limits.maximumDocumentBytes);
        if (size.HasError())
            return Result<std::shared_ptr<const Detail::SourceText>>::Failure(size.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Failure<std::shared_ptr<const Detail::SourceText>>(SourceDocumentErrors::Cancelled);
        if (const auto admitted = AdmitBytes(reservedBytes + size.Value()); admitted.HasError())
            return Result<std::shared_ptr<const Detail::SourceText>>::Failure(admitted.ErrorValue());
        const auto &before = record.root->text->bytes;
        // The throwing constructor keeps Debug iterator-proxy allocation inside the edit failure boundary.
        std::string bytes{"", 0};
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

    /** @copydoc SourceDocumentService::Storage::PrepareHistory */
    Result<SourceDocumentService::Storage::HistoryEntry> SourceDocumentService::Storage::PrepareHistory(
        Record &record, const SourceTextEdit &edit, const Detail::SourceText &after) const {
        const auto &before = record.root->text->bytes;
        HistoryEntry entry{edit.offset, before.substr(edit.offset, edit.eraseBytes), std::string(edit.insert),
                           edit.beforeView.value_or(SourceEditingView{edit.offset, edit.offset, 0}),
                           edit.afterView.value_or(
                               SourceEditingView{edit.offset + edit.insert.size(), edit.offset + edit.insert.size(), 0})};
        if (const auto validView = [](const SourceEditingView &view, const std::string_view bytes) {
            return IsBoundary(bytes, view.anchor) && IsBoundary(bytes, view.caret) && std::isfinite(view.scrollLines);
        }; !validView(entry.before, before) || !validView(entry.after, after.bytes) || entry.before.viewId != entry.after.viewId)
            return Failure<HistoryEntry>(SourceDocumentErrors::Invalid);
        if (limits.maximumHistoryEntries != 0 && entry.Bytes() <= limits.maximumHistoryBytes)
            record.history.reserve(record.historyPosition + 1);
        return Result<HistoryEntry>::Success(std::move(entry));
    }

    /** @copydoc SourceDocumentService::Storage::CommitHistory */
    void SourceDocumentService::Storage::CommitHistory(Record &record, HistoryEntry entry) const noexcept {
        const bool retain = limits.maximumHistoryEntries != 0 && entry.Bytes() <= limits.maximumHistoryBytes;
        while (record.history.size() > record.historyPosition) {
            record.historyBytes -= record.history.back().Bytes();
            record.history.pop_back();
        }
        while (!record.history.empty() && (!retain || record.history.size() >= limits.maximumHistoryEntries ||
                                           entry.Bytes() > limits.maximumHistoryBytes - record.historyBytes)) {
            record.historyBytes -= record.history.front().Bytes();
            record.history.erase(record.history.begin());
            record.historyBoundary = SourceHistoryBoundary::Evicted;
        }
        if (!retain)
            record.historyBoundary = SourceHistoryBoundary::OversizedEdit;
        if (retain) {
            record.historyBytes += entry.Bytes();
            record.history.push_back(std::move(entry));
        }
        record.historyPosition = record.history.size();
    }

    /** @copydoc SourceDocumentService::Storage::Record::Step */
    std::optional<SourceDocumentService::Storage::HistoryStep> SourceDocumentService::Storage::Record::Step(const bool redo) const {
        if (redo ? historyPosition == history.size() : historyPosition == 0)
            return {};
        if (redo) {
            const auto &entry = history[historyPosition];
            return HistoryStep{{root->revision, entry.offset, entry.removed.size(), entry.inserted}, entry.after, historyPosition + 1};
        }
        const auto &entry = history[historyPosition - 1];
        return HistoryStep{{root->revision, entry.offset, entry.inserted.size(), entry.removed}, entry.before, historyPosition - 1};
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
            auto entry = storage_->PrepareHistory(record, edit, *root.Value()->text);
            if (entry.HasError())
                return Result<SourceDocumentSnapshot>::Failure(entry.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
            storage_->CommitHistory(record, std::move(entry).Value());
            record.Publish(std::move(root).Value());
            return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{record.root});
        } catch (const std::bad_alloc &) {
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::Storage::FindCurrent */
    SourceDocumentService::Storage::Record *SourceDocumentService::Storage::FindCurrent(const SourceDocumentSnapshot &snapshot) {
        auto *record = Find(snapshot.Identity().instance);
        return record && SourceDocumentSnapshot{record->root}.SharesStateWith(snapshot) ? record : nullptr;
    }

    /** @copydoc SourceDocumentService::Storage::VerifyBatchBase */
    Result<void> SourceDocumentService::Storage::VerifyBatchBase(const Record &record, const CancellationToken cancellation) const {
        if (record.root->external != SourceExternalState::InSync || record.root->saveUnconfirmed)
            return Failure<void>(SourceDocumentErrors::SaveConflict);
        const auto disk = ReadRecord(record, cancellation);
        if (disk.HasError())
            return Result<void>::Failure(disk.ErrorValue());
        if (disk.Value()->bytes != record.root->base->bytes)
            return Failure<void>(SourceDocumentErrors::SaveConflict);
        return Result<void>::Success();
    }

    /** @copydoc SourceDocumentService::Storage::PrepareBatchRoot */
    Result<std::shared_ptr<const Detail::SourceDocumentRoot>> SourceDocumentService::Storage::PrepareBatchRoot(
        const Record &record, const SourceTextEdit &edit, const CancellationToken cancellation, const std::size_t reservedBytes) const {
        using Output = Result<std::shared_ptr<const Detail::SourceDocumentRoot>>;
        auto text = PrepareEdit(record, edit, cancellation, reservedBytes);
        if (text.HasError())
            return Output::Failure(text.ErrorValue());
        if (text.Value()->bytes == record.root->text->bytes)
            return Output::Success(record.root);
        auto root = Detail::NextRoot(*record.root);
        if (root.HasError())
            return Output::Failure(root.ErrorValue());
        root.Value()->text = text.Value()->bytes == record.root->base->bytes ? record.root->base : std::move(text).Value();
        return Output::Success(std::move(root).Value());
    }

    /** @copydoc SourceDocumentService::Storage::StageBatchEdit */
    Result<SourceDocumentService::Storage::StagedBatchEdit> SourceDocumentService::Storage::StageBatchEdit(
        Record &record, const SourceTextEdit &edit, const CancellationToken cancellation, const std::size_t reservedBytes) const {
        using Output = Result<StagedBatchEdit>;
        auto next = PrepareBatchRoot(record, edit, cancellation, reservedBytes);
        if (next.HasError())
            return Output::Failure(next.ErrorValue());
        if (next.Value() == record.root)
            return Output::Success({&record, record.root, {}});
        auto history = PrepareHistory(record, edit, *next.Value()->text);
        if (history.HasError())
            return Output::Failure(history.ErrorValue());
        return Output::Success({&record, std::move(next).Value(), std::move(history).Value()});
    }

    /** @copydoc SourceDocumentService::Storage::StageEdits */
    Result<SourceDocumentService::Storage::StagedBatch> SourceDocumentService::Storage::StageEdits(
        const std::span<const SourceBatchEdit> edits, const CancellationToken cancellation, const std::size_t draftBytes) {
        if (edits.size() > limits.maximumDocuments)
            return Failure<StagedBatch>(SourceDocumentErrors::Invalid);
        StagedBatch batch;
        batch.bytes = draftBytes;
        batch.items.reserve(edits.size());
        for (const auto &edit : edits) {
            auto *record = FindCurrent(edit.source);
            if (!record)
                return Failure<StagedBatch>(SourceDocumentErrors::Stale);
            if (std::ranges::find(batch.items, record, &StagedBatchEdit::record) != batch.items.end())
                return Failure<StagedBatch>(SourceDocumentErrors::Invalid);
            auto next = StageBatchEdit(*record, edit.edit, cancellation, batch.bytes);
            if (next.HasError())
                return Result<StagedBatch>::Failure(next.ErrorValue());
            auto &item = batch.items.emplace_back(std::move(next).Value());
            if (item.history && item.next->text != record->root->base)
                batch.bytes += item.next->text->bytes.size();
        }
        return Result<StagedBatch>::Success(std::move(batch));
    }

    /** @copydoc SourceDocumentService::Storage::MakeBatchReceipt */
    SourceEditBatch SourceDocumentService::Storage::MakeBatchReceipt(const std::span<const StagedBatchEdit> edits) const {
        auto data = std::make_shared<SourceEditBatch::Data>();
        data->items.reserve(edits.size());
        bool retainsAll = limits.maximumHistoryEntries != 0;
        bool hasChange = false;
        for (const auto &item : edits) {
            data->items.push_back({SourceDocumentSnapshot{item.next}, item.history.has_value()});
            if (item.history) {
                hasChange = true;
                retainsAll &= item.history->Bytes() <= limits.maximumHistoryBytes;
            }
        }
        data->undoable = hasChange && retainsAll;
        SourceEditBatch receipt;
        receipt.data_ = std::move(data);
        return receipt;
    }

    /** @copydoc SourceDocumentService::Storage::VerifyBatchBases */
    Result<void> SourceDocumentService::Storage::VerifyBatchBases(const StagedBatch &batch, const CancellationToken cancellation) const {
        if (const auto reserved = AdmitBytes(batch.bytes + limits.maximumDocumentBytes); reserved.HasError())
            return reserved;
        for (const auto &item : batch.items)
            if (const auto base = VerifyBatchBase(*item.record, cancellation); base.HasError())
                return base;
        return Result<void>::Success();
    }

    /** @copydoc SourceDocumentService::EditBatch */
    Result<SourceEditBatch> SourceDocumentService::EditBatch(const std::span<const SourceBatchEdit> edits,
                                                             const CancellationToken cancellation) {
        return PublishBatch(edits, nullptr, nullptr, cancellation);
    }

    /** @copydoc SourceDocumentService::EditBatchWithEvidence */
    Result<SourceEditBatch> SourceDocumentService::EditBatchWithEvidence(const std::span<const SourceBatchEdit> edits,
                                                                         const SourceBatchDiskEvidence &evidence,
                                                                         const DurableFileSystem &files,
                                                                         const CancellationToken cancellation) {
        return PublishBatch(edits, &evidence, &files, cancellation);
    }

    /** @copydoc SourceDocumentService::PublishBatch */
    Result<SourceEditBatch> SourceDocumentService::PublishBatch(const std::span<const SourceBatchEdit> edits,
                                                                const SourceBatchDiskEvidence *evidence, const DurableFileSystem *files,
                                                                const CancellationToken cancellation) {
        using Output = Result<SourceEditBatch>;
        try {
            if (const auto access = CheckAccess(); access.HasError())
                return Output::Failure(access.ErrorValue());
            auto stage = storage_->StageEdits(edits, cancellation);
            if (stage.HasError())
                return Output::Failure(stage.ErrorValue());
            auto batch = std::move(stage).Value();
            static_assert(std::is_nothrow_move_constructible_v<Output>);
            auto result = Output::Success(storage_->MakeBatchReceipt(batch.items));
            const auto disk = evidence ? storage_->VerifyBatchEvidence(batch, *evidence, *files, cancellation)
                                       : storage_->VerifyBatchBases(batch, cancellation);
            if (disk.HasError())
                return Output::Failure(disk.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Failure<SourceEditBatch>(SourceDocumentErrors::Cancelled);
            storage_->PublishStagedEdits(batch.items);
            return result;
        } catch (const std::bad_alloc &) {
            return Failure<SourceEditBatch>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::Storage::PublishStagedEdits */
    void SourceDocumentService::Storage::PublishStagedEdits(const std::span<StagedBatchEdit> edits) const noexcept {
        for (auto &item : edits) {
            if (!item.history)
                continue;
            CommitHistory(*item.record, std::move(*item.history));
            item.record->Publish(std::move(item.next));
        }
    }

    /** @copydoc SourceDocumentService::Storage::StageDrafts */
    Result<std::vector<SourceDocumentService::Storage::Record>> SourceDocumentService::Storage::StageDrafts(
        const std::span<const DraftRequest> drafts, const CancellationToken cancellation) const {
        using Records = std::vector<Record>;
        using Output = Result<Records>;
        if (drafts.size() > limits.maximumDocuments - records.size())
            return Failure<Records>(SourceDocumentErrors::TooLarge);
        std::size_t bytes{};
        for (const auto &draft : drafts) {
            if (draft.text.size() > limits.maximumDocumentBytes || draft.text.size() > limits.maximumResidentBytes - bytes)
                return Failure<Records>(SourceDocumentErrors::TooLarge);
            bytes += draft.text.size();
        }
        if (const auto budget = AdmitBytes(bytes); budget.HasError())
            return Output::Failure(budget.ErrorValue());
        Records staged;
        staged.reserve(drafts.size());
        for (const auto &draft : drafts) {
            if (const auto admitted = AdmitNew(draft.identity); admitted.HasError())
                return Output::Failure(admitted.ErrorValue());
            auto record = PrepareDraft(draft.identity, draft.path, std::string{draft.text}, cancellation);
            if (record.HasError())
                return Output::Failure(record.ErrorValue());
            staged.push_back(std::move(record).Value());
        }
        return Output::Success(std::move(staged));
    }

    /** @copydoc SourceDocumentService::Storage::StageMoves */
    Result<std::vector<SourceDocumentService::Storage::StagedMove>> SourceDocumentService::Storage::StageMoves(
        const std::span<const RenameRequest> renames, const std::span<const DeleteRequest> deletions, const StagedBatch &batch,
        DurableFileSystem &files, const CancellationToken cancellation, const std::span<const SourceReviewTrashRestore> restores) {
        using Moves = std::vector<StagedMove>;
        using Output = Result<Moves>;
        Moves moves;
        moves.reserve(renames.size() + deletions.size() + restores.size());
        auto reservedBytes = batch.bytes;
        const auto append = [&](Result<StagedMove> staged) -> Result<void> {
            if (staged.HasError())
                return Result<void>::Failure(std::move(staged).ErrorValue());
            const auto *record = staged.Value().edit.record;
            if (record && (std::ranges::find(batch.items, record, &StagedBatchEdit::record) != batch.items.end() ||
                           std::ranges::any_of(moves, [&](const auto &move) {
                return move.edit.record == record;
            })))
                return Failure<void>(SourceDocumentErrors::Invalid);
            auto &move = moves.emplace_back(std::move(staged).Value());
            const auto &edit = move.edit;
            if (edit.history && edit.next->text != edit.record->root->base)
                reservedBytes += edit.next->text->bytes.size();
            return Result<void>::Success();
        };
        for (const auto &rename : renames) {
            if (const auto result = append(StageRename(rename, cancellation, reservedBytes)); result.HasError())
                return Output::Failure(result.ErrorValue());
        }
        for (const auto &deletion : deletions) {
            if (const auto result = append(StageDelete(deletion)); result.HasError())
                return Output::Failure(result.ErrorValue());
        }
        for (const auto &restore : restores) {
            if (const auto result = append(StageTrashRestore(restore)); result.HasError())
                return Output::Failure(result.ErrorValue());
        }
        for (auto &move : moves) {
            if (const auto lease = LockMove(move, files, cancellation); lease.HasError())
                return Output::Failure(lease.ErrorValue());
        }
        return Output::Success(std::move(moves));
    }

    /** @copydoc SourceDocumentService::PublishReviewGroup */
    Result<SourceFileReviewBatch> SourceDocumentService::PublishReviewGroup(const std::span<const SourceBatchEdit> edits,
                                                                            const SourceBatchDiskEvidence &evidence,
                                                                            DurableFileSystem &files, const CancellationToken cancellation,
                                                                            const std::span<const DraftRequest> drafts,
                                                                            const std::span<const RenameRequest> renames,
                                                                            const std::span<const DeleteRequest> deletions) {
        using Output = Result<SourceFileReviewBatch>;
        try {
            if (const auto access = CheckAccess(); access.HasError())
                return Output::Failure(access.ErrorValue());
            if (drafts.empty() && renames.empty() && deletions.empty())
                return Failure<SourceFileReviewBatch>(SourceDocumentErrors::Invalid);
            Detail::SourcePublicationGuard guard{storage_->saving};
            auto prepared = storage_->StageDrafts(drafts, cancellation);
            if (prepared.HasError())
                return Output::Failure(prepared.ErrorValue());
            auto newRecords = std::move(prepared).Value();
            std::vector<SourceDocumentSnapshot> opened;
            opened.reserve(newRecords.size());
            std::size_t draftBytes{};
            for (const auto &record : newRecords) {
                draftBytes += record.root->text->bytes.size();
                opened.push_back(SourceDocumentSnapshot{record.root});
            }
            storage_->records.reserve(storage_->records.size() + newRecords.size());
            auto stage = storage_->StageEdits(edits, cancellation, draftBytes);
            if (stage.HasError())
                return Output::Failure(stage.ErrorValue());
            auto batch = std::move(stage).Value();
            auto stagedMoves = storage_->StageMoves(renames, deletions, batch, files, cancellation);
            if (stagedMoves.HasError())
                return Output::Failure(stagedMoves.ErrorValue());
            auto moves = std::move(stagedMoves).Value();
            std::vector<SourceDocumentSnapshot> movedSources;
            std::vector<SourceDocumentSnapshot> originalSources;
            movedSources.reserve(renames.size());
            originalSources.reserve(renames.size());
            for (const auto &move : moves) {
                if (move.edit.next) {
                    movedSources.push_back(SourceDocumentSnapshot{move.edit.next});
                    originalSources.push_back(move.source);
                }
            }
            static_assert(std::is_nothrow_move_constructible_v<Output>);
            auto result = Output::Success({storage_->MakeBatchReceipt(batch.items), std::move(opened), std::move(movedSources),
                                           std::move(originalSources), deletions.size()});
            if (!edits.empty()) {
                const auto disk = storage_->VerifyBatchEvidence(batch, evidence, files, cancellation);
                if (disk.HasError())
                    return Output::Failure(disk.ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return Failure<SourceFileReviewBatch>(SourceDocumentErrors::Cancelled);
            if (!moves.empty()) {
                if (const auto moved = storage_->MoveSources(moves, files, cancellation, &evidence, &batch); moved.HasError())
                    return Output::Failure(moved.ErrorValue());
            }
            storage_->PublishStagedEdits(batch.items);
            storage_->PublishMoves(moves);
            static_assert(std::is_nothrow_move_constructible_v<Storage::Record>);
            for (auto &record : newRecords)
                storage_->records.push_back(std::move(record));
            return result;
        } catch (const std::bad_alloc &) {
            return Failure<SourceFileReviewBatch>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::Storage::StageBatchUndo */
    Result<SourceDocumentService::Storage::StagedBatchUndo> SourceDocumentService::Storage::StageBatchUndo(
        const SourceBatchItem &item, const CancellationToken cancellation, const std::size_t reservedBytes) {
        auto *record = FindCurrent(item.snapshot);
        if (!record)
            return Failure<StagedBatchUndo>(SourceDocumentErrors::Stale);
        const auto step = record->Step(false);
        if (!step)
            return Failure<StagedBatchUndo>(SourceDocumentErrors::Invalid);
        auto next = PrepareBatchRoot(*record, step->patch, cancellation, reservedBytes);
        if (next.HasError())
            return Result<StagedBatchUndo>::Failure(next.ErrorValue());
        return Result<StagedBatchUndo>::Success({record, std::move(next).Value(), step->position});
    }

    /** @copydoc SourceDocumentService::UndoBatch */
    Result<std::vector<SourceDocumentSnapshot>> SourceDocumentService::UndoBatch(const SourceEditBatch &batch,
                                                                                 const CancellationToken cancellation) {
        return UndoWithDrafts(batch, {}, cancellation);
    }

    /** @copydoc SourceDocumentService::UndoWithDrafts */
    Result<std::vector<SourceDocumentSnapshot>> SourceDocumentService::UndoWithDrafts(
        const SourceEditBatch &batch, const std::span<const SourceDocumentSnapshot> drafts, const CancellationToken cancellation,
        const std::span<const RenameRequest> reverseRenames, DurableFileSystem *files,
        const std::span<const SourceReviewTrashRestore> restores) {
        using Snapshots = std::vector<SourceDocumentSnapshot>;
        using Output = Result<Snapshots>;

        try {
            if (const auto access = CheckAccess(); access.HasError())
                return Output::Failure(access.ErrorValue());
            const bool hasEdits = std::ranges::any_of(batch.Items(), &SourceBatchItem::changed);
            if (!batch.CanUndo() && ((drafts.empty() && reverseRenames.empty() && restores.empty()) || hasEdits))
                return Failure<Snapshots>(SourceDocumentErrors::Invalid);
            for (const auto &draft : drafts) {
                const auto *record = storage_->FindCurrent(draft);
                if (!record || !record->root->draft)
                    return Failure<Snapshots>(SourceDocumentErrors::Stale);
            }
            std::vector<Storage::StagedBatchUndo> staged;
            Snapshots snapshots;
            staged.reserve(batch.Items().size());
            snapshots.reserve(batch.Items().size());
            std::size_t reservedBytes{};
            for (const auto &item : batch.Items()) {
                if (!item.changed)
                    continue;
                auto next = storage_->StageBatchUndo(item, cancellation, reservedBytes);
                if (next.HasError())
                    return Output::Failure(next.ErrorValue());
                const auto &prepared = staged.emplace_back(std::move(next).Value());
                if (prepared.next->text != prepared.record->root->base)
                    reservedBytes += prepared.next->text->bytes.size();
                snapshots.push_back(SourceDocumentSnapshot{prepared.next});
            }
            const auto finish = [&](Output &final, const std::span<Storage::StagedMove> moves) {
                final = Output::Success(std::move(snapshots));
                if (cancellation.IsCancellationRequested()) {
                    final = Failure<Snapshots>(SourceDocumentErrors::Cancelled);
                    return;
                }
                if (!moves.empty()) {
                    auto restored = storage_->MoveSources(moves, *files, cancellation);
                    if (restored.HasError()) {
                        final = Output::Failure(std::move(restored).ErrorValue());
                        return;
                    }
                }
                for (auto &item : staged) {
                    item.record->historyPosition = item.position;
                    item.record->Publish(std::move(item.next));
                }
                storage_->PublishMoves(moves);
                static_assert(std::is_nothrow_move_assignable_v<Storage::Record>);
                std::erase_if(storage_->records, [&](const auto &record) noexcept {
                    const auto snapshot = SourceDocumentSnapshot{record.root};
                    if (!std::ranges::any_of(drafts, [&](const auto &draft) {
                        return draft.SharesStateWith(snapshot);
                    }))
                        return false;
                    record.Revoke();
                    return true;
                });
            };
            if (reverseRenames.empty() && restores.empty())
                return Output::CompleteInPlace(MakeError(SourceDocumentErrors::TooLarge), [&](Output &final) {
                    finish(final, {});
                });
            if (!files)
                return Failure<Snapshots>(SourceDocumentErrors::Invalid);
            Storage::StagedBatch related{.bytes = reservedBytes};
            related.items.reserve(staged.size());
            for (const auto &item : staged)
                related.items.push_back({item.record, item.next, {}});
            Detail::SourcePublicationGuard guard{storage_->saving};
            auto moves = storage_->StageMoves(reverseRenames, {}, related, *files, cancellation, restores);
            if (moves.HasError())
                return Output::Failure(std::move(moves).ErrorValue());
            auto ready = std::move(moves).Value();
            return Output::CompleteInPlace(MakeError(SourceDocumentErrors::TooLarge), [&](Output &final) {
                finish(final, ready);
            });
        } catch (const std::bad_alloc &) {
            return Failure<Snapshots>(SourceDocumentErrors::TooLarge);
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
                record.Publish(std::move(next).Value());
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
            if (expectedRevision != record.root->revision || record.root->draft)
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Stale);
            auto loaded = storage_->ReadRecord(record, cancellation);
            if (loaded.HasError())
                return Result<SourceDocumentSnapshot>::Failure(loaded.ErrorValue());
            if (cancellation.IsCancellationRequested())
                return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::Cancelled);
            if (MatchesCurrentDisk(*record.root, *loaded.Value())) {
                record.history.clear();
                record.historyPosition = 0;
                record.historyBytes = 0;
                record.historyBoundary = SourceHistoryBoundary::None;
                return current;
            }
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
            record.history.clear();
            record.historyPosition = 0;
            record.historyBytes = 0;
            record.historyBoundary = SourceHistoryBoundary::None;
            record.Publish(std::move(next).Value());
            return Result<SourceDocumentSnapshot>::Success(SourceDocumentSnapshot{record.root});
        } catch (const std::bad_alloc &) {
            return Failure<SourceDocumentSnapshot>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::History */
    Result<SourceHistoryState> SourceDocumentService::History(const DocumentInstanceId instance) const {
        if (const auto current = Snapshot(instance); current.HasError())
            return Result<SourceHistoryState>::Failure(current.ErrorValue());
        const auto &record = *storage_->Find(instance);
        return Result<SourceHistoryState>::Success(
            {record.historyPosition, record.history.size() - record.historyPosition, record.historyBytes, record.historyBoundary});
    }

    /** @copydoc SourceDocumentService::ReplayHistory */
    Result<SourceHistoryResult> SourceDocumentService::ReplayHistory(const DocumentInstanceId instance,
                                                                     const std::uint64_t expectedRevision, const bool redo,
                                                                     const CancellationToken cancellation) {
        try {
            if (const auto current = Snapshot(instance); current.HasError())
                return Result<SourceHistoryResult>::Failure(current.ErrorValue());
            auto &record = *storage_->Find(instance);
            if (expectedRevision != record.root->revision)
                return Failure<SourceHistoryResult>(SourceDocumentErrors::Stale);
            if (cancellation.IsCancellationRequested())
                return Failure<SourceHistoryResult>(SourceDocumentErrors::Cancelled);
            const auto step = record.Step(redo);
            if (!step)
                return Failure<SourceHistoryResult>(SourceDocumentErrors::Invalid);
            auto text = storage_->PrepareEdit(record, step->patch, cancellation);
            if (text.HasError())
                return Result<SourceHistoryResult>::Failure(text.ErrorValue());
            auto next = Detail::NextRoot(*record.root);
            if (next.HasError())
                return Result<SourceHistoryResult>::Failure(next.ErrorValue());
            next.Value()->text = text.Value()->bytes == record.root->base->bytes ? record.root->base : std::move(text).Value();
            if (cancellation.IsCancellationRequested())
                return Failure<SourceHistoryResult>(SourceDocumentErrors::Cancelled);
            record.historyPosition = step->position;
            record.Publish(std::move(next).Value());
            return Result<SourceHistoryResult>::Success({SourceDocumentSnapshot{record.root}, step->view});
        } catch (const std::bad_alloc &) {
            return Failure<SourceHistoryResult>(SourceDocumentErrors::TooLarge);
        }
    }

    /** @copydoc SourceDocumentService::Undo */
    Result<SourceHistoryResult> SourceDocumentService::Undo(const DocumentInstanceId instance, const std::uint64_t expectedRevision,
                                                            const CancellationToken cancellation) {
        return ReplayHistory(instance, expectedRevision, false, cancellation);
    }

    /** @copydoc SourceDocumentService::Redo */
    Result<SourceHistoryResult> SourceDocumentService::Redo(const DocumentInstanceId instance, const std::uint64_t expectedRevision,
                                                            const CancellationToken cancellation) {
        return ReplayHistory(instance, expectedRevision, true, cancellation);
    }
}  // namespace Horo::Editor
