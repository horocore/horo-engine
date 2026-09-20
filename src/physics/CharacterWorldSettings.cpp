#include "Horo/Physics/CharacterWorldSettings.h"

#include "Horo/Physics/CharacterErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <span>

namespace Horo::Character {
    namespace {
        [[nodiscard]] Result<void> Invalid(const char *message) {
            return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, message));
        }

        [[nodiscard]] Result<void> Exceeded(const char *message) {
            return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded, message));
        }

        [[nodiscard]] bool IsZero(const CharacterWorldHistoryBudgets &history) noexcept {
            return history.maximumCheckpoints == 0 && history.maximumBytes == 0 && history.maximumResimulationTicks == 0;
        }

        /** @brief Reports whether every bounded integral setting is non-zero. */
        [[nodiscard]] bool AreNonZero(const std::span<const std::uint64_t> values) noexcept {
            return std::ranges::all_of(values, [](const auto value) {
                return value != 0;
            });
        }

        /** @brief Reports whether corresponding integral settings fit their canonical ceilings. */
        [[nodiscard]] bool AreWithinLimits(const std::span<const std::uint64_t> values,
                                           const std::span<const std::uint64_t> limits) noexcept {
            for (std::size_t index = 0; index < values.size(); ++index) {
                if (values[index] > limits[index]) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateCapacities(const CharacterWorldCapacities &values) {
            const std::array<std::uint64_t, 8> fields{
                values.maximumControllers,   values.maximumQueuedCommands, values.maximumRetainedContacts,  values.maximumQueuedEvents,
                values.maximumQueuedQueries, values.maximumStagedImpulses, values.maximumDiagnosticRecords, values.maximumDebugPrimitives,
            };
            if (!AreNonZero(fields)) {
                return Invalid("Character world retained capacities must all be non-zero.");
            }
            if (constexpr std::array<std::uint64_t, 8> limits{
                    CharacterWorldSettingLimits::MaximumControllers,
                    CharacterWorldSettingLimits::MaximumQueuedCommands,
                    CharacterWorldSettingLimits::MaximumRetainedContacts,
                    CharacterWorldSettingLimits::MaximumQueuedEvents,
                    CharacterWorldSettingLimits::MaximumQueuedQueries,
                    CharacterWorldSettingLimits::MaximumStagedImpulses,
                    CharacterWorldSettingLimits::MaximumDiagnosticRecords,
                    CharacterWorldSettingLimits::MaximumDebugPrimitives,
                };
                !AreWithinLimits(fields, limits)) {
                return Exceeded("Character world retained capacity exceeds a schema-1 hard ceiling.");
            }
            return Result<void>::Success();
        }

        /** @brief Checks basic work-budget representation before ceilings or cross-field policy. */
        [[nodiscard]] Result<void> ValidateWorkValues(const CharacterWorldWorkBudgets &work) {
            if (const std::array<std::uint64_t, 5> values{
                    work.maximumCommandsPerTick,
                    work.maximumQueriesPerTick,
                    work.maximumContactsPerMovement,
                    work.maximumMovementIterations,
                    work.scratchBytes,
                };
                !AreNonZero(values) || !std::isfinite(work.maximumDisplacementMetersPerTick) ||
                work.maximumDisplacementMetersPerTick <= 0.0F) {
                return Invalid("Character world work budgets must be finite and non-zero.");
            }
            return Result<void>::Success();
        }

        /** @brief Checks work against retained storage and canonical schema ceilings. */
        [[nodiscard]] Result<void> ValidateWorkLimits(const CharacterWorldWorkBudgets &work, const CharacterWorldCapacities &capacities) {
            const std::array<std::uint64_t, 5> values{
                work.maximumCommandsPerTick,    work.maximumQueriesPerTick, work.maximumContactsPerMovement,
                work.maximumMovementIterations, work.scratchBytes,
            };
            if (const std::array<std::uint64_t, 5> limits{
                    capacities.maximumQueuedCommands,
                    capacities.maximumQueuedQueries,
                    MaximumCharacterContacts,
                    CharacterWorldSettingLimits::MaximumMovementIterations,
                    CharacterWorldSettingLimits::MaximumScratchBytes,
                };
                !AreWithinLimits(values, limits) ||
                work.maximumRecoveryIterations > CharacterWorldSettingLimits::MaximumRecoveryIterations ||
                work.maximumDisplacementMetersPerTick > CharacterWorldSettingLimits::MaximumDisplacementMetersPerTick) {
                return Exceeded("Character fixed-tick work exceeds retained storage or a schema-1 hard ceiling.");
            }
            return Result<void>::Success();
        }

        /** @brief Checks aggregate contact storage after all individual bounds are admitted. */
        [[nodiscard]] Result<void> ValidateWorkConsistency(const CharacterWorldWorkBudgets &work,
                                                           const CharacterWorldCapacities &capacities) {
            if (const auto retainedContactRequirement =
                    static_cast<std::uint64_t>(capacities.maximumControllers) * work.maximumContactsPerMovement;
                retainedContactRequirement > capacities.maximumRetainedContacts) {
                return Invalid("Retained contact capacity must cover every controller's admitted movement result.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateWork(const CharacterWorldWorkBudgets &work, const CharacterWorldCapacities &capacities) {
            if (const auto values = ValidateWorkValues(work); values.HasError()) {
                return values;
            }
            if (const auto limits = ValidateWorkLimits(work, capacities); limits.HasError()) {
                return limits;
            }
            return ValidateWorkConsistency(work, capacities);
        }

        [[nodiscard]] Result<void> ValidateHistory(const CharacterWorldHistoryBudgets &history) {
            if (IsZero(history)) {
                return Result<void>::Success();
            }
            if (!history.Enabled()) {
                return Invalid("Character history must be fully disabled or provide every checkpoint and resimulation bound.");
            }
            if (history.maximumCheckpoints > CharacterWorldSettingLimits::MaximumHistoryCheckpoints ||
                history.maximumBytes > CharacterWorldSettingLimits::MaximumHistoryBytes ||
                history.maximumResimulationTicks > CharacterWorldSettingLimits::MaximumResimulationTicks) {
                return Exceeded("Character history or resimulation capacity exceeds a schema-1 hard ceiling.");
            }
            if (history.maximumResimulationTicks > history.maximumCheckpoints || history.maximumBytes < history.maximumCheckpoints) {
                return Invalid("Character history must retain at least one byte per checkpoint and cover the resimulation horizon.");
            }
            return Result<void>::Success();
        }

        /**
         * @brief Counts schema-1 members semantically so aggregate growth fails compilation until packing is revised.
         *
         * Structured bindings intentionally couple this guard to member count without depending on ABI size or padding.
         */
        consteval std::size_t SchemaFieldCount() {
            CharacterWorldCapacities capacities;
            const auto &[controllers, commands, contacts, events, queries, impulses, diagnostics, debug] = capacities;
            CharacterWorldWorkBudgets work;
            const auto &[tickCommands, tickQueries, movementContacts, movementIterations, recoveryIterations, scratch, displacement] = work;
            CharacterWorldHistoryBudgets history;
            const auto &[checkpoints, historyBytes, resimulationTicks] = history;
            static_cast<void>(controllers);
            static_cast<void>(commands);
            static_cast<void>(contacts);
            static_cast<void>(events);
            static_cast<void>(queries);
            static_cast<void>(impulses);
            static_cast<void>(diagnostics);
            static_cast<void>(debug);
            static_cast<void>(tickCommands);
            static_cast<void>(tickQueries);
            static_cast<void>(movementContacts);
            static_cast<void>(movementIterations);
            static_cast<void>(recoveryIterations);
            static_cast<void>(scratch);
            static_cast<void>(displacement);
            static_cast<void>(checkpoints);
            static_cast<void>(historyBytes);
            static_cast<void>(resimulationTicks);
            return 18;
        }

        [[nodiscard]] CharacterWorldSettingsIdentity SettingsIdentity(const CharacterWorldSettingsDescriptor &values) noexcept {
            constexpr std::uint64_t SchemaVersion = 1;
            const auto displacement = std::bit_cast<std::uint32_t>(values.work.maximumDisplacementMetersPerTick);
            const std::array<std::uint64_t, 1 + SchemaFieldCount()> words{
                SchemaVersion,
                values.capacities.maximumControllers,
                values.capacities.maximumQueuedCommands,
                values.capacities.maximumRetainedContacts,
                values.capacities.maximumQueuedEvents,
                values.capacities.maximumQueuedQueries,
                values.capacities.maximumStagedImpulses,
                values.capacities.maximumDiagnosticRecords,
                values.capacities.maximumDebugPrimitives,
                values.work.maximumCommandsPerTick,
                values.work.maximumQueriesPerTick,
                values.work.maximumContactsPerMovement,
                values.work.maximumMovementIterations,
                values.work.maximumRecoveryIterations,
                values.work.scratchBytes,
                displacement,
                values.history.maximumCheckpoints,
                values.history.maximumBytes,
                values.history.maximumResimulationTicks,
            };
            std::array<std::byte, words.size() * sizeof(std::uint64_t)> bytes{};
            std::size_t cursor{};
            for (const auto word : words) {
                for (std::uint32_t shift{}; shift < 64; shift += 8) {
                    bytes[cursor++] = static_cast<std::byte>((word >> shift) & 0xffU);
                }
            }
            return {ComputeSha256(bytes)};
        }
    }  // namespace

    /** @copydoc CharacterWorldSettings::Capture */
    Result<CharacterWorldSettings> CharacterWorldSettings::Capture(const CharacterWorldSettingsDescriptor &descriptor) {
        if (const auto capacities = ValidateCapacities(descriptor.capacities); capacities.HasError()) {
            return Result<CharacterWorldSettings>::Failure(capacities.ErrorValue());
        }
        if (const auto work = ValidateWork(descriptor.work, descriptor.capacities); work.HasError()) {
            return Result<CharacterWorldSettings>::Failure(work.ErrorValue());
        }
        if (const auto history = ValidateHistory(descriptor.history); history.HasError()) {
            return Result<CharacterWorldSettings>::Failure(history.ErrorValue());
        }
        return Result<CharacterWorldSettings>::Success(CharacterWorldSettings{descriptor, SettingsIdentity(descriptor)});
    }

    /** @copydoc CharacterWorldSettings::CharacterWorldSettings */
    CharacterWorldSettings::CharacterWorldSettings(const CharacterWorldSettingsDescriptor &values,
                                                   const CharacterWorldSettingsIdentity &identity)
        : values_(values), identity_(identity) {}

    /** @copydoc CharacterWorldSettings::Values */
    const CharacterWorldSettingsDescriptor &CharacterWorldSettings::Values() const noexcept {
        return values_;
    }

    /** @copydoc CharacterWorldSettings::Identity */
    const CharacterWorldSettingsIdentity &CharacterWorldSettings::Identity() const noexcept {
        return identity_;
    }
}  // namespace Horo::Character
