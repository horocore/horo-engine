#pragma once

/**
 * @file CookedPrefab.h
 * @brief Immutable, bounded, backend-neutral runtime prefab template and portable byte schema.
 */

#include "Horo/Assets/AssetDependency.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Prefab/PrefabIdentity.h"

#include <array>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Prefab {
    /** @brief Runtime format version, independent from project source migration versions. */
    inline constexpr std::uint32_t CurrentCookedPrefabVersion = 2;
    /** @brief Fixed HPFB envelope bytes: magic, version, identity, object count, payload size and SHA-256. */
    inline constexpr std::size_t CookedPrefabHeaderBytes = 64;

    /** @brief Dense template-local entity slot; never a Scene Runtime handle. */
    struct CookedPrefabEntitySlot final {
        std::uint32_t value{};
        [[nodiscard]] auto operator<=>(const CookedPrefabEntitySlot &) const noexcept = default;
    };

    /** @brief Dense member occurrence in one entity's member table. */
    struct CookedPrefabMemberSlot final {
        CookedPrefabEntitySlot entity;
        std::uint32_t member{};
        [[nodiscard]] auto operator<=>(const CookedPrefabMemberSlot &) const noexcept = default;
    };

    /** @brief Stable initialization interface identity, distinct from authoring properties and binding slots. */
    struct PrefabInitializationId final {
        std::uint64_t value{};
        [[nodiscard]] auto operator<=>(const PrefabInitializationId &) const noexcept = default;
    };

    /** @brief Closed value kinds; values cannot contain entity references or structural operations. */
    enum class PrefabInitializationKind : std::uint8_t {
        Boolean = 1,
        Integer,
        Number,
        String,
        Vec2,
        Vec3,
        Quaternion
    };

    /** @brief Cook-owned mapping to one existing behavior field occurrence, never a caller-supplied property path.
     * Optional omission retains the immutable template value. Numeric limits are inclusive for Integer/Number;
     * strings are limited to 256 bytes. Declarations are sorted by identity and cannot alias one field.
     */
    struct CookedPrefabInitialization final {
        PrefabInitializationId id;
        CookedPrefabMemberSlot owner;
        std::uint32_t field{};
        PrefabInitializationKind kind{PrefabInitializationKind::Boolean};
        bool required{true};
        double minimum{-1.0e18};
        double maximum{1.0e18};
        [[nodiscard]] bool operator==(const CookedPrefabInitialization &) const noexcept = default;
    };

    /** @brief One copied initialization value addressed solely through a declared runtime interface. */
    struct PrefabInitializationValue final {
        PrefabInitializationId id;
        Gameplay::BehaviorFieldValue value;
    };

    /** @brief Maximum declarations and supplied values per complete runtime template/spawn. */
    inline constexpr std::size_t MaximumPrefabInitializationValues = 64;

    /** @brief Validates one value against an exact declaration without mutation.
     * @param declaration Immutable typed interface and bounds.
     * @param value Copied value with no source/structural authority.
     * @return Success or typed initialization admission failure.
     */
    [[nodiscard]] Result<void> ValidatePrefabInitializationValue(const CookedPrefabInitialization &declaration,
                                                                 const Gameplay::BehaviorFieldValue &value);

    /** @brief Dense entry in the complete typed runtime dependency table. */
    struct CookedPrefabAssetSlot final {
        std::uint32_t value{};
        [[nodiscard]] auto operator<=>(const CookedPrefabAssetSlot &) const noexcept = default;
    };

    /** @brief Declared external interface, resolved transactionally by the spawn caller. */
    struct CookedPrefabBindingSlot final {
        std::uint32_t value{};
        [[nodiscard]] auto operator<=>(const CookedPrefabBindingSlot &) const noexcept = default;
    };

    /** @brief Closed portable reference classes; no scene handles, pointers, paths or queries. */
    using CookedPrefabReferenceTarget =
        std::variant<CookedPrefabEntitySlot, CookedPrefabMemberSlot, CookedPrefabAssetSlot, CookedPrefabBindingSlot>;

    /** @brief Typed reference fixup applied only after the complete spawned group is reserved. */
    struct CookedPrefabReference final {
        CookedPrefabMemberSlot owner;       /**< Exact component/behavior occurrence receiving the reference. */
        PrefabPropertyId property;          /**< Stable provider-owned reference property, independent from offsets. */
        CookedPrefabReferenceTarget target; /**< Dense target or explicit external binding interface. */
        [[nodiscard]] bool operator==(const CookedPrefabReference &) const noexcept = default;
    };

    /** @brief Stable binding declaration; an empty component type requests an entity relationship. */
    struct CookedPrefabBindingDeclaration final {
        PrefabPropertyId id;                                    /**< Stable non-zero interface identity. */
        std::optional<Gameplay::ComponentTypeId> componentType; /**< Present only for a typed component relationship. */
        bool required{true};                                    /**< Missing required binding rejects spawn before publication. */
        [[nodiscard]] bool operator==(const CookedPrefabBindingDeclaration &) const noexcept = default;
    };

    /** @brief Diagnostic source evidence, with no filesystem or source-loading authority. */
    struct CookedPrefabProvenance final {
        Assets::AssetId sourceAsset;
        PrefabObjectAddress sourceObject;
        Sha256Digest sourceDigest;
        [[nodiscard]] bool operator==(const CookedPrefabProvenance &) const noexcept = default;
    };

    /** @brief Portable member envelope; native provider implementations are admitted separately at spawn. */
    using CookedPrefabMember = std::variant<RawComponentPayload, Gameplay::BehaviorComponent>;

    /** @brief One fully flattened entity, in parent-before-child order. */
    struct CookedPrefabEntity final {
        std::optional<CookedPrefabEntitySlot> parent; /**< Empty only for slot zero. */
        Math::Transform localTransform;
        CookedPrefabProvenance provenance;
        std::vector<CookedPrefabMember> members; /**< Dense occurrence order retained by reference fixups. */
        [[nodiscard]] bool operator==(const CookedPrefabEntity &) const noexcept = default;
    };

    /** @brief One required runtime asset and the exact cooked artifact revision captured by the cooker. */
    struct CookedPrefabDependency final {
        Assets::AssetDependency asset;
        Sha256Digest artifactDigest;
        [[nodiscard]] bool operator==(const CookedPrefabDependency &) const noexcept = default;
    };

    /** @brief Detached mutable construction candidate; Create publishes only complete validated values. */
    struct CookedPrefabData final {
        Assets::AssetId assetId;
        std::vector<CookedPrefabEntity> entities;
        std::vector<CookedPrefabDependency> dependencies;       /**< Strict ascending unique AssetId order. */
        std::vector<CookedPrefabBindingDeclaration> bindings;   /**< Strict ascending unique interface ID order. */
        std::vector<CookedPrefabReference> references;          /**< Strict ascending owner/property order. */
        std::vector<CookedPrefabInitialization> initialization; /**< Strict ascending unique runtime interface IDs. */
        [[nodiscard]] bool operator==(const CookedPrefabData &) const noexcept = default;
    };

    /**
     * @brief Owned immutable runtime template, with canonical bytes and verified cooked-byte integrity.
     * @details Construction/decoding is load-time work. No source resolver, registry lookup, scene mutation,
     * callbacks, I/O or backend activation occurs. Views borrow this value until it is moved or destroyed.
     */
    class CookedPrefab final {
    public:
        CookedPrefab(const CookedPrefab &) = default;
        CookedPrefab(CookedPrefab &&) noexcept = default;
        CookedPrefab &operator=(const CookedPrefab &) = delete;
        CookedPrefab &operator=(CookedPrefab &&) = delete;

        /**
         * @brief Validates and canonically encodes one detached runtime template.
         * @param candidate Complete flattened entity/member/reference/dependency data.
         * @param limits Captured immutable project ceilings, including encoded cooked payload bytes.
         * @return Complete immutable artifact or a typed identity, structure or bounds failure.
         * @throws std::bad_alloc if owned storage allocation fails; no artifact is published.
         */
        [[nodiscard]] static Result<CookedPrefab> Create(CookedPrefabData candidate, const PrefabLimitProfile &limits);

        /**
         * @brief Verifies an HPFB envelope and SHA-256 before bounded decoding and structural validation.
         * @param bytes Exact artifact bytes; trailing data, unknown tags and unsupported versions are rejected.
         * @param expectedAsset Requested template identity from the caller's captured catalog.
         * @param limits Captured immutable project ceilings checked before allocation.
         * @return Owned complete template, UnsupportedCookedVersion, CorruptedPayload or a typed bounds failure.
         * @throws std::bad_alloc if owned storage allocation fails; caller bytes remain unchanged.
         */
        [[nodiscard]] static Result<CookedPrefab> Parse(std::span<const std::byte> bytes, const Assets::AssetId &expectedAsset,
                                                        const PrefabLimitProfile &limits);

        /** @brief Returns the exact template identity. @return Path-independent AssetId. */
        [[nodiscard]] const Assets::AssetId &GetAssetId() const noexcept;
        /** @brief Returns the root-inclusive entity count. @return Bounded dense slot count. */
        [[nodiscard]] std::uint32_t GetObjectCount() const noexcept;
        /** @brief Returns all immutable typed tables. @return Borrowed owned data. */
        [[nodiscard]] const CookedPrefabData &Data() const noexcept;
        /** @brief Returns the exact canonical HPFB artifact. @return Borrowed owned bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
        /** @brief Returns SHA-256 of the actual encoded payload. @return Cooked integrity digest, separate from source evidence. */
        [[nodiscard]] Sha256Digest PayloadDigest() const noexcept;

    private:
        /** @brief Owns complete validated typed data and its exact canonical encoding. */
        CookedPrefab(CookedPrefabData data, std::vector<std::byte> bytes, const Sha256Digest &digest) noexcept;
        CookedPrefabData data_;
        std::vector<std::byte> bytes_;
        Sha256Digest digest_;
    };
}  // namespace Horo::Prefab
