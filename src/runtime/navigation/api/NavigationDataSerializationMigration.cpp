#include "Horo/Navigation/NavigationDataSerialization.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "NavigationDataSerializationInternal.h"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    namespace {
        using namespace SerializationInternal;

        [[nodiscard]] Result<void> ValidateMigrationSteps(const std::span<const NavigationSourceMigrationStep> steps) {
            for (const auto &step : steps) {
                if (!IsSupportedEnvelopeVersion(step.from) || !IsSupportedEnvelopeVersion(step.to) || step.from >= step.to ||
                    step.upgrade == nullptr)
                    return Result<void>::Failure(MakeError(NavigationErrors::SourceMigrationInvalid));
                for (const auto &other : steps) {
                    if (&step != &other && step.from == other.from)
                        return Result<void>::Failure(MakeError(NavigationErrors::SourceMigrationInvalid));
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] const NavigationSourceMigrationStep *FindMigrationStep(const std::span<const NavigationSourceMigrationStep> steps,
                                                                             const NavigationSourceSchemaVersion from) noexcept {
            const NavigationSourceMigrationStep *selected = nullptr;
            for (const auto &step : steps) {
                if (step.from == from)
                    selected = &step;
            }
            return selected;
        }

        [[nodiscard]] Result<std::vector<NavigationAuthoredRecord>> ApplyMigrationStep(const std::vector<NavigationAuthoredRecord> &records,
                                                                                       const NavigationSourceMigrationStep &step,
                                                                                       const NavigationSourceLoadContext &context) {
            auto migrated = step.upgrade(records);
            if (migrated.HasError())
                return Result<std::vector<NavigationAuthoredRecord>>::Failure(migrated.ErrorValue());
            auto candidate = NavigationSourceRecords::Create(step.to, std::move(migrated).Value(), context);
            if (candidate.HasError())
                return Result<std::vector<NavigationAuthoredRecord>>::Failure(candidate.ErrorValue());
            return Result<std::vector<NavigationAuthoredRecord>>::Success(
                {candidate.Value().Records().begin(), candidate.Value().Records().end()});
        }
    }  // namespace

    /** @copydoc NavigationSourceRecords::SchemaVersion */
    NavigationSourceSchemaVersion NavigationSourceRecords::SchemaVersion() const noexcept {
        return schemaVersion_;
    }

    /** @copydoc NavigationSourceRecords::Version */
    NavigationSourceSchemaVersion NavigationSourceRecords::Version() const noexcept {
        return SchemaVersion();
    }

    /** @copydoc NavigationSourceRecords::Records */
    std::span<const NavigationAuthoredRecord> NavigationSourceRecords::Records() const noexcept {
        return records_;
    }

    /** @copydoc NavigationSourceRecords::GeneratedPayloads */
    std::span<const NavigationGeneratedPayload> NavigationSourceRecords::GeneratedPayloads() const noexcept {
        return generatedPayloads_;
    }

    /** @copydoc NavigationSourceRecords::QuarantinedGeneratedPayloads */
    std::span<const NavigationQuarantinedGeneratedPayload> NavigationSourceRecords::QuarantinedGeneratedPayloads() const noexcept {
        return quarantinedGeneratedPayloads_;
    }

    /** @copydoc NavigationSourceRecords::HasQuarantinedGeneratedPayloads */
    bool NavigationSourceRecords::HasQuarantinedGeneratedPayloads() const noexcept {
        return !quarantinedGeneratedPayloads_.empty();
    }

    /** @copydoc MigrateNavigationSourceRecords */
    Result<NavigationSourceRecords> MigrateNavigationSourceRecords(const NavigationSourceRecords &source,
                                                                   const NavigationSourceSchemaVersion targetVersion,
                                                                   const std::span<const NavigationSourceMigrationStep> steps,
                                                                   const NavigationSourceLoadContext &context) {
        if (!IsSupportedEnvelopeVersion(targetVersion))
            return Failure<NavigationSourceRecords>(NavigationErrors::SourceUnsupportedVersion);
        if (targetVersion == source.SchemaVersion()) {
            std::vector<NavigationGeneratedPayload> generatedPayloads{source.GeneratedPayloads().begin(), source.GeneratedPayloads().end()};
            for (const auto &quarantined : source.QuarantinedGeneratedPayloads())
                generatedPayloads.push_back(quarantined.payload);
            return NavigationSourceRecords::Create(targetVersion,
                                                   std::vector<NavigationAuthoredRecord>{source.Records().begin(), source.Records().end()},
                                                   std::move(generatedPayloads), context);
        }
        if (targetVersion < source.SchemaVersion())
            return Failure<NavigationSourceRecords>(NavigationErrors::SourceMigrationMissing);

        if (const auto valid = ValidateMigrationSteps(steps); valid.HasError())
            return Result<NavigationSourceRecords>::Failure(valid.ErrorValue());

        std::vector<NavigationAuthoredRecord> records{source.Records().begin(), source.Records().end()};
        NavigationSourceSchemaVersion current = source.SchemaVersion();
        std::size_t applied{};
        while (current < targetVersion) {
            const NavigationSourceMigrationStep *selected = FindMigrationStep(steps, current);
            if (selected == nullptr)
                return Failure<NavigationSourceRecords>(NavigationErrors::SourceMigrationMissing);
            if (++applied > steps.size())
                return Failure<NavigationSourceRecords>(NavigationErrors::SourceMigrationInvalid);
            auto migrated = ApplyMigrationStep(records, *selected, context);
            if (migrated.HasError())
                return Result<NavigationSourceRecords>::Failure(migrated.ErrorValue());
            records = std::move(migrated).Value();
            current = selected->to;
        }
        if (current != targetVersion)
            return Failure<NavigationSourceRecords>(NavigationErrors::SourceMigrationMissing);
        return NavigationSourceRecords::Create(targetVersion, std::move(records), context);
    }

    /** @copydoc UpgradeNavigationSourceRecords */
    Result<NavigationSourceRecords> UpgradeNavigationSourceRecords(const NavigationSourceRecords &source,
                                                                   const NavigationSourceSchemaVersion targetVersion,
                                                                   const std::span<const NavigationSourceMigrationStep> steps,
                                                                   const NavigationSourceLoadContext &context) {
        return MigrateNavigationSourceRecords(source, targetVersion, steps, context);
    }
}  // namespace Horo::Navigation
