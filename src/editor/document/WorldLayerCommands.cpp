#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <numeric>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        constexpr std::size_t MaximumHistoryEntries = 128;
        constexpr std::size_t MaximumHistoryBytes = 4 * 1024 * 1024;
        constexpr std::size_t MaximumFilterBytes = 1024;

        /** @brief Retains the document boundary's typed failure identity. */
        template <typename T> Result<T> Reject(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Charges the complete owned layer/filter section, including inline vector element storage. */
        std::size_t LayerBytes(const std::vector<WS::WorldLayerDescriptor> &layers, const WorldLayerOutlinerFilter &filter) {
            return std::accumulate(layers.begin(), layers.end(), filter.name.size() + layers.size() * sizeof(WS::WorldLayerDescriptor),
                                   [](const std::size_t sum, const auto &layer) {
                return sum + layer.name.size();
            });
        }
    }  // namespace

    /** @copydoc PartitionedWorldWorkspace::ApplyLayerCommand */
    Result<void> PartitionedWorldWorkspace::ApplyLayerCommand(const WorldLayerCommand &command, PartitionedWorldSnapshot &candidate) const {
        if (command.valueless_by_exception())
            return Reject<void>(WorldDocumentErrors::Unsupported);
        return std::visit([&](const auto &intent) -> Result<void> {
            using Intent = std::decay_t<decltype(intent)>;
            auto &layers = candidate.document.layers;
            if constexpr (std::is_same_v<Intent, CreateWorldLayer>) {
                if (layers.size() >= limits_.maximumLayers || intent.layer.name.size() > limits_.maximumLayerNameBytes)
                    return Reject<void>(WorldDocumentErrors::Capacity);
                if (intent.position > layers.size())
                    return Reject<void>(WorldDocumentErrors::Invalid);
                layers.insert(layers.begin() + static_cast<std::ptrdiff_t>(intent.position), intent.layer);
            } else if constexpr (std::is_same_v<Intent, ReorderWorldLayer>) {
                const auto found = std::ranges::find(layers, intent.layer, &WS::WorldLayerDescriptor::id);
                if (!intent.layer.IsValid() || found == layers.end() || intent.position >= layers.size())
                    return Reject<void>(WorldDocumentErrors::Invalid);
                const auto old = static_cast<std::size_t>(found - layers.begin());
                if (old < intent.position)
                    std::rotate(found, found + 1, layers.begin() + static_cast<std::ptrdiff_t>(intent.position + 1));
                else if (old > intent.position)
                    std::rotate(layers.begin() + static_cast<std::ptrdiff_t>(intent.position), found, found + 1);
            } else if constexpr (std::is_same_v<Intent, FilterWorldLayers>) {
                if (intent.filter.name.size() > MaximumFilterBytes)
                    return Reject<void>(WorldDocumentErrors::Capacity);
                if (intent.filter.ownership && *intent.filter.ownership > WS::WorldLayerOwnership::NetworkReplication)
                    return Reject<void>(WorldDocumentErrors::Unsupported);
                if (intent.filter.name.find('\0') != std::string::npos || !IsValidUtf8ScalarSequence(intent.filter.name))
                    return Reject<void>(WorldDocumentErrors::Invalid);
                candidate.layerFilter = intent.filter;
            } else {
                if (intent.layer && (!intent.layer->IsValid() || std::ranges::none_of(layers, [&](const auto &layer) {
                    return layer.id == *intent.layer;
                })))
                    return Reject<void>(WorldDocumentErrors::Invalid);
                candidate.currentLayer = intent.layer;
            }
            return Result<void>::Success();
        }, command);
    }

    /** @copydoc PartitionedWorldWorkspace::ExecuteLayerCommand */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::ExecuteLayerCommand(const WorldLayerCommand &command,
                                                                                         const WorldDocumentFence expected,
                                                                                         const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Reject<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        if (const auto applied = ApplyLayerCommand(command, *candidate); applied.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(applied.ErrorValue());
        // Complete document admission is shared with Commit; layer edits cannot bypass membership or policy validation.
        if (const auto valid = ValidateLayerCandidate(candidate->document); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        const bool authored = candidate->document.layers != snapshot_->document.layers;
        if (!authored && candidate->layerFilter == snapshot_->layerFilter && candidate->currentLayer == snapshot_->currentLayer)
            return Result<PartitionedWorldSnapshotLease>::Success(snapshot_);
        LayerTransaction entry{.before = {snapshot_->document.layers, snapshot_->layerFilter, snapshot_->currentLayer},
                               .after = {candidate->document.layers, candidate->layerFilter, candidate->currentLayer},
                               .bytes = sizeof(LayerTransaction) + LayerBytes(snapshot_->document.layers, snapshot_->layerFilter) +
                                        LayerBytes(candidate->document.layers, candidate->layerFilter),
                               .beforeDirty = snapshot_->dirty,
                               .afterDirty = snapshot_->dirty || authored};
        if (entry.bytes > MaximumHistoryBytes)
            return Reject<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Capacity);
        std::vector<LayerTransaction> history(layerHistory_.begin(), layerHistory_.begin() + static_cast<std::ptrdiff_t>(layerCursor_));
        auto bytes = std::accumulate(history.begin(), history.end(), std::size_t{}, [](const std::size_t sum, const auto &item) {
            return sum + item.bytes;
        });
        while (!history.empty() && (history.size() >= MaximumHistoryEntries || bytes > MaximumHistoryBytes - entry.bytes)) {
            bytes -= history.front().bytes;
            history.erase(history.begin());
        }
        history.push_back(std::move(entry));
        candidate->dirty = candidate->dirty || authored;
        candidate->layerUndoCount = history.size();
        candidate->layerRedoCount = 0;
        auto published = Publish(std::move(candidate), cancellation);
        if (published.HasValue()) {
            layerHistory_.swap(history);
            layerCursor_ = layerHistory_.size();
        }
        return published;
    }

    /** @copydoc PartitionedWorldWorkspace::ReplayLayerCommand */
    Result<PartitionedWorldSnapshotLease> PartitionedWorldWorkspace::ReplayLayerCommand(const bool redo, const WorldDocumentFence expected,
                                                                                        const std::stop_token cancellation) {
        if (const auto valid = CheckFence(expected); valid.HasError())
            return Result<PartitionedWorldSnapshotLease>::Failure(valid.ErrorValue());
        if (cancellation.stop_requested())
            return Reject<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Cancelled);
        if (redo ? layerCursor_ == layerHistory_.size() : layerCursor_ == 0)
            return Reject<PartitionedWorldSnapshotLease>(WorldDocumentErrors::Conflict);
        const auto &entry = layerHistory_[redo ? layerCursor_ : layerCursor_ - 1];
        const auto &state = redo ? entry.after : entry.before;
        auto candidate = std::make_shared<PartitionedWorldSnapshot>(*snapshot_);
        candidate->document.layers = state.layers;
        candidate->layerFilter = state.filter;
        candidate->currentLayer = state.current;
        candidate->dirty = redo ? entry.afterDirty : entry.beforeDirty;
        const auto cursor = redo ? layerCursor_ + 1 : layerCursor_ - 1;
        candidate->layerUndoCount = cursor;
        candidate->layerRedoCount = layerHistory_.size() - cursor;
        auto published = Publish(std::move(candidate), cancellation);
        if (published.HasValue())
            layerCursor_ = cursor;
        return published;
    }

    /** @copydoc PartitionedWorldWorkspace::VisibleLayers */
    Result<std::vector<WS::StreamingLayerId>> PartitionedWorldWorkspace::VisibleLayers() const {
        const auto current = Snapshot();
        if (current.HasError())
            return Result<std::vector<WS::StreamingLayerId>>::Failure(current.ErrorValue());
        std::vector<WS::StreamingLayerId> rows;
        const auto &filter = current.Value()->layerFilter;
        for (const auto &layer : current.Value()->document.layers) {
            if ((!filter.ownership || layer.ownership == *filter.ownership) && layer.name.find(filter.name) != std::string::npos)
                rows.push_back(layer.id);
        }
        return Result<std::vector<WS::StreamingLayerId>>::Success(std::move(rows));
    }

    /** @copydoc PartitionedWorldWorkspace::ClearLayerHistory */
    void PartitionedWorldWorkspace::ClearLayerHistory() noexcept {
        layerHistory_.clear();
        layerCursor_ = 0;
    }
}  // namespace Horo::Editor
