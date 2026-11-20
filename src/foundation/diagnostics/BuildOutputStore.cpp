#include "Horo/Foundation/BuildOutputStore.h"

#include <algorithm>
#include <utility>

namespace Horo {
    namespace {
        constexpr std::size_t MaximumOperationObservers = 8;
        constexpr std::size_t MaximumTailRecords = 16;
        constexpr std::size_t MaximumTailBytes = 32U << 10U;
    }  // namespace

    /** @copydoc BuildOutputOperationWitness::IsCurrent */
    bool BuildOutputOperationWitness::IsCurrent() const noexcept {
        return current_ && current_->load();
    }

    /** @copydoc IBuildOutputQuery::CaptureOperationTail */
    std::optional<BuildOutputOperationTail> IBuildOutputQuery::CaptureOperationTail(OperationId, std::size_t, std::size_t) const {
        return std::nullopt;
    }

    /** @copydoc BuildOutputStore::BuildOutputStore */
    BuildOutputStore::BuildOutputStore(const std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1U)) {}

    BuildOutputStore::~BuildOutputStore() {
        for (const auto &observer : operationObservers_)
            if (const auto witness = observer.current.lock())
                witness->store(false);
    }

    /** @copydoc BuildOutputStore::RevokeOperation */
    void BuildOutputStore::RevokeOperation(const std::optional<OperationId> operation) noexcept {
        std::erase_if(operationObservers_, [operation](const OperationObserver &observer) {
            const auto witness = observer.current.lock();
            if (!witness)
                return true;
            if (observer.operation != operation)
                return false;
            witness->store(false);
            return true;
        });
    }

    /** @copydoc BuildOutputStore::BeginSession */
    std::optional<BuildOutputSessionId> BuildOutputStore::BeginSession() {
        std::lock_guard lock(mutex_);
        if (nextSessionValue_ == 0U)
            return std::nullopt;
        const BuildOutputSessionId id{nextSessionValue_};
        ++nextSessionValue_;
        return id;
    }

    /** @copydoc BuildOutputStore::Append */
    void BuildOutputStore::Append(BuildOutputRecord record) {
        std::lock_guard lock(mutex_);
        record.sequence = nextSequence_++;
        if (records_.size() == capacity_) {
            RevokeOperation(records_.front().operationId);
            records_.pop_front();
            ++droppedRecordCount_;
        }
        records_.push_back(std::move(record));
        RevokeOperation(records_.back().operationId);
        ++revision_;
    }

    /** @copydoc BuildOutputStore::CaptureOperationTail */
    std::optional<BuildOutputOperationTail> BuildOutputStore::CaptureOperationTail(const OperationId operation,
                                                                                   const std::size_t maximumRecords,
                                                                                   const std::size_t maximumBytes) const {
        if (operation == 0 || maximumRecords == 0 || maximumRecords > MaximumTailRecords || maximumBytes == 0 ||
            maximumBytes > MaximumTailBytes)
            return std::nullopt;
        std::lock_guard lock(mutex_);
        BuildOutputOperationTail tail{.operation = operation, .truncated = droppedRecordCount_ != 0};
        std::size_t remaining = maximumBytes;
        for (auto record = records_.rbegin(); record != records_.rend(); ++record) {
            if (record->operationId != operation || record->message.empty())
                continue;
            if (!tail.lastSequence)
                tail.lastSequence = record->sequence;
            if (tail.messages.size() == maximumRecords || record->message.size() > remaining) {
                tail.truncated = true;
                break;
            }
            remaining -= record->message.size();
            tail.messages.push_back(record->message);
        }
        if (tail.messages.empty())
            return std::nullopt;
        std::ranges::reverse(tail.messages);
        std::erase_if(operationObservers_, [](const OperationObserver &observer) {
            return observer.current.expired();
        });
        const auto existing = std::ranges::find(operationObservers_, operation, &OperationObserver::operation);
        auto current = existing != operationObservers_.end() ? existing->current.lock() : nullptr;
        if (!current) {
            if (operationObservers_.size() == MaximumOperationObservers)
                return std::nullopt;
            current = std::make_shared<std::atomic<bool>>(true);
            operationObservers_.push_back({operation, current});
        }
        tail.witness = BuildOutputOperationWitness{std::move(current)};
        return tail;
    }

    /** @copydoc BuildOutputStore::SnapshotIfChanged */
    std::optional<BuildOutputSnapshot> BuildOutputStore::SnapshotIfChanged(const std::uint64_t knownRevision) const {
        std::lock_guard lock(mutex_);
        if (knownRevision == revision_)
            return std::nullopt;
        return BuildOutputSnapshot{.revision = revision_,
                                   .droppedRecordCount = droppedRecordCount_,
                                   .capacity = capacity_,
                                   .records = {records_.begin(), records_.end()}};
    }
}  // namespace Horo
