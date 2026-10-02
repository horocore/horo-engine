#include "Horo/AI/AICanonicalState.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <array>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Finds a stable key in an admitted sorted schema. */
        [[nodiscard]] std::size_t KeyIndex(const BlackboardSchema &schema, const BlackboardKeyId key) noexcept {
            const auto keys = schema.Keys();
            const auto found = std::ranges::lower_bound(keys, key, {}, &BlackboardKeyDescriptor::key);
            return found != keys.end() && found->key == key ? static_cast<std::size_t>(found - keys.begin()) : keys.size();
        }

        /** @brief Requires well-formed unique mapping sources and available non-discard destinations. */
        [[nodiscard]] Result<void> ValidateMapping(const BlackboardSchemaMigration &migration, const BlackboardSchema &destination) {
            if (migration.keys.size() > MaximumBlackboardKeys)
                return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            std::array<bool, MaximumBlackboardKeys> seen{};
            for (const auto &mapping : migration.keys) {
                const auto index = KeyIndex(*migration.source, mapping.source);
                if (index == migration.source->Keys().size() || seen[index] ||
                    (mapping.destination.IsValid() && KeyIndex(destination, mapping.destination) == destination.Keys().size()))
                    return Result<void>::Failure(MakeError(AIErrors::CanonicalMigrationFailed));
                seen[index] = true;
            }
            return Result<void>::Success();
        }

        /** @brief Resolves exactly one declared source schema for a direct forward migration. */
        [[nodiscard]] Result<const BlackboardSchemaMigration *> SelectMigration(
            const BlackboardCanonicalState &state, const BlackboardSchema &destination,
            const std::span<const BlackboardSchemaMigration> migrations) {
            const BlackboardSchemaMigration *selected = nullptr;
            for (const auto &migration : migrations) {
                if (!migration.source || migration.source->Identity() != state.schema ||
                    migration.source->Version() != state.schemaVersion || migration.destination != destination.Identity() ||
                    migration.destinationVersion != destination.Version())
                    continue;
                if (selected != nullptr)
                    return Result<const BlackboardSchemaMigration *>::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
                selected = &migration;
            }
            if (selected == nullptr || state.schema != destination.Identity() || state.schemaVersion >= destination.Version())
                return Result<const BlackboardSchemaMigration *>::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
            return Result<const BlackboardSchemaMigration *>::Success(selected);
        }

        /** @brief Applies mapped source values to detached target defaults, rejecting collisions or implicit removals. */
        [[nodiscard]] Result<BlackboardCanonicalState> ApplyMigration(const BlackboardCanonicalState &state,
                                                                      const BlackboardSchema &destination,
                                                                      const BlackboardSchemaMigration &migration) {
            BlackboardCanonicalState candidate{.schema = destination.Identity(), .schemaVersion = destination.Version()};
            candidate.entries.reserve(destination.Keys().size());
            for (const auto &key : destination.Keys())
                candidate.entries.push_back({key.key, key.defaultValue});
            std::array<bool, MaximumBlackboardKeys> assigned{};
            for (const auto &entry : state.entries) {
                const auto mapping = std::ranges::find(migration.keys, entry.key, &BlackboardKeyMigration::source);
                const BlackboardKeyId target = mapping == migration.keys.end() ? entry.key : mapping->destination;
                if (!target.IsValid())
                    continue;
                const auto index = KeyIndex(destination, target);
                if (index == destination.Keys().size() || assigned[index])
                    return Result<BlackboardCanonicalState>::Failure(MakeError(AIErrors::CanonicalMigrationFailed));
                assigned[index] = true;
                candidate.entries[index].value = entry.value;
            }
            if (const auto valid = ValidateCanonicalBlackboard(candidate, destination); valid.HasError())
                return Result<BlackboardCanonicalState>::Failure(WrapError(AIErrors::CanonicalMigrationFailed, valid.ErrorValue()));
            return Result<BlackboardCanonicalState>::Success(std::move(candidate));
        }
    }  // namespace

    /** @copydoc ValidateCanonicalBlackboard */
    Result<void> ValidateCanonicalBlackboard(const BlackboardCanonicalState &state, const BlackboardSchema &schema) {
        if (state.schema != schema.Identity() || state.schemaVersion != schema.Version())
            return Result<void>::Failure(MakeError(AIErrors::CanonicalSchemaUnsupported));
        if (state.entries.size() != schema.Keys().size() || state.entries.size() > MaximumBlackboardKeys)
            return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        for (std::size_t index = 0; index < state.entries.size(); ++index) {
            const auto &entry = state.entries[index];
            const auto &key = schema.Keys()[index];
            if (entry.key != key.key || (!entry.value && key.presence == BlackboardKeyPresence::Required))
                return Result<void>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
            if (entry.value) {
                if (const auto valid = ValidateBlackboardValue(*entry.value, &key, schema.UnknownValuePolicy()); valid.HasError())
                    return valid;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc MigrateCanonicalBlackboard */
    Result<BlackboardCanonicalState> MigrateCanonicalBlackboard(const BlackboardCanonicalState &state, const BlackboardSchema &destination,
                                                                const std::span<const BlackboardSchemaMigration> migrations) {
        if (migrations.size() > MaximumAiSchemaMigrations || state.entries.size() > MaximumBlackboardKeys)
            return Result<BlackboardCanonicalState>::Failure(MakeError(AIErrors::CanonicalStateInvalid));
        try {
            if (state.schema == destination.Identity() && state.schemaVersion == destination.Version()) {
                if (const auto valid = ValidateCanonicalBlackboard(state, destination); valid.HasError())
                    return Result<BlackboardCanonicalState>::Failure(valid.ErrorValue());
                return Result<BlackboardCanonicalState>::Success(state);
            }
            const auto selected = SelectMigration(state, destination, migrations);
            if (selected.HasError())
                return Result<BlackboardCanonicalState>::Failure(selected.ErrorValue());
            const auto &migration = *selected.Value();
            if (const auto valid = ValidateCanonicalBlackboard(state, *migration.source); valid.HasError())
                return Result<BlackboardCanonicalState>::Failure(valid.ErrorValue());
            if (const auto valid = ValidateMapping(migration, destination); valid.HasError())
                return Result<BlackboardCanonicalState>::Failure(valid.ErrorValue());
            return ApplyMigration(state, destination, migration);
        } catch (const std::bad_alloc &) {
            return Result<BlackboardCanonicalState>::Failure(MakeError(AIErrors::BlackboardStorageUnavailable));
        }
    }
}  // namespace Horo::AI
