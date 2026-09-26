#include "Horo/Runtime/Save/SaveManagerProjection.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::Runtime {
    struct SaveManagerProjection::Data final {
        SaveManagerSnapshotId id;
        SaveManagerProjectionLimits limits;
        std::vector<SaveManagerProfileRow> profiles;
        std::vector<SaveManagerSlotRow> slots;
        std::vector<SaveManagerOperationRow> operations;
        std::vector<SaveManagerDiagnostic> diagnostics;
    };

    namespace {
        [[nodiscard]] bool ValidLimits(const SaveManagerProjectionLimits &limits) noexcept {
            return limits.maximumProfiles > 0 && limits.maximumProfiles <= 64 && limits.maximumSlots > 0 && limits.maximumSlots <= 4'096 &&
                   limits.maximumOperations > 0 && limits.maximumOperations <= 64 && limits.maximumDiagnostics > 0 &&
                   limits.maximumDiagnostics <= 128 && limits.maximumPageSize > 0 && limits.maximumPageSize <= 128 &&
                   limits.maximumFilterBytes > 0 && limits.maximumFilterBytes <= 256;
        }

        [[nodiscard]] bool ValidAssessment(const SaveManagerSlotAssessment &assessment) noexcept {
            return assessment.slot.IsValid() && assessment.generation.IsValid() &&
                   assessment.compatibility <= SaveManagerCompatibility::Unsupported &&
                   assessment.integrity <= SaveManagerIntegrity::Failed;
        }

        [[nodiscard]] bool ValidDiagnostic(const SaveManagerDiagnostic &diagnostic) noexcept {
            return diagnostic.kind <= SaveManagerDiagnosticKind::Unknown && (!diagnostic.slot || diagnostic.slot->IsValid()) &&
                   (!diagnostic.generation || (diagnostic.slot && diagnostic.generation->IsValid())) &&
                   (!diagnostic.operation || *diagnostic.operation != 0);
        }

        [[nodiscard]] bool ValidOperation(const SaveManagerOperationSource &source) noexcept {
            return source.snapshot.operation != 0 && source.snapshot.kind <= SaveOperationKind::Delete &&
                   source.snapshot.state <= SaveOperationState::Cancelled && source.snapshot.stage <= SaveOperationStage::Deleting &&
                   source.snapshot.commit <= SaveOperationCommitOutcome::Unknown && source.snapshot.progress.totalUnits != 0 &&
                   source.snapshot.progress.completedUnits <= source.snapshot.progress.totalUnits &&
                   (!source.slot || source.slot->IsValid()) && (!source.generation || (source.slot && source.generation->IsValid())) &&
                   (!source.failureCategory ||
                    (source.snapshot.state == SaveOperationState::Failed && *source.failureCategory <= SaveManagerDiagnosticKind::Unknown));
        }

        [[nodiscard]] bool Matches(const SaveManagerSlotRow &row, const SaveManagerSlotFilter &filter) noexcept {
            return (!filter.kind || row.kind == *filter.kind) && (!filter.cloud || row.cloud == *filter.cloud) &&
                   (!filter.compatibility || row.compatibility == *filter.compatibility) &&
                   (!filter.integrity || row.integrity == *filter.integrity) &&
                   row.displayName.find(filter.nameContains) != std::string::npos;
        }

        [[nodiscard]] bool ValidFilter(const SaveManagerSlotFilter &filter, const SaveManagerProjectionLimits &limits) noexcept {
            return filter.nameContains.size() <= limits.maximumFilterBytes && (!filter.kind || *filter.kind <= SaveSlotKind::System) &&
                   (!filter.cloud || *filter.cloud <= SaveSlotCloudState::Conflict) &&
                   (!filter.compatibility || *filter.compatibility <= SaveManagerCompatibility::Unsupported) &&
                   (!filter.integrity || *filter.integrity <= SaveManagerIntegrity::Failed);
        }

        [[nodiscard]] Result<void> ValidateInput(const SaveManagerProjectionInput &input, const SaveManagerProjectionLimits &limits) {
            if (!ValidLimits(limits) || input.publicationRevision == 0 || input.binding.revision == 0 || !input.binding.active ||
                input.binding.state != SaveNamespaceBindingState::Available || !input.binding.active->IsValid())
                return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
            if (input.profiles.size() > limits.maximumProfiles || input.catalog.entries.size() > limits.maximumSlots ||
                input.assessments.size() > input.catalog.entries.size() || input.operations.size() > limits.maximumOperations ||
                input.diagnostics.size() > limits.maximumDiagnostics)
                return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionLimitExceeded));
            if (auto index = ValidateSaveSlotIndex(input.catalog, {.maximumEntries = limits.maximumSlots}); index.HasError())
                return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));

            bool activeFound = false;
            std::optional<SaveNamespaceId> previousProfile;
            for (const auto &profile : input.profiles) {
                if (!profile.namespaceId.IsValid() || profile.namespaceId.product != input.binding.active->product ||
                    profile.namespaceId.environment != input.binding.active->environment ||
                    (previousProfile && !(*previousProfile < profile.namespaceId)))
                    return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
                if (profile.namespaceId == *input.binding.active) {
                    if (!profile.available)
                        return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
                    activeFound = true;
                }
                previousProfile = profile.namespaceId;
            }
            if (!activeFound)
                return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));

            std::size_t entryPosition = 0;
            for (const auto &assessment : input.assessments) {
                if (!ValidAssessment(assessment))
                    return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
                while (entryPosition < input.catalog.entries.size() &&
                       input.catalog.entries[entryPosition].publication.slot < assessment.slot)
                    ++entryPosition;
                if (entryPosition == input.catalog.entries.size() ||
                    input.catalog.entries[entryPosition].publication.slot != assessment.slot ||
                    input.catalog.entries[entryPosition].publication.generation != assessment.generation)
                    return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionStale));
                ++entryPosition;
            }
            for (const auto &operation : input.operations) {
                if (!ValidOperation(operation))
                    return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
            }
            for (const auto &diagnostic : input.diagnostics) {
                if (!ValidDiagnostic(diagnostic))
                    return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Copies validated opaque profile summaries into detached view rows. */
        void AppendProfiles(std::vector<SaveManagerProfileRow> &rows, const SaveManagerProjectionInput &input) {
            for (const auto &profile : input.profiles)
                rows.push_back({profile.namespaceId, profile.available, profile.namespaceId == *input.binding.active});
        }

        /** @brief Combines validated catalog facts with generation-matched assessments. */
        void AppendSlots(std::vector<SaveManagerSlotRow> &rows, const SaveManagerProjectionInput &input) {
            std::size_t assessmentPosition = 0;
            for (const auto &entry : input.catalog.entries) {
                const auto &publication = entry.publication;
                SaveManagerSlotRow row{.slot = publication.slot,
                                       .generation = publication.generation,
                                       .kind = publication.kind,
                                       .cloud = publication.cloudState,
                                       .savedAtUnixMilliseconds = publication.savedAtUnixMilliseconds,
                                       .playTimeNanoseconds = publication.playTimeNanoseconds,
                                       .baseScene = publication.baseScene,
                                       .saveSchema = publication.saveSchema,
                                       .productCompatibility = publication.productCompatibility,
                                       .checkpoint = publication.checkpoint,
                                       .thumbnail = publication.thumbnail,
                                       .displayName = entry.display.displayName,
                                       .summary = entry.display.summary};
                if (assessmentPosition < input.assessments.size() && input.assessments[assessmentPosition].slot == row.slot) {
                    row.compatibility = input.assessments[assessmentPosition].compatibility;
                    row.integrity = input.assessments[assessmentPosition].integrity;
                    ++assessmentPosition;
                }
                rows.push_back(std::move(row));
            }
        }

        /** @brief Copies progress facts without retaining raw terminal errors. */
        [[nodiscard]] bool AppendOperations(std::vector<SaveManagerOperationRow> &rows, const SaveManagerProjectionInput &input) {
            for (const auto &source : input.operations) {
                const auto &operation = source.snapshot;
                rows.push_back({.operation = operation.operation,
                                .kind = operation.kind,
                                .state = operation.state,
                                .stage = operation.stage,
                                .progress = operation.progress,
                                .commit = operation.commit,
                                .slot = source.slot,
                                .generation = source.generation,
                                .failureCategory = source.failureCategory});
            }
            std::ranges::sort(rows, {}, &SaveManagerOperationRow::operation);
            return std::adjacent_find(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
                return left.operation == right.operation;
            }) == rows.end();
        }
    }  // namespace

    SaveManagerProjection::SaveManagerProjection(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}

    /** @copydoc SaveManagerProjection::Create */
    Result<SaveManagerProjection> SaveManagerProjection::Create(const SaveManagerProjectionInput &input,
                                                                const SaveManagerProjectionLimits limits) {
        if (auto validated = ValidateInput(input, limits); validated.HasError())
            return Result<SaveManagerProjection>::Failure(validated.ErrorValue());
        try {
            auto data = std::make_shared<Data>();
            data->id = {.namespaceId = *input.binding.active,
                        .bindingRevision = input.binding.revision,
                        .catalogRevision = input.catalog.revision,
                        .publicationRevision = input.publicationRevision};
            data->limits = limits;
            data->profiles.reserve(input.profiles.size());
            data->slots.reserve(input.catalog.entries.size());
            data->operations.reserve(input.operations.size());
            data->diagnostics.reserve(input.diagnostics.size());

            AppendProfiles(data->profiles, input);
            AppendSlots(data->slots, input);
            if (!AppendOperations(data->operations, input))
                return Result<SaveManagerProjection>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
            data->diagnostics.assign(input.diagnostics.begin(), input.diagnostics.end());
            return Result<SaveManagerProjection>::Success(SaveManagerProjection{std::move(data)});
        } catch (const std::bad_alloc &) {
            return Result<SaveManagerProjection>::Failure(MakeError(SaveErrors::ManagerProjectionAllocationFailed));
        } catch (const std::length_error &) {
            return Result<SaveManagerProjection>::Failure(MakeError(SaveErrors::ManagerProjectionLimitExceeded));
        }
    }

    /** @copydoc SaveManagerProjection::Id */
    const SaveManagerSnapshotId &SaveManagerProjection::Id() const noexcept {
        return data_->id;
    }

    /** @copydoc SaveManagerProjection::Profiles */
    std::span<const SaveManagerProfileRow> SaveManagerProjection::Profiles() const noexcept {
        return data_->profiles;
    }

    /** @copydoc SaveManagerProjection::Operations */
    std::span<const SaveManagerOperationRow> SaveManagerProjection::Operations() const noexcept {
        return data_->operations;
    }

    /** @copydoc SaveManagerProjection::Diagnostics */
    std::span<const SaveManagerDiagnostic> SaveManagerProjection::Diagnostics() const noexcept {
        return data_->diagnostics;
    }

    /** @copydoc SaveManagerProjection::Detail */
    std::optional<SaveManagerSlotRow> SaveManagerProjection::Detail(const SaveGameSlotId slot) const {
        const auto found = std::ranges::lower_bound(data_->slots, slot, {}, &SaveManagerSlotRow::slot);
        if (found == data_->slots.end() || found->slot != slot)
            return std::nullopt;
        return *found;
    }

    /** @copydoc SaveManagerProjection::Page */
    Result<SaveManagerPage> SaveManagerProjection::Page(const SaveManagerPageRequest &request) const {
        if (!ValidFilter(request.filter, data_->limits) || request.maximumRows == 0 || request.maximumRows > data_->limits.maximumPageSize)
            return Result<SaveManagerPage>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
        if (ValidateSaveSlotDisplayMetadata({.displayName = request.filter.nameContains}).HasError())
            return Result<SaveManagerPage>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
        if (request.cursor && (request.cursor->snapshot != data_->id || request.cursor->filter != request.filter))
            return Result<SaveManagerPage>::Failure(MakeError(SaveErrors::ManagerProjectionStale));
        const std::size_t nextMatch = request.cursor ? request.cursor->nextMatch : 0;
        SaveManagerPage page;
        try {
            page.rows.reserve(request.maximumRows);
            for (const auto &row : data_->slots) {
                if (!Matches(row, request.filter))
                    continue;
                if (page.totalMatches >= nextMatch && page.rows.size() < request.maximumRows)
                    page.rows.push_back(row);
                ++page.totalMatches;
            }
            if (nextMatch > page.totalMatches)
                return Result<SaveManagerPage>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
            if (nextMatch + page.rows.size() < page.totalMatches)
                page.next = SaveManagerPageCursor{data_->id, request.filter, nextMatch + page.rows.size()};
            return Result<SaveManagerPage>::Success(std::move(page));
        } catch (const std::bad_alloc &) {
            return Result<SaveManagerPage>::Failure(MakeError(SaveErrors::ManagerProjectionAllocationFailed));
        }
    }

    /** @copydoc SaveManagerProjection::Command */
    Result<SaveManagerCommand> SaveManagerProjection::Command(const SaveManagerCommandKind kind, const SaveGameSlotId slot) const {
        if (kind > SaveManagerCommandKind::Delete || !slot.IsValid())
            return Result<SaveManagerCommand>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
        const auto found = std::ranges::lower_bound(data_->slots, slot, {}, &SaveManagerSlotRow::slot);
        if (found == data_->slots.end() || found->slot != slot)
            return Result<SaveManagerCommand>::Failure(MakeError(SaveErrors::ManagerProjectionStale));
        return Result<SaveManagerCommand>::Success({kind, data_->id, slot, found->generation});
    }

    /** @copydoc SaveManagerProjection::ValidateCommand */
    Result<void> SaveManagerProjection::ValidateCommand(const SaveManagerCommand &command) const {
        if (command.kind > SaveManagerCommandKind::Delete || !command.slot.IsValid() || !command.expectedGeneration.IsValid())
            return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionInvalid));
        if (command.expectedSnapshot != data_->id)
            return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionStale));
        const auto found = std::ranges::lower_bound(data_->slots, command.slot, {}, &SaveManagerSlotRow::slot);
        if (found == data_->slots.end() || found->slot != command.slot || found->generation != command.expectedGeneration)
            return Result<void>::Failure(MakeError(SaveErrors::ManagerProjectionStale));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
