#pragma once

/**
 * @file CharacterDiagnostics.h
 * @brief Bounded backend-neutral Character diagnostic records and rate limiting.
 */

#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/PhysicsIdentity.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace Horo::Character {
    /** @brief Maximum optional metadata entries retained by one Character diagnostic. */
    inline constexpr std::size_t MaximumCharacterDiagnosticMetadataEntries = 4;
    /** @brief Maximum UTF-8 bytes retained by one Character diagnostic message. */
    inline constexpr std::size_t MaximumCharacterDiagnosticMessageBytes = 512;
    /** @brief Maximum typed error nodes inspected while preserving an originating Physics failure. */
    inline constexpr std::size_t MaximumCharacterDiagnosticCauseDepth = 8;

    /** @brief Stable Character failure area used for filtering and presentation. */
    enum class CharacterDiagnosticCategory : std::uint8_t {
        Command,
        Lifecycle,
        Solver,
        Query,
        Capacity,
    };

    /** @brief Closed optional metadata vocabulary; controller, scene and tick are mandatory record fields. */
    enum class CharacterDiagnosticMetadataKey : std::uint8_t {
        PhysicsWorld,
        OperationSequence,
        RequestedCount,
        Capacity,
    };

    /** @brief Owned scalar or stable Horo identity admitted as optional Character diagnostic metadata. */
    using CharacterDiagnosticMetadataValue = std::variant<std::uint64_t, Physics::PhysicsWorldId>;

    /** @brief One typed metadata field; inputs require strictly increasing key order. */
    struct CharacterDiagnosticMetadataEntry final {
        CharacterDiagnosticMetadataKey key{CharacterDiagnosticMetadataKey::PhysicsWorld};
        CharacterDiagnosticMetadataValue value;
    };

    /** @brief Mandatory identity and timing evidence for one Character diagnostic. */
    struct CharacterDiagnosticContext final {
        CharacterControllerHandle controller;
        std::uint64_t sceneGeneration{};
        std::uint64_t simulationTick{};
        std::span<const CharacterDiagnosticMetadataEntry> metadata{};
    };

    /** @brief Owned inert Character failure evidence with an optional exact originating Physics code. */
    struct CharacterDiagnosticRecord final {
        std::uint32_t schemaVersion{1};
        CharacterDiagnosticCategory category{CharacterDiagnosticCategory::Command};
        CharacterControllerHandle controller;
        std::uint64_t sceneGeneration{};
        std::uint64_t simulationTick{};
        DiagnosticCode code;
        std::optional<DiagnosticCode> originatingPhysicsCode;
        DiagnosticSeverity severity{DiagnosticSeverity::Error};
        std::array<char, MaximumCharacterDiagnosticMessageBytes> message;
        std::uint16_t messageLength{};
        std::array<CharacterDiagnosticMetadataEntry, MaximumCharacterDiagnosticMetadataEntries> metadata;
        std::uint8_t metadataCount{};

        /** @brief Returns the exact owned message bytes. @return View valid for this record's lifetime. */
        [[nodiscard]] std::string_view Message() const noexcept {
            return {message.data(), messageLength};
        }
    };

    /** @brief Allocation-free per-stream diagnostic admission policy in fixed-tick time. */
    struct CharacterDiagnosticRatePolicy final {
        std::uint32_t maximumEmissionsPerTick{1};
        std::uint64_t minimumTickInterval{1};
    };

    /** @brief Result of one non-blocking diagnostic admission attempt. */
    enum class CharacterDiagnosticAdmission : std::uint8_t {
        Emit,
        SuppressedRate,
        RejectedTick,
    };

    /** @brief Cumulative allocation-free counters for one owner-managed diagnostic stream. */
    struct CharacterDiagnosticRateStatistics final {
        std::uint64_t emitted{};
        std::uint64_t suppressed{};
        std::uint64_t rejectedTicks{};
    };

    /** @brief Owner-thread rate limiter for one controller/category/code stream. */
    class CharacterDiagnosticRateLimiter final {
    public:
        /**
         * @brief Validates and captures a rate policy.
         * @param policy Positive finite emission and tick-spacing bounds.
         * @return Limiter or CharacterErrors::DescriptorInvalid.
         */
        [[nodiscard]] static Result<CharacterDiagnosticRateLimiter> Create(CharacterDiagnosticRatePolicy policy);

        /**
         * @brief Admits one tick without allocation, locking, logging or callback execution.
         * @param simulationTick Non-zero monotonically non-decreasing fixed tick.
         * @return Emit, SuppressedRate, or RejectedTick.
         */
        [[nodiscard]] CharacterDiagnosticAdmission TryAdmit(std::uint64_t simulationTick) noexcept;

        /** @brief Returns cumulative admission counters. @return Current value snapshot. */
        [[nodiscard]] CharacterDiagnosticRateStatistics Statistics() const noexcept;

    private:
        explicit CharacterDiagnosticRateLimiter(CharacterDiagnosticRatePolicy policy) noexcept;

        CharacterDiagnosticRatePolicy policy_;
        CharacterDiagnosticRateStatistics statistics_;
        std::uint64_t lastTick_{};
        std::uint64_t lastEmissionTick_{};
        std::uint32_t emissionsThisTick_{};
    };

    /**
     * @brief Returns the stable dotted presentation name for a known category.
     * @param category Closed category value.
     * @return Stable process-lifetime name, or empty for an unknown value.
     */
    [[nodiscard]] std::string_view CharacterDiagnosticCategoryName(CharacterDiagnosticCategory category) noexcept;

    /**
     * @brief Creates bounded Character evidence from a declared Character error and typed context.
     * @param category Stable Character failure area.
     * @param error Declared `horo.character` error, optionally wrapping a declared `horo.physics` cause.
     * @param context Mandatory controller/scene/tick evidence plus ordered bounded metadata.
     * @return Owned record, or a stable Character descriptor/unsupported error.
     * @pre Invoke only after the owner-managed rate limiter admits the stream; construction may allocate.
     * @post Failure publishes no record, log, callback or runtime mutation.
     */
    [[nodiscard]] Result<CharacterDiagnosticRecord> MakeCharacterDiagnosticRecord(CharacterDiagnosticCategory category, const Error &error,
                                                                                  const CharacterDiagnosticContext &context);
}  // namespace Horo::Character
