#include "Horo/Runtime/Save/SaveSlotIndex.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] bool ValidLimits(const SaveSlotIndexLimits &limits) noexcept {
            return limits.maximumEntries != 0 && limits.maximumArtifacts != 0 && limits.maximumDiagnostics != 0 &&
                   limits.maximumEntries <= limits.maximumArtifacts;
        }

        [[nodiscard]] const SaveGameSlotId &Slot(const SaveSlotCatalogEntry &entry) noexcept {
            return entry.publication.slot;
        }

        [[nodiscard]] bool EntryLess(const SaveSlotCatalogEntry &left, const SaveSlotCatalogEntry &right) noexcept {
            if (Slot(left) != Slot(right))
                return Slot(left) < Slot(right);
            return left.publication.generation < right.publication.generation;
        }

        [[nodiscard]] bool DiagnosticLess(const SaveSlotIndexDiagnostic &left, const SaveSlotIndexDiagnostic &right) noexcept {
            if (left.slot != right.slot)
                return left.slot < right.slot;
            if (left.kind != right.kind)
                return left.kind < right.kind;
            return left.generation < right.generation;
        }

        [[nodiscard]] Result<void> ValidateEntry(const SaveSlotCatalogEntry &entry) {
            if (auto publication = ValidateSaveSlotPublicationMetadata(entry.publication); publication.HasError())
                return publication;
            return ValidateSaveSlotDisplayMetadata(entry.display);
        }

        [[nodiscard]] Result<void> AddDiagnostic(std::vector<SaveSlotIndexDiagnostic> &diagnostics, const SaveSlotIndexLimits &limits,
                                                 SaveSlotIndexDiagnostic diagnostic) {
            if (diagnostics.size() == limits.maximumDiagnostics)
                return Result<void>::Failure(MakeError(SaveErrors::SlotIndexLimitExceeded));
            try {
                diagnostics.push_back(std::move(diagnostic));
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(SaveErrors::SlotIndexAllocationFailed));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ConsumeObservation(const SaveSlotArtifactObservation &observation,
                                                      std::vector<SaveSlotCatalogEntry> &committed,
                                                      std::vector<SaveSlotIndexDiagnostic> &diagnostics,
                                                      const SaveSlotIndexLimits &limits) {
            if (observation.state == SaveSlotArtifactState::Temporary)
                return Result<void>::Success();
            if (observation.state != SaveSlotArtifactState::Committed || !observation.entry || ValidateEntry(*observation.entry).HasError())
                return AddDiagnostic(diagnostics, limits,
                                     {.kind = SaveSlotIndexDiagnosticKind::Corrupt, .slot = observation.suspectedSlot});
            if (committed.size() == limits.maximumArtifacts)
                return Result<void>::Failure(MakeError(SaveErrors::SlotIndexLimitExceeded));
            committed.push_back(*observation.entry);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendEntryGroup(const std::span<const SaveSlotCatalogEntry> group,
                                                    std::vector<SaveSlotCatalogEntry> &unique,
                                                    std::vector<SaveSlotIndexDiagnostic> &diagnostics, const SaveSlotIndexLimits &limits) {
            if (group.size() == 1) {
                if (unique.size() == limits.maximumEntries)
                    return Result<void>::Failure(MakeError(SaveErrors::SlotIndexLimitExceeded));
                unique.push_back(group.front());
                return Result<void>::Success();
            }
            for (const auto &entry : group) {
                if (auto added = AddDiagnostic(diagnostics, limits,
                                               {.kind = SaveSlotIndexDiagnosticKind::Duplicate,
                                                .slot = Slot(entry),
                                                .generation = entry.publication.generation});
                    added.HasError())
                    return added;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<SaveSlotCatalogEntry>> SelectUniqueEntries(const std::vector<SaveSlotCatalogEntry> &committed,
                                                                                    std::vector<SaveSlotIndexDiagnostic> &diagnostics,
                                                                                    const SaveSlotIndexLimits &limits) {
            std::vector<SaveSlotCatalogEntry> unique;
            try {
                unique.reserve(committed.size());
            } catch (const std::bad_alloc &) {
                return Result<std::vector<SaveSlotCatalogEntry>>::Failure(MakeError(SaveErrors::SlotIndexAllocationFailed));
            }
            std::size_t position = 0;
            while (position < committed.size()) {
                std::size_t groupEnd = position + 1;
                while (groupEnd < committed.size() && Slot(committed[groupEnd]) == Slot(committed[position]))
                    ++groupEnd;
                if (auto appended =
                        AppendEntryGroup(std::span{committed}.subspan(position, groupEnd - position), unique, diagnostics, limits);
                    appended.HasError())
                    return Result<std::vector<SaveSlotCatalogEntry>>::Failure(appended.ErrorValue());
                position = groupEnd;
            }
            return Result<std::vector<SaveSlotCatalogEntry>>::Success(std::move(unique));
        }

        enum class IndexEntryRelation : std::uint8_t {
            Missing,
            Orphaned,
            Matched,
        };

        [[nodiscard]] IndexEntryRelation NextRelation(const std::span<const SaveSlotCatalogEntry> previous,
                                                      const std::span<const SaveSlotCatalogEntry> rebuilt, const std::size_t oldPosition,
                                                      const std::size_t newPosition) noexcept {
            using enum IndexEntryRelation;
            if (oldPosition == previous.size())
                return Missing;
            if (newPosition == rebuilt.size())
                return Orphaned;
            if (Slot(rebuilt[newPosition]) < Slot(previous[oldPosition]))
                return Missing;
            if (Slot(previous[oldPosition]) < Slot(rebuilt[newPosition]))
                return Orphaned;
            return Matched;
        }

        struct IndexEntryAdvance final {
            std::size_t previous{};
            std::size_t rebuilt{};
        };

        [[nodiscard]] Result<IndexEntryAdvance> CompareIndexEntry(const std::span<const SaveSlotCatalogEntry> previous,
                                                                  const std::span<const SaveSlotCatalogEntry> rebuilt,
                                                                  const std::size_t oldPosition, const std::size_t newPosition,
                                                                  std::vector<SaveSlotIndexDiagnostic> &diagnostics,
                                                                  const SaveSlotIndexLimits &limits) {
            using enum IndexEntryRelation;
            const IndexEntryRelation relation = NextRelation(previous, rebuilt, oldPosition, newPosition);
            if (relation == Missing) {
                if (auto added = AddDiagnostic(diagnostics, limits,
                                               {.kind = SaveSlotIndexDiagnosticKind::Missing,
                                                .slot = Slot(rebuilt[newPosition]),
                                                .generation = rebuilt[newPosition].publication.generation});
                    added.HasError())
                    return Result<IndexEntryAdvance>::Failure(added.ErrorValue());
                return Result<IndexEntryAdvance>::Success({.rebuilt = 1});
            }
            if (relation == Orphaned) {
                if (auto added = AddDiagnostic(diagnostics, limits,
                                               {.kind = SaveSlotIndexDiagnosticKind::Orphaned,
                                                .slot = Slot(previous[oldPosition]),
                                                .generation = previous[oldPosition].publication.generation});
                    added.HasError())
                    return Result<IndexEntryAdvance>::Failure(added.ErrorValue());
                return Result<IndexEntryAdvance>::Success({.previous = 1});
            }
            if (previous[oldPosition].publication.generation != rebuilt[newPosition].publication.generation) {
                if (auto added = AddDiagnostic(diagnostics, limits,
                                               {.kind = SaveSlotIndexDiagnosticKind::Stale,
                                                .slot = Slot(rebuilt[newPosition]),
                                                .generation = rebuilt[newPosition].publication.generation});
                    added.HasError())
                    return Result<IndexEntryAdvance>::Failure(added.ErrorValue());
            }
            return Result<IndexEntryAdvance>::Success({.previous = 1, .rebuilt = 1});
        }

        [[nodiscard]] Result<void> CompareIndexEntries(const std::span<const SaveSlotCatalogEntry> previous,
                                                       const std::span<const SaveSlotCatalogEntry> rebuilt,
                                                       std::vector<SaveSlotIndexDiagnostic> &diagnostics,
                                                       const SaveSlotIndexLimits &limits) {
            std::size_t oldPosition = 0;
            std::size_t newPosition = 0;
            while (oldPosition < previous.size() || newPosition < rebuilt.size()) {
                auto compared = CompareIndexEntry(previous, rebuilt, oldPosition, newPosition, diagnostics, limits);
                if (compared.HasError())
                    return Result<void>::Failure(compared.ErrorValue());
                oldPosition += compared.Value().previous;
                newPosition += compared.Value().rebuilt;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateSaveSlotIndex */
    Result<void> ValidateSaveSlotIndex(const SaveSlotIndex &index, const SaveSlotIndexLimits &limits) {
        if (!ValidLimits(limits) || index.entries.size() > limits.maximumEntries)
            return Result<void>::Failure(MakeError(SaveErrors::SlotIndexLimitExceeded));
        if (index.schemaVersion != SaveSlotIndexSchemaVersion || index.revision == 0)
            return Result<void>::Failure(MakeError(SaveErrors::SlotIndexInvalid));
        for (std::size_t position = 0; position < index.entries.size(); ++position) {
            if (ValidateEntry(index.entries[position]).HasError())
                return Result<void>::Failure(MakeError(SaveErrors::SlotIndexCorrupt));
            if (position != 0 && !(Slot(index.entries[position - 1]) < Slot(index.entries[position])))
                return Result<void>::Failure(MakeError(SaveErrors::SlotIndexCorrupt));
        }
        return Result<void>::Success();
    }

    SaveSlotIndexRebuilder::SaveSlotIndexRebuilder(std::optional<SaveSlotIndex> previous, const std::uint64_t nextRevision,
                                                   const SaveSlotIndexLimits limits)
        : previous_(std::move(previous)), nextRevision_(nextRevision), limits_(limits) {}

    /** @copydoc SaveSlotIndexRebuilder::Create */
    Result<SaveSlotIndexRebuilder> SaveSlotIndexRebuilder::Create(std::optional<SaveSlotIndex> previous, const std::uint64_t nextRevision,
                                                                  const SaveSlotIndexLimits limits) {
        if (!ValidLimits(limits) || nextRevision == 0)
            return Result<SaveSlotIndexRebuilder>::Failure(MakeError(SaveErrors::SlotIndexInvalid));
        if (previous && ValidateSaveSlotIndex(*previous, limits).HasError())
            previous.reset();
        if (previous && nextRevision <= previous->revision)
            return Result<SaveSlotIndexRebuilder>::Failure(MakeError(SaveErrors::SlotIndexInvalid));
        try {
            SaveSlotIndexRebuilder builder{std::move(previous), nextRevision, limits};
            builder.committed_.reserve(limits.maximumArtifacts);
            builder.diagnostics_.reserve(limits.maximumDiagnostics);
            return Result<SaveSlotIndexRebuilder>::Success(std::move(builder));
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotIndexRebuilder>::Failure(MakeError(SaveErrors::SlotIndexAllocationFailed));
        }
    }

    /** @copydoc SaveSlotIndexRebuilder::Consume */
    Result<std::size_t> SaveSlotIndexRebuilder::Consume(const std::span<const SaveSlotArtifactObservation> observations,
                                                        const std::size_t maximumToConsume) {
        if (sealed_ || maximumToConsume == 0)
            return Result<std::size_t>::Failure(MakeError(SaveErrors::SlotIndexInvalid));
        const std::size_t count = std::min(observations.size(), maximumToConsume);
        if (count > limits_.maximumArtifacts - artifactsExamined_)
            return Result<std::size_t>::Failure(MakeError(SaveErrors::SlotIndexLimitExceeded));
        for (const auto &observation : observations.first(count)) {
            ++artifactsExamined_;
            if (auto consumed = ConsumeObservation(observation, committed_, diagnostics_, limits_); consumed.HasError())
                return Result<std::size_t>::Failure(consumed.ErrorValue());
        }
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc SaveSlotIndexRebuilder::Finalize */
    Result<SaveSlotIndexRebuildResult> SaveSlotIndexRebuilder::Finalize() {
        if (sealed_)
            return Result<SaveSlotIndexRebuildResult>::Failure(MakeError(SaveErrors::SlotIndexInvalid));
        sealed_ = true;
        std::ranges::sort(committed_, EntryLess);
        auto uniqueResult = SelectUniqueEntries(committed_, diagnostics_, limits_);
        if (uniqueResult.HasError())
            return Result<SaveSlotIndexRebuildResult>::Failure(uniqueResult.ErrorValue());
        auto unique = std::move(uniqueResult).Value();
        const std::span<const SaveSlotCatalogEntry> oldEntries =
            previous_ ? std::span{previous_->entries} : std::span<const SaveSlotCatalogEntry>{};
        if (auto compared = CompareIndexEntries(oldEntries, unique, diagnostics_, limits_); compared.HasError())
            return Result<SaveSlotIndexRebuildResult>::Failure(compared.ErrorValue());
        std::ranges::sort(diagnostics_, DiagnosticLess);
        SaveSlotIndex candidate{.schemaVersion = SaveSlotIndexSchemaVersion, .revision = nextRevision_, .entries = std::move(unique)};
        return Result<SaveSlotIndexRebuildResult>::Success(
            {.candidate = std::move(candidate), .diagnostics = std::move(diagnostics_), .artifactsExamined = artifactsExamined_});
    }
}  // namespace Horo::Runtime
