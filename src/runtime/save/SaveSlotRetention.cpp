#include "SaveSlotLifecycleInternal.h"

#include <algorithm>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    namespace {
        /** @brief Selects only the two independently bounded automatic categories. */
        [[nodiscard]] std::size_t KindIndex(const SaveSlotKind kind) noexcept {
            return kind == SaveSlotKind::Auto ? 0 : 1;
        }

        /** @brief Rejects limits that could overwrite the only durable automatic publication. */
        [[nodiscard]] bool ValidLimits(const SaveSlotRetentionLimits &limits) noexcept {
            return limits.maximumSlots >= 2 && limits.maximumSlots <= 64 && limits.lowSpaceSlots >= 2 &&
                   limits.lowSpaceSlots <= limits.maximumSlots && limits.minimumSlots >= 1 && limits.minimumSlots <= limits.lowSpaceSlots &&
                   limits.backupGenerations >= 1 && limits.backupGenerations <= 64;
        }

        /** @brief Counts only visible publications in the exact automatic category. */
        [[nodiscard]] bool Included(const Record &record, const SaveSlotKind kind) noexcept {
            return !record.deleted && record.entry.publication.kind == kind;
        }

        /** @brief Legacy unknown order, pins and the newest durable publication are unconditional protections. */
        [[nodiscard]] bool Eligible(const Record &record, const SaveSlotKind kind, const std::uint64_t newest) noexcept {
            return Included(record, kind) && !record.pinned && record.sequence != 0 && record.sequence != newest;
        }

        /** @brief Selects a diagnostic without reinterpreting low-space pressure as publication failure. */
        [[nodiscard]] SaveSlotRetentionReason Reason(const bool capacityExceeded, const bool lowSpace) noexcept {
            if (!capacityExceeded)
                return SaveSlotRetentionReason::Age;
            return lowSpace ? SaveSlotRetentionReason::LowSpace : SaveSlotRetentionReason::Capacity;
        }

        /** @brief Tests age without overflow or trusting caller-authored archive timestamps. */
        [[nodiscard]] bool Expired(const Record &record, const Catalog &catalog, const SaveSlotRetentionLimits &limits) noexcept {
            return limits.maximumAgeMilliseconds != 0 && catalog.clock >= record.committedAt &&
                   catalog.clock - record.committedAt >= limits.maximumAgeMilliseconds;
        }
    }  // namespace

    /** @copydoc ValidateRetentionPolicy */
    Result<void> ValidateRetentionPolicy(const SaveSlotRetentionPolicy &policy) {
        for (const auto &limits : policy.kinds) {
            if (!ValidLimits(limits))
                return Result<void>::Failure(MakeError(SaveErrors::StoragePolicyInvalid));
        }
        return Result<void>::Success();
    }

    /** @copydoc RetireForRetention */
    void RetireForRetention(Catalog &catalog, const Record &record, const SaveSlotRetentionPolicy &policy) {
        catalog.retired.push_back({record.entry, false, record.sequence, record.committedAt, true, policy.cloudTombstones});
    }

    /** @copydoc BoundRetentionBackups */
    void BoundRetentionBackups(Catalog &catalog, const SaveSlotKind kind, const SaveSlotRetentionPolicy &policy) {
        std::vector<Retired *> ordered;
        for (auto &record : catalog.retired) {
            if (record.backup && record.entry.publication.kind == kind)
                ordered.push_back(&record);
        }
        std::ranges::sort(ordered, [](const Retired *left, const Retired *right) {
            return left->sequence > right->sequence;
        });
        const auto count = policy.kinds[KindIndex(kind)].backupGenerations;
        for (std::size_t index = count; index < ordered.size(); ++index)
            ordered[index]->backup = false;
    }

    /** @copydoc ApplyRetention */
    Result<std::vector<SaveSlotRetentionDecision>> ApplyRetention(Catalog &catalog, const SaveSlotKind kind,
                                                                  const SaveSlotRetentionPolicy &policy, const bool lowSpace) {
        const auto &limits = policy.kinds[KindIndex(kind)];
        const std::size_t capacity = lowSpace ? limits.lowSpaceSlots : limits.maximumSlots;
        std::size_t count = 0;
        std::uint64_t newest = 0;
        for (const auto &record : catalog.records) {
            if (Included(record, kind)) {
                ++count;
                newest = std::max(newest, record.sequence);
            }
        }
        std::vector<Record> eligible;
        for (const auto &record : catalog.records) {
            if (Eligible(record, kind, newest))
                eligible.push_back(record);
        }
        std::ranges::sort(eligible, {}, &Record::sequence);
        std::vector<SaveSlotRetentionDecision> decisions;
        for (const auto &record : eligible) {
            const bool capacityExceeded = count > capacity;
            const bool ageExceeded = count > limits.minimumSlots && Expired(record, catalog, limits);
            if (!capacityExceeded && !ageExceeded)
                continue;
            const auto reason = Reason(capacityExceeded, lowSpace);
            decisions.push_back({record.entry, reason, record.sequence});
            RetireForRetention(catalog, record, policy);
            std::erase_if(catalog.records, [&record](const Record &selected) {
                return selected.entry.publication.generation == record.entry.publication.generation;
            });
            --count;
        }
        if (count > capacity)
            return Result<std::vector<SaveSlotRetentionDecision>>::Failure(MakeError(SaveErrors::StorageQuotaExceeded));
        BoundRetentionBackups(catalog, kind, policy);
        return Result<std::vector<SaveSlotRetentionDecision>>::Success(std::move(decisions));
    }
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail
