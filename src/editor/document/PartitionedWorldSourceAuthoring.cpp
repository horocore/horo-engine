#include "Horo/Editor/PartitionedWorldWorkspace.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Preserves the declared failure at the owning document boundary. */
        Result<PartitionedWorldSnapshotLease> Reject(const ErrorCodeDescriptor &error) {
            return Result<PartitionedWorldSnapshotLease>::Failure(MakeError(error));
        }
    }  // namespace

    /** @copydoc PartitionedWorldWorkspace::ClearSourceHistory */
    void PartitionedWorldWorkspace::ClearSourceHistory() noexcept {
        sourceHistory_.clear();
        sourceHistoryCursor_ = 0;
    }

    /** @copydoc PartitionedWorldWorkspace::EditSource */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::EditSource(const WorldSourceEdit &edit,
                                                                                const std::stop_token cancellation) {
        if (const auto valid = CheckFence(edit.expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Reject(WorldDocumentErrors::Cancelled);
        if (!edit.source.IsValid() || (edit.value && edit.value->id != edit.source))
            return Reject(WorldDocumentErrors::Invalid);
        auto document = snapshot_->document;
        auto found = std::ranges::find(document.sources, edit.source, &PartitionedWorldSource::id);
        if (!edit.value) {
            if (found == document.sources.end())
                return Reject(WorldDocumentErrors::Conflict);
            document.sources.erase(found);
        } else {
            auto value = *edit.value;
            if (found != document.sources.end()) {
                value.revision = found->revision;
                if (value == *found)
                    return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
            }
            if (sourceRevision_ == std::numeric_limits<std::uint64_t>::max())
                return Reject(WorldDocumentErrors::Exhausted);
            value.revision = WorldStreaming::StreamingSourceRevision::Create(sourceRevision_ + 1).Value();
            if (found == document.sources.end())
                document.sources.push_back(value);
            else
                *found = value;
        }
        if (sourceHistoryCursor_ >= limits_.maximumSourceHistory)
            return Reject(WorldDocumentErrors::Capacity);
        // Prepare all fallible history storage before the authority publishes anything.
        auto history = sourceHistory_;
        history.resize(sourceHistoryCursor_);
        history.push_back({snapshot_->document.sources, document.sources});
        auto result = Commit(document, edit.expected, cancellation);
        if (result.HasValue()) {
            sourceHistory_.swap(history);
            sourceHistoryCursor_ = sourceHistory_.size();
        }
        return result;
    }

    /** @copydoc PartitionedWorldWorkspace::ReplaySources */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::ReplaySources(const WorldDocumentFence expected, const bool redo,
                                                                                   const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Reject(WorldDocumentErrors::Cancelled);
        if ((redo && sourceHistoryCursor_ == sourceHistory_.size()) || (!redo && sourceHistoryCursor_ == 0))
            return Reject(WorldDocumentErrors::Conflict);
        if (sourceRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Reject(WorldDocumentErrors::Exhausted);
        const auto cursor = redo ? sourceHistoryCursor_ + 1 : sourceHistoryCursor_ - 1;
        auto document = snapshot_->document;
        document.sources = redo ? sourceHistory_[sourceHistoryCursor_].after : sourceHistory_[cursor].before;
        const auto revision = WorldStreaming::StreamingSourceRevision::Create(sourceRevision_ + 1).Value();
        for (auto &source : document.sources)
            source.revision = revision;
        auto history = sourceHistory_;
        const auto nextRevision = sourceRevision_ + 1;
        auto result = Commit(document, expected, cancellation);
        if (result.HasValue()) {
            sourceRevision_ = nextRevision;
            sourceHistory_.swap(history);
            sourceHistoryCursor_ = cursor;
        }
        return result;
    }
}  // namespace Horo::Editor
