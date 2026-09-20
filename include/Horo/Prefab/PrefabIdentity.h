#pragma once

/**
 * @file PrefabIdentity.h
 * @brief Stable typed identities and addresses for prefab authoring contracts.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/StrongId.h"
#include "Horo/Gameplay/Component.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabLimits.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace Horo::Prefab {
    /** @brief Maximum nested placement segments in one persisted object address. */
    inline constexpr std::size_t MaximumPrefabObjectScopeDepth = PrefabHardLimits::NestedPrefabDepth;

    struct PrefabInstanceIdTag;
    struct PrefabComponentInstanceIdTag;
    struct PrefabPropertyIdTag;

    /** @brief Stable identity of one authored prefab occurrence in a containing document. */
    using PrefabInstanceId = Foundation::Detail::NonZeroId64<PrefabInstanceIdTag, PrefabErrors::IdentityInvalid>;
    /** @brief Stable identity of one component occurrence on a prefab-local object. */
    using PrefabComponentInstanceId = Foundation::Detail::NonZeroId64<PrefabComponentInstanceIdTag, PrefabErrors::IdentityInvalid>;
    /** @brief Stable registered identity of one prefab-addressable property. */
    using PrefabPropertyId = Foundation::Detail::NonZeroId64<PrefabPropertyIdTag, PrefabErrors::IdentityInvalid>;

    /** @brief Persisted local slot identifying one authored prefab object; zero is the root slot. */
    struct LocalObjectId final {
        std::uint32_t value{};

        /** @brief Reports whether this slot identifies the root. @return True only for slot zero. */
        [[nodiscard]] constexpr bool IsRoot() const noexcept {
            return value == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const LocalObjectId &) const noexcept = default;
    };

    /** @brief Path-independent reference to one prefab asset. */
    class PrefabAssetReference final {
    public:
        /** @brief Constructs an invalid empty reference. */
        PrefabAssetReference() = default;

        /**
         * @brief Validates an Asset Registry identity for prefab use.
         * @param assetId Stable sidecar-owned asset identity.
         * @return Reference or PrefabErrors::ReferenceInvalid.
         */
        [[nodiscard]] static Result<PrefabAssetReference> Create(const Assets::AssetId &assetId);

        /** @brief Returns the authoritative asset identity. @return Stable path-independent identity. */
        [[nodiscard]] const Assets::AssetId &Asset() const noexcept;
        /** @brief Reports whether this reference contains a valid AssetId. @return True for a usable reference. */
        [[nodiscard]] bool IsValid() const noexcept;

        [[nodiscard]] auto operator<=>(const PrefabAssetReference &) const noexcept = default;

    private:
        explicit PrefabAssetReference(Assets::AssetId assetId) noexcept : assetId_(std::move(assetId)) {}

        Assets::AssetId assetId_{};
    };

    /** @brief Stable address of an object inside a possibly nested prefab occurrence. */
    class PrefabObjectAddress final {
    public:
        /** @brief Constructs the root object in the outer source scope. */
        PrefabObjectAddress() = default;

        /**
         * @brief Copies and validates a bounded nested placement scope.
         * @param nestedInstanceScope Outer-to-inner non-root placement slots.
         * @param sourceObject Target local object in the final scope; zero denotes its root.
         * @return Stable address or PrefabErrors::AddressInvalid.
         */
        [[nodiscard]] static Result<PrefabObjectAddress> Create(std::span<const LocalObjectId> nestedInstanceScope,
                                                                LocalObjectId sourceObject);

        /** @brief Returns the immutable outer-to-inner placement scope. @return Borrowed bounded scope. */
        [[nodiscard]] std::span<const LocalObjectId> NestedInstanceScope() const noexcept;

        /** @brief Returns the target object in the final source scope. @return Stable local slot. */
        [[nodiscard]] constexpr LocalObjectId SourceObject() const noexcept {
            return sourceObject_;
        }

        [[nodiscard]] constexpr auto operator<=>(const PrefabObjectAddress &) const noexcept = default;

    private:
        PrefabObjectAddress(std::array<LocalObjectId, MaximumPrefabObjectScopeDepth> scope, std::uint8_t scopeSize,
                            LocalObjectId sourceObject) noexcept
            : scope_(scope), scopeSize_(scopeSize), sourceObject_(sourceObject) {}

        std::array<LocalObjectId, MaximumPrefabObjectScopeDepth> scope_{};
        std::uint8_t scopeSize_{};
        LocalObjectId sourceObject_{};
    };

    /** @brief Collision-free logical key for one expanded object before scene-ID hashing. */
    struct ExpandedPrefabObjectKey final {
        PrefabInstanceId instance;  /**< Stable containing scene-instance identity. */
        PrefabObjectAddress object; /**< Exact nested prefab-local target. */

        /** @brief Reports whether both identity dimensions are usable. @return True for a valid logical expansion key. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return instance.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const ExpandedPrefabObjectKey &) const noexcept = default;
    };

    /** @brief Stable component/property target within an exact prefab-local object scope. */
    class PrefabPropertyAddress final {
    public:
        /**
         * @brief Validates every identity dimension of a property target.
         * @param object Exact prefab-local object address.
         * @param componentType Registered project/component schema identity.
         * @param componentInstance Stable occurrence identity on the object.
         * @param property Stable property identity independent from names or offsets.
         * @return Address or PrefabErrors::AddressInvalid.
         */
        [[nodiscard]] static Result<PrefabPropertyAddress> Create(PrefabObjectAddress object, Gameplay::ComponentTypeId componentType,
                                                                  PrefabComponentInstanceId componentInstance, PrefabPropertyId property);

        /** @brief Returns the object target. @return Exact nested object address. */
        [[nodiscard]] const PrefabObjectAddress &Object() const noexcept;
        /** @brief Returns the component schema identity. @return Registered component type. */
        [[nodiscard]] const Gameplay::ComponentTypeId &ComponentType() const noexcept;

        /** @brief Returns the component occurrence identity. @return Stable non-zero identity. */
        [[nodiscard]] constexpr PrefabComponentInstanceId ComponentInstance() const noexcept {
            return componentInstance_;
        }

        /** @brief Returns the property identity. @return Stable non-zero identity. */
        [[nodiscard]] constexpr PrefabPropertyId Property() const noexcept {
            return property_;
        }

        [[nodiscard]] auto operator<=>(const PrefabPropertyAddress &) const noexcept = default;

    private:
        PrefabPropertyAddress(PrefabObjectAddress object, Gameplay::ComponentTypeId componentType,
                              PrefabComponentInstanceId componentInstance, PrefabPropertyId property) noexcept
            : object_(std::move(object)), componentType_(std::move(componentType)), componentInstance_(componentInstance),
              property_(property) {}

        PrefabObjectAddress object_{};
        Gameplay::ComponentTypeId componentType_{};
        PrefabComponentInstanceId componentInstance_{};
        PrefabPropertyId property_{};
    };

    /** @brief Opaque project-owned component payload with a stable occurrence identity. */
    struct RawComponentPayload final {
        PrefabComponentInstanceId instance;      /**< Stable occurrence on its prefab object. */
        Gameplay::SerializedComponent component; /**< Canonical bytes preserved when project code is unavailable. */

        [[nodiscard]] bool operator==(const RawComponentPayload &) const noexcept = default;
    };

    /**
     * @brief Validates an opaque component envelope without interpreting its payload.
     * @param payload Candidate identity, schema and canonical bytes.
     * @return Success, a prefab identity/document error, or the owning Gameplay validation error.
     */
    [[nodiscard]] Result<void> ValidateRawComponentPayload(const RawComponentPayload &payload);
}  // namespace Horo::Prefab
