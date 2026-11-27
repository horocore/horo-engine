#pragma once

/**
 * @file AIIdentity.h
 * @brief Persistent gameplay-AI identities and scene-incarnation-safe runtime handles.
 */

#include "Horo/AI/AIErrors.h"
#include "Horo/Foundation/Handles.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/StrongId.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::AI {
    /** @brief Canonical fixed-width network-byte-order representation of one persistent AI identity. */
    using SerializedAiIdentity = std::array<std::uint8_t, sizeof(std::uint64_t)>;

    /** @brief Strong persistent non-zero gameplay-AI identity in one tag-defined domain. */
    template <typename Tag> using AiStableIdentity = Foundation::Detail::NonZeroId64<Tag, AIErrors::IdentityInvalid>;

    struct AgentIdentityTag;
    struct ControllerTypeIdentityTag;
    struct TaskIdentityTag;
    struct BlackboardSchemaIdentityTag;
    struct BlackboardKeyIdentityTag;
    struct SenseTypeIdentityTag;
    struct StimulusTypeIdentityTag;
    struct PerceptionListenerTypeIdentityTag;
    struct PerceptionProviderIdentityTag;
    struct QueryIdentityTag;
    struct QueryItemTypeIdentityTag;
    struct QueryResultSchemaIdentityTag;

    /** @brief Persistent authored identity of an AI agent, independent of display name and runtime instance. */
    using AgentId = AiStableIdentity<AgentIdentityTag>;
    /** @brief Persistent identity of an authored controller type. */
    using ControllerTypeId = AiStableIdentity<ControllerTypeIdentityTag>;
    /** @brief Persistent authored identity of a task definition. */
    using TaskId = AiStableIdentity<TaskIdentityTag>;
    /** @brief Persistent identity of a blackboard schema. */
    using BlackboardSchemaId = AiStableIdentity<BlackboardSchemaIdentityTag>;
    /** @brief Persistent identity of a key within its authored blackboard identity domain. */
    using BlackboardKeyId = AiStableIdentity<BlackboardKeyIdentityTag>;
    /** @brief Persistent identity of a perception sense type, independent of display name. */
    using SenseTypeId = AiStableIdentity<SenseTypeIdentityTag>;
    /** @brief Persistent identity of a typed perception stimulus payload. */
    using StimulusTypeId = AiStableIdentity<StimulusTypeIdentityTag>;
    /** @brief Persistent identity of a listener descriptor type. */
    using PerceptionListenerTypeId = AiStableIdentity<PerceptionListenerTypeIdentityTag>;
    /** @brief Persistent identity of a native, script, or package descriptor provider. */
    using PerceptionProviderId = AiStableIdentity<PerceptionProviderIdentityTag>;
    /** @brief Stable authored EQS asset identity; AssetRegistry binds it to an AssetId outside HoroAI. */
    using QueryId = AiStableIdentity<QueryIdentityTag>;
    /** @brief Stable item payload type identity. */
    using QueryItemTypeId = AiStableIdentity<QueryItemTypeIdentityTag>;
    /** @brief Stable identity of an authored query result schema. */
    using QueryResultSchemaId = AiStableIdentity<QueryResultSchemaIdentityTag>;

    /**
     * @brief Encodes a persistent AI identity in canonical network byte order.
     * @param identity Persistent identity to encode.
     * @return Exact eight-byte representation; the invalid identity encodes as all zeroes.
     */
    template <typename Tag>
    [[nodiscard]] constexpr SerializedAiIdentity SerializeAiIdentity(const AiStableIdentity<Tag> identity) noexcept {
        SerializedAiIdentity bytes{};
        std::uint64_t remaining = identity.Value();
        for (auto iterator = bytes.rbegin(); iterator != bytes.rend(); ++iterator) {
            *iterator = static_cast<std::uint8_t>(remaining % 256U);
            remaining /= 256U;
        }
        return bytes;
    }

    /**
     * @brief Decodes one exact persistent AI identity from canonical network byte order.
     * @param bytes Fixed-width bytes to decode.
     * @return Exact strong identity or AIErrors::IdentityInvalid for the reserved all-zero encoding.
     */
    template <typename Tag> [[nodiscard]] Result<AiStableIdentity<Tag>> DeserializeAiIdentity(const SerializedAiIdentity &bytes) {
        std::uint64_t value{};
        for (const std::uint8_t byte : bytes)
            value = value * 256U + byte;
        return AiStableIdentity<Tag>::Create(value);
    }

    /**
     * @brief Requires duplication to use a distinct owner-issued persistent identity.
     * @param source Existing identity being duplicated.
     * @param duplicate Newly issued identity for the copy.
     * @return Success, AIErrors::IdentityInvalid, or AIErrors::DescriptorConflict.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateAiIdentityDuplication(const AiStableIdentity<Tag> source, const AiStableIdentity<Tag> duplicate) {
        if (!source.IsValid() || !duplicate.IsValid())
            return Result<void>::Failure(MakeError(AIErrors::IdentityInvalid));
        if (source == duplicate)
            return Result<void>::Failure(MakeError(AIErrors::DescriptorConflict));
        return Result<void>::Success();
    }

    /** @brief Maximum total persistent identities validated by one descriptor-set operation. */
    inline constexpr std::size_t MaximumAiIdentityDescriptors = 4'096;

    /** @brief Borrowed persistent identity domains validated together before runtime activation. */
    struct AiIdentityDescriptorSet {
        std::span<const AgentId> agents;                           /**< Authored agent identities. */
        std::span<const ControllerTypeId> controllerTypes;         /**< Controller type identities. */
        std::span<const TaskId> tasks;                             /**< Authored task identities. */
        std::span<const BlackboardSchemaId> blackboardSchemas;     /**< Blackboard schema identities. */
        std::span<const BlackboardKeyId> blackboardKeys;           /**< Blackboard key identities. */
        std::span<const SenseTypeId> senseTypes;                   /**< Perception sense type identities. */
        std::span<const StimulusTypeId> stimulusTypes;             /**< Perception stimulus type identities. */
        std::span<const PerceptionListenerTypeId> listenerTypes;   /**< Perception listener type identities. */
        std::span<const PerceptionProviderId> perceptionProviders; /**< Perception descriptor provider identities. */
    };

    /**
     * @brief Rejects invalid, duplicate, or over-limit identities before activation.
     *
     * Uniqueness is checked independently within each identity domain, so equal numeric
     * values in different strong domains do not conflict. Input order does not affect the result.
     *
     * @param descriptors Borrowed descriptor identity domains.
     * @return Success, AIErrors::IdentityInvalid, AIErrors::DescriptorConflict, or AIErrors::DescriptorLimitExceeded.
     */
    [[nodiscard]] Result<void> ValidateAiIdentityDescriptorSet(const AiIdentityDescriptorSet &descriptors);

    /** @brief Non-zero host-issued identity of one active SceneRuntime incarnation; never serialized. */
    class AiRuntimeIncarnation final {
    public:
        /** @brief Constructs the reserved invalid incarnation. */
        AiRuntimeIncarnation() = default;

        /**
         * @brief Validates a host-issued scene-runtime incarnation.
         * @param value Non-zero process-local value.
         * @return Exact incarnation or AIErrors::HandleInvalid.
         */
        [[nodiscard]] static Result<AiRuntimeIncarnation> Create(std::uint64_t value);

        /** @brief Returns the process-local value. @return Zero only for the invalid incarnation. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation. @return Whether the incarnation is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        constexpr auto operator<=>(const AiRuntimeIncarnation &) const noexcept = default;

    private:
        explicit constexpr AiRuntimeIncarnation(const std::uint64_t value) noexcept : value_(value) {}

        std::uint64_t value_{};
    };

    /** @brief Scene-incarnation-owned process-local handle that must never be serialized. */
    template <typename Tag> struct AiRuntimeHandle final {
        AiRuntimeIncarnation incarnation; /**< Exact SceneRuntime incarnation that issued the handle. */
        Horo::Handle<Tag> slot;           /**< Runtime slot and non-zero generation. */

        /** @brief Checks representation, not current slot residency. @return True for a well-formed handle. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return incarnation.IsValid() && slot.index != decltype(slot)::InvalidIndex && slot.generation != 0;
        }

        constexpr auto operator<=>(const AiRuntimeHandle &) const noexcept = default;
    };

    struct AgentHandleTag;
    struct TaskHandleTag;

    /** @brief Process-local identity of one agent instance in an exact SceneRuntime incarnation. */
    using AgentHandle = AiRuntimeHandle<AgentHandleTag>;
    /** @brief Process-local identity of one executing task instance in an exact SceneRuntime incarnation. */
    using TaskHandle = AiRuntimeHandle<TaskHandleTag>;

    /**
     * @brief Rejects malformed or foreign AI runtime handles before registry access.
     * @param handle Handle submitted to the owning scene runtime.
     * @param expectedIncarnation Exact active SceneRuntime incarnation.
     * @return Success or AIErrors::HandleInvalid.
     * @post Success does not prove residency; the registry must compare the current slot generation.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateAiRuntimeHandle(const AiRuntimeHandle<Tag> &handle, const AiRuntimeIncarnation expectedIncarnation) {
        if (!expectedIncarnation.IsValid() || !handle.IsValid() || handle.incarnation != expectedIncarnation)
            return Result<void>::Failure(MakeError(AIErrors::HandleInvalid));
        return Result<void>::Success();
    }

    /**
     * @brief Advances a non-zero runtime slot generation without permitting wraparound.
     * @param currentGeneration Currently issued non-zero slot generation.
     * @return Next generation, AIErrors::HandleInvalid for zero, or AIErrors::GenerationExhausted at the maximum.
     */
    [[nodiscard]] Result<std::uint32_t> AdvanceAiRuntimeGeneration(std::uint32_t currentGeneration);
}  // namespace Horo::AI
