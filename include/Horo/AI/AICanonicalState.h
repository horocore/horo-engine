#pragma once

/** @file AICanonicalState.h
 * @brief AI-owned value snapshots and explicit blackboard schema migration contracts.
 */
#include "Horo/AI/AISceneComponents.h"
#include "Horo/AI/BlackboardSchema.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::AI {
    /** @brief Current internal canonical state contract; independent of save containers and decision plans. */
    inline constexpr std::uint32_t CurrentAiCanonicalStateVersion = 1;
    /** @brief Maximum explicitly composed schema migrations per restore. */
    inline constexpr std::size_t MaximumAiSchemaMigrations = 128;

    /** @brief One stable schema-keyed owned value; absence is preserved for optional keys. */
    struct BlackboardCanonicalEntry final {
        BlackboardKeyId key;
        std::optional<BlackboardValue> value;
        [[nodiscard]] bool operator==(const BlackboardCanonicalEntry &) const noexcept = default;
    };

    /** @brief The only canonical blackboard snapshot; no observers, runtime bindings, or leases. */
    struct BlackboardCanonicalState final {
        BlackboardSchemaId schema;
        std::uint32_t schemaVersion{};
        std::vector<BlackboardCanonicalEntry> entries; /**< Complete key-sorted schema layout on capture. */
        [[nodiscard]] bool operator==(const BlackboardCanonicalState &) const noexcept = default;
    };

    /** @brief Base controller state shared by save, reload and reconstruction; no executing decision-task state. */
    struct AiBaseControllerCanonicalState final {
        AiControllerComponent authored;
        std::optional<BlackboardCanonicalState> blackboard; /**< Present exactly when the agent is active. */
        [[nodiscard]] bool operator==(const AiBaseControllerCanonicalState &) const noexcept = default;
    };

    /** @brief Persistable agent lifecycle, distinct from task execution status. */
    enum class AiCanonicalAgentState : std::uint8_t {
        Active,
        Disabled,
        Count
    };

    /** @brief The only canonical base-agent snapshot; identity comes from the authored AgentId. */
    struct AiAgentCanonicalState final {
        AiAgentComponent authored;
        AiCanonicalAgentState state{AiCanonicalAgentState::Disabled};
        std::optional<AiBaseControllerCanonicalState> controller;
        [[nodiscard]] bool operator==(const AiAgentCanonicalState &) const noexcept = default;
    };

    /** @brief Detached complete AI population; external integrations own their existing container encoding. */
    struct AiCanonicalState final {
        std::uint32_t version{CurrentAiCanonicalStateVersion};
        std::vector<AiAgentCanonicalState> agents; /**< Stable AgentId order on capture. */
        [[nodiscard]] bool operator==(const AiCanonicalState &) const noexcept = default;
    };

    /** @brief Explicit stable key rename/removal; an invalid destination deliberately discards the source. */
    struct BlackboardKeyMigration final {
        BlackboardKeyId source;
        BlackboardKeyId destination;
    };

    /** @brief Inert direct forward migration to one exact target schema; never activates a decision plan. */
    struct BlackboardSchemaMigration final {
        std::shared_ptr<const BlackboardSchema> source;
        BlackboardSchemaId destination;
        std::uint32_t destinationVersion{};
        std::span<const BlackboardKeyMigration> keys; /**< Borrowed only during staging; unspecified keys retain identity. */
    };

    /**
     * @brief Validates a complete canonical layout against one admitted immutable schema.
     * @param state Detached candidate including every schema key exactly once.
     * @param schema Admitted source or destination schema.
     * @return Typed malformed, unsupported schema, presence, or value diagnostic.
     * @post Neither input changes.
     */
    [[nodiscard]] Result<void> ValidateCanonicalBlackboard(const BlackboardCanonicalState &state, const BlackboardSchema &schema);

    /**
     * @brief Migrates complete source values into target defaults without modifying either schema or source.
     * @param state Canonical source state.
     * @param destination Admitted destination schema.
     * @param migrations Explicit bounded direct forward migration catalog.
     * @return Complete destination state or a typed compatibility/migration diagnostic.
     * @details Same-version layouts require exact keys. Version changes require one unambiguous source schema and mapping;
     * added keys use admitted defaults, renamed keys retain validated values, and removals require explicit discard.
     */
    [[nodiscard]] Result<BlackboardCanonicalState> MigrateCanonicalBlackboard(const BlackboardCanonicalState &state,
                                                                              const BlackboardSchema &destination,
                                                                              std::span<const BlackboardSchemaMigration> migrations);
}  // namespace Horo::AI
