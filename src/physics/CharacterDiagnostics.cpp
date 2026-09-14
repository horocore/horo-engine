#include "Horo/Physics/CharacterDiagnostics.h"

#include "Horo/Physics/CharacterErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::Character {
    namespace {
        constexpr std::uint32_t MaximumEmissionsPerTick = 1'024;
        constexpr std::uint64_t MaximumMinimumTickInterval = 1'000'000;

        const std::array CharacterDescriptors{
            &CharacterErrors::DescriptorInvalid,    &CharacterErrors::RequestInvalid, &CharacterErrors::CommandOrderInvalid,
            &CharacterErrors::OperationUnsupported, &CharacterErrors::WorldInvalid,   &CharacterErrors::HandleMalformed,
            &CharacterErrors::HandleWorldMismatch,  &CharacterErrors::HandleStale,    &CharacterErrors::GenerationExhausted,
            &CharacterErrors::CapacityExceeded,     &CharacterErrors::InvalidState,
        };

        const std::array PhysicsDescriptors{
            &Physics::PhysicsErrors::WorldInvalid,
            &Physics::PhysicsErrors::HandleMalformed,
            &Physics::PhysicsErrors::HandleWorldMismatch,
            &Physics::PhysicsErrors::HandleStale,
            &Physics::PhysicsErrors::GenerationExhausted,
            &Physics::PhysicsErrors::CapabilityUnavailable,
            &Physics::PhysicsErrors::OperationUnsupported,
            &Physics::PhysicsErrors::InvalidState,
            &Physics::PhysicsErrors::ThreadAffinityViolation,
            &Physics::PhysicsErrors::SolverDeadlineExceeded,
            &Physics::PhysicsErrors::SolverValidationMessage,
            &Physics::PhysicsErrors::SolverAssertionFailed,
            &Physics::PhysicsErrors::SolverFatalCondition,
            &Physics::PhysicsErrors::DescriptorInvalid,
            &Physics::PhysicsErrors::ProfileUnsupported,
            &Physics::PhysicsErrors::CapacityExceeded,
            &Physics::PhysicsErrors::CapabilityStale,
            &Physics::PhysicsErrors::QuerySnapshotStale,
            &Physics::PhysicsErrors::InitializationFailed,
        };

        struct PhysicsCauseProjection final {
            std::optional<DiagnosticCode> code;
            bool valid{true};
        };

        [[nodiscard]] PhysicsCauseProjection OriginatingPhysicsCode(const Error &error) {
            const Error *candidate = error.cause.Get();
            std::size_t depth{};
            while (candidate != nullptr && depth < MaximumCharacterDiagnosticCauseDepth) {
                if (candidate->domain.Value() == "horo.physics") {
                    auto code = DiagnosticCodeForDeclaredError(*candidate, "horo.physics", PhysicsDescriptors);
                    const bool valid = code.has_value();
                    return {std::move(code), valid};
                }
                candidate = candidate->cause.Get();
                ++depth;
            }
            return {{}, candidate == nullptr};
        }

        [[nodiscard]] bool ValidateMetadata(const CharacterDiagnosticMetadataEntry &entry) noexcept {
            switch (entry.key) {
                using enum CharacterDiagnosticMetadataKey;
                case PhysicsWorld:
                    if (const auto *world = std::get_if<Physics::PhysicsWorldId>(&entry.value))
                        return world->IsValid();
                    return false;
                case OperationSequence:
                    if (const auto *value = std::get_if<std::uint64_t>(&entry.value))
                        return *value != 0;
                    return false;
                case RequestedCount:
                case Capacity:
                    return std::holds_alternative<std::uint64_t>(entry.value);
            }
            return false;
        }

        [[nodiscard]] bool ValidateRecordInput(const Error &error, const CharacterDiagnosticContext &context,
                                               const std::optional<DiagnosticCode> &code,
                                               const std::optional<DiagnosticSeverity> &severity) noexcept {
            const std::array valid{
                code.has_value(),
                severity.has_value(),
                !error.message.empty(),
                error.message.size() <= MaximumCharacterDiagnosticMessageBytes,
                context.controller.IsValid(),
                context.sceneGeneration != 0,
                context.simulationTick != 0,
                context.controller.sceneGeneration == context.sceneGeneration,
                context.metadata.size() <= MaximumCharacterDiagnosticMetadataEntries,
            };
            return std::ranges::all_of(valid, std::identity{});
        }

        [[nodiscard]] bool ValidateOrderedMetadata(const std::span<const CharacterDiagnosticMetadataEntry> metadata) noexcept {
            std::optional<CharacterDiagnosticMetadataKey> previousKey;
            for (const CharacterDiagnosticMetadataEntry &entry : metadata) {
                if ((previousKey.has_value() && entry.key <= *previousKey) || !ValidateMetadata(entry))
                    return false;
                previousKey = entry.key;
            }
            return true;
        }

        void SaturatingIncrement(std::uint64_t &value) noexcept {
            if (value != std::numeric_limits<std::uint64_t>::max())
                ++value;
        }
    }  // namespace

    /** @copydoc CharacterDiagnosticRateLimiter::CharacterDiagnosticRateLimiter */
    CharacterDiagnosticRateLimiter::CharacterDiagnosticRateLimiter(const CharacterDiagnosticRatePolicy policy) noexcept : policy_(policy) {}

    /** @copydoc CharacterDiagnosticRateLimiter::Create */
    Result<CharacterDiagnosticRateLimiter> CharacterDiagnosticRateLimiter::Create(const CharacterDiagnosticRatePolicy policy) {
        if (policy.maximumEmissionsPerTick == 0 || policy.maximumEmissionsPerTick > MaximumEmissionsPerTick ||
            policy.minimumTickInterval == 0 || policy.minimumTickInterval > MaximumMinimumTickInterval)
            return Result<CharacterDiagnosticRateLimiter>::Failure(
                MakeError(CharacterErrors::DescriptorInvalid, "Character diagnostic rate policy is outside its finite bounds."));
        return Result<CharacterDiagnosticRateLimiter>::Success(CharacterDiagnosticRateLimiter{policy});
    }

    /** @copydoc CharacterDiagnosticRateLimiter::TryAdmit */
    CharacterDiagnosticAdmission CharacterDiagnosticRateLimiter::TryAdmit(const std::uint64_t simulationTick) noexcept {
        if (simulationTick == 0 || simulationTick < lastTick_) {
            SaturatingIncrement(statistics_.rejectedTicks);
            return CharacterDiagnosticAdmission::RejectedTick;
        }
        if (simulationTick != lastTick_) {
            lastTick_ = simulationTick;
            emissionsThisTick_ = 0;
        }
        const bool intervalClosed = lastEmissionTick_ != 0 && simulationTick != lastEmissionTick_ &&
                                    simulationTick - lastEmissionTick_ < policy_.minimumTickInterval;
        if (intervalClosed || emissionsThisTick_ == policy_.maximumEmissionsPerTick) {
            SaturatingIncrement(statistics_.suppressed);
            return CharacterDiagnosticAdmission::SuppressedRate;
        }
        ++emissionsThisTick_;
        lastEmissionTick_ = simulationTick;
        SaturatingIncrement(statistics_.emitted);
        return CharacterDiagnosticAdmission::Emit;
    }

    /** @copydoc CharacterDiagnosticRateLimiter::Statistics */
    CharacterDiagnosticRateStatistics CharacterDiagnosticRateLimiter::Statistics() const noexcept {
        return statistics_;
    }

    /** @copydoc CharacterDiagnosticCategoryName */
    std::string_view CharacterDiagnosticCategoryName(const CharacterDiagnosticCategory category) noexcept {
        switch (category) {
            using enum CharacterDiagnosticCategory;
            case Command:
                return "character.command";
            case Lifecycle:
                return "character.lifecycle";
            case Solver:
                return "character.solver";
            case Query:
                return "character.query";
            case Capacity:
                return "character.capacity";
        }
        return {};
    }

    /** @copydoc MakeCharacterDiagnosticRecord */
    Result<CharacterDiagnosticRecord> MakeCharacterDiagnosticRecord(const CharacterDiagnosticCategory category, const Error &error,
                                                                    const CharacterDiagnosticContext &context) {
        if (CharacterDiagnosticCategoryName(category).empty())
            return Result<CharacterDiagnosticRecord>::Failure(
                MakeError(CharacterErrors::OperationUnsupported, "Unknown Character diagnostic category."));
        const auto code = DiagnosticCodeForDeclaredError(error, "horo.character", CharacterDescriptors);
        const auto severity = DiagnosticSeverityForError(error.severity);
        if (!ValidateRecordInput(error, context, code, severity))
            return Result<CharacterDiagnosticRecord>::Failure(
                MakeError(CharacterErrors::DescriptorInvalid, "Character diagnostic evidence is malformed or exceeds its bounds."));
        if (!ValidateOrderedMetadata(context.metadata))
            return Result<CharacterDiagnosticRecord>::Failure(
                MakeError(CharacterErrors::DescriptorInvalid, "Character diagnostic metadata is unordered or malformed."));

        const PhysicsCauseProjection physicsCause = OriginatingPhysicsCode(error);
        if (!physicsCause.valid)
            return Result<CharacterDiagnosticRecord>::Failure(
                MakeError(CharacterErrors::DescriptorInvalid, "Character diagnostic cause evidence is unknown or exceeds its bound."));

        CharacterDiagnosticRecord record;
        record.category = category;
        record.controller = context.controller;
        record.sceneGeneration = context.sceneGeneration;
        record.simulationTick = context.simulationTick;
        record.code = *code;
        record.originatingPhysicsCode = physicsCause.code;
        record.severity = *severity;
        record.message = error.message;
        record.metadataCount = static_cast<std::uint8_t>(context.metadata.size());
        for (std::size_t index = 0; index < context.metadata.size(); ++index)
            record.metadata[index] = context.metadata[index];
        return Result<CharacterDiagnosticRecord>::Success(std::move(record));
    }
}  // namespace Horo::Character
