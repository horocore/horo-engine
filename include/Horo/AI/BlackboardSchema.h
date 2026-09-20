#pragma once

/**
 * @file BlackboardSchema.h
 * @brief Immutable typed gameplay-AI blackboard schema and bounded stored-value contracts.
 */

#include "Horo/AI/AIIdentity.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/WorldCoordinate64.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace Horo::AI {
    /** @brief Maximum keys admitted by one immutable blackboard schema. */
    inline constexpr std::size_t MaximumBlackboardKeys = 128;
    /** @brief Maximum scalar elements stored by one blackboard collection value. */
    inline constexpr std::size_t MaximumBlackboardCollectionElements = 16;
    /** @brief Maximum canonical bytes preserved for one unavailable unknown value type. */
    inline constexpr std::size_t MaximumBlackboardOpaqueBytes = 128;

    /** @brief Canonical fixed-width network-byte-order entity projection encoding. */
    using SerializedBlackboardEntityReference = std::array<std::uint8_t, 16>;

    /** @brief Supported non-recursive element kinds in the schema-1 blackboard value model. */
    enum class BlackboardValueKind : std::uint8_t {
        Boolean,
        SignedInteger,
        Scalar,
        EntityReference,
        AssetReference,
        WorldCoordinate,
        Count, /**< Non-serializable upper bound for schema validation. */
    };

    /** @brief Whether a key stores one scalar or a bounded flat collection of scalars. */
    enum class BlackboardValueCardinality : std::uint8_t {
        Scalar,
        Collection,
        Count, /**< Non-serializable upper bound for schema validation. */
    };

    /** @brief Access granted to decision and behavior consumers after schema admission. */
    enum class BlackboardKeyAccess : std::uint8_t {
        ReadOnly,
        ReadWrite,
        Count, /**< Non-serializable upper bound for schema validation. */
    };

    /** @brief Presence policy applied when an instance is materialized from a schema. */
    enum class BlackboardKeyPresence : std::uint8_t {
        Required,
        Optional,
        Count, /**< Non-serializable upper bound for schema validation. */
    };

    /** @brief Version policy for canonical bytes whose key or value type is unavailable locally. */
    enum class BlackboardUnknownValuePolicy : std::uint8_t {
        Reject,
        PreserveOpaque,
        Count, /**< Non-serializable upper bound for schema validation. */
    };

    /**
     * @brief Dependency-neutral stored projection of a generation-checked scene entity reference.
     *
     * This is blackboard storage, not a second canonical runtime entity identity. A RuntimeScene
     * adapter owns conversion and must revalidate incarnation, generation, and residency.
     */
    struct BlackboardStoredEntityReference final {
        std::uint64_t sceneIncarnation{};                              /**< Serialized scene-incarnation identity. */
        std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()}; /**< Serialized entity slot. */
        std::uint32_t generation{};                                    /**< Non-zero serialized slot generation. */

        /** @brief Checks stored representation only. @return True when all reserved values are absent. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return sceneIncarnation != 0 && slot != std::numeric_limits<std::uint32_t>::max() && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const BlackboardStoredEntityReference &) const noexcept = default;
    };

    /**
     * @brief Dependency-neutral stored projection of one canonical 128-bit asset identity.
     *
     * This byte carrier is not an alternate AssetId authority. An Assets adapter owns exact
     * conversion and must revalidate asset type and availability before use.
     */
    struct BlackboardStoredAssetReference final {
        std::array<std::uint8_t, 16> canonicalBytes{}; /**< Exact canonical asset-identity bytes. */

        /** @brief Checks the reserved all-zero representation. @return True for a non-zero identity. */
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] constexpr auto operator<=>(const BlackboardStoredAssetReference &) const noexcept = default;
    };

    /**
     * @brief Encodes a well-formed stored entity projection without serializing ABI padding.
     * @param reference Stored scene incarnation, entity slot, and generation.
     * @return Exact 16-byte network-order encoding or AIErrors::BlackboardValueInvalid.
     */
    [[nodiscard]] Result<SerializedBlackboardEntityReference> SerializeBlackboardEntityReference(
        const BlackboardStoredEntityReference &reference);

    /**
     * @brief Decodes and validates one exact stored entity projection.
     * @param bytes Exact 16-byte network-order encoding.
     * @return Stored projection or AIErrors::BlackboardValueInvalid.
     */
    [[nodiscard]] Result<BlackboardStoredEntityReference> DeserializeBlackboardEntityReference(
        const SerializedBlackboardEntityReference &bytes);

    /** @brief One schema-1 scalar value; collections contain only this non-recursive variant. */
    using BlackboardScalarValue =
        std::variant<bool, std::int64_t, double, BlackboardStoredEntityReference, BlackboardStoredAssetReference, Math::WorldCoordinate64>;

    /** @brief Fixed-capacity homogeneous collection with no hidden allocation or recursive ownership. */
    struct BlackboardCollectionValue final {
        BlackboardValueKind elementKind{BlackboardValueKind::Boolean};                     /**< Declared kind of every active element. */
        std::array<BlackboardScalarValue, MaximumBlackboardCollectionElements> elements{}; /**< Inline owned storage. */
        std::size_t count{};                                                               /**< Active prefix length. */

        /** @brief Compares kind and active element prefix only. @return Whether the logical collection values match. */
        [[nodiscard]] bool operator==(const BlackboardCollectionValue &other) const noexcept;
    };

    /** @brief Bounded canonical bytes retained only when an unavailable type is explicitly preservable. */
    struct BlackboardOpaqueValue final {
        std::uint32_t schemaVersion{};                               /**< Non-zero source schema version. */
        std::uint32_t serializedType{};                              /**< Non-zero external type discriminant. */
        std::array<std::byte, MaximumBlackboardOpaqueBytes> bytes{}; /**< Byte-for-byte owned canonical payload. */
        std::size_t size{};                                          /**< Active payload byte count. */

        /** @brief Compares metadata and active payload prefix only. @return Whether the logical opaque values match. */
        [[nodiscard]] bool operator==(const BlackboardOpaqueValue &other) const noexcept;
    };

    /** @brief Owned blackboard value or preserved unavailable canonical payload. */
    using BlackboardValue = std::variant<BlackboardScalarValue, BlackboardCollectionValue, BlackboardOpaqueValue>;

    /** @brief Authored descriptor for one stable typed blackboard key. */
    struct BlackboardKeyDescriptor final {
        BlackboardKeyId key;                                    /**< Stable identity independent of display text or array position. */
        BlackboardValueKind kind{BlackboardValueKind::Boolean}; /**< Required scalar or collection element kind. */
        BlackboardValueCardinality cardinality{BlackboardValueCardinality::Scalar}; /**< Scalar or bounded collection. */
        std::size_t maximumCollectionElements{1};                                   /**< Collection limit; exactly one for scalar keys. */
        BlackboardKeyPresence presence{BlackboardKeyPresence::Required};            /**< Instance presence policy. */
        BlackboardKeyAccess access{BlackboardKeyAccess::ReadWrite};                 /**< Consumer mutation policy. */
        std::optional<BlackboardValue> defaultValue;                                /**< Optional type-checked owned default. */

        [[nodiscard]] bool operator==(const BlackboardKeyDescriptor &) const noexcept = default;
    };

    /** @brief Borrowed source used to transactionally capture one immutable schema value. */
    struct BlackboardSchemaDescriptor final {
        BlackboardSchemaId identity;                                                           /**< Stable schema identity. */
        std::uint32_t version{1};                                                              /**< Non-zero serialized schema version. */
        BlackboardUnknownValuePolicy unknownValuePolicy{BlackboardUnknownValuePolicy::Reject}; /**< Version skew policy. */
        std::span<const BlackboardKeyDescriptor> keys;                                         /**< Borrowed unsorted key definitions. */
    };

    /** @brief Immutable sorted owned schema admitted before blackboard instance allocation. */
    class BlackboardSchema final {
    public:
        BlackboardSchema() = delete;

        /**
         * @brief Validates and copies a borrowed descriptor without aliasing its storage.
         * @param descriptor Candidate identity, version, policy, keys, and defaults.
         * @return Owned key-sorted schema or a typed AIErrors validation failure.
         * @post Failure leaves descriptor and all input storage unchanged.
         */
        [[nodiscard]] static Result<BlackboardSchema> Capture(const BlackboardSchemaDescriptor &descriptor);

        /** @brief Returns the stable schema identity. @return Non-zero authored identity. */
        [[nodiscard]] constexpr BlackboardSchemaId Identity() const noexcept {
            return identity_;
        }

        /** @brief Returns the serialized schema version. @return Non-zero version. */
        [[nodiscard]] constexpr std::uint32_t Version() const noexcept {
            return version_;
        }

        /** @brief Returns the unavailable-value policy. @return Captured policy. */
        [[nodiscard]] constexpr BlackboardUnknownValuePolicy UnknownValuePolicy() const noexcept {
            return unknownValuePolicy_;
        }

        /**
         * @brief Borrows stable key-sorted descriptors owned by this value.
         * @return View valid while the owning BlackboardSchema remains alive; empty for a moved-from value.
         */
        [[nodiscard]] std::span<const BlackboardKeyDescriptor> Keys() const noexcept;

        BlackboardSchema(const BlackboardSchema &) = delete;
        BlackboardSchema(BlackboardSchema &&) noexcept = default;
        BlackboardSchema &operator=(const BlackboardSchema &) = delete;
        BlackboardSchema &operator=(BlackboardSchema &&) = delete;

    private:
        BlackboardSchema(BlackboardSchemaId identity, std::uint32_t version, BlackboardUnknownValuePolicy policy,
                         std::unique_ptr<std::array<BlackboardKeyDescriptor, MaximumBlackboardKeys>> keys, std::size_t keyCount) noexcept;

        BlackboardSchemaId identity_;
        std::uint32_t version_{};
        BlackboardUnknownValuePolicy unknownValuePolicy_{BlackboardUnknownValuePolicy::Reject};
        std::unique_ptr<std::array<BlackboardKeyDescriptor, MaximumBlackboardKeys>> keys_;
        std::size_t keyCount_{};
    };

    /**
     * @brief Validates one value against a known key or the schema's unavailable-value policy.
     * @param value Candidate owned value.
     * @param key Known key descriptor, or null when the key/type is unavailable locally.
     * @param unknownPolicy Policy used only when key is empty.
     * @return Success or a typed AIErrors validation failure.
     * @post Validation never mutates value or key.
     */
    [[nodiscard]] Result<void> ValidateBlackboardValue(const BlackboardValue &value, const BlackboardKeyDescriptor *key,
                                                       BlackboardUnknownValuePolicy unknownPolicy);
}  // namespace Horo::AI
