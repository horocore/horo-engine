#pragma once

/**
 * @file PropertyBindingRegistry.h
 * @brief Scene-owned typed property metadata and validating accessors.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/StableIdentity.h"
#include "Horo/Gameplay/Component.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Scene/PropertyBindingErrors.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Stable identity of one scene property binding descriptor. */
    using PropertyBindingId = Foundation::PropertyBindingId;

    /** @brief Property value kinds supported by the first cinematic typed-track contract. */
    enum class PropertyBindingType : std::uint8_t {
        Float,
        Vec2,
        Vec3,
        Vec4,
        Boolean,
        Bool = Boolean,
        Count
    };

    /** @brief Compatibility spelling used by model and inspector adapters. */
    using PropertyType = PropertyBindingType;

    /** @brief Value carrier shared by inspector reads and cinematic writes. */
    using PropertyBindingValue = std::variant<float, Math::Vec2, Math::Vec3, Math::Vec4, bool>;

    /** @brief Compatibility spelling for the typed value carrier. */
    using PropertyValue = PropertyBindingValue;

    /** @brief Optional inclusive scalar constraint applied component-wise to numeric values. */
    struct PropertyRangeConstraint final {
        std::optional<float> minimum;
        std::optional<float> maximum;

        [[nodiscard]] constexpr auto operator<=>(const PropertyRangeConstraint &) const noexcept = default;
    };

    /** @brief Compatibility spelling used by authoring-facing adapters. */
    using PropertyBindingRange = PropertyRangeConstraint;

    /** @brief Authority policy for a descriptor's setter. */
    enum class PropertyWritePolicy : std::uint8_t {
        DirectScene,
        Direct = DirectScene,
        OwnerRequest,
        ReadOnly,
        Count
    };

    /** @brief Typed getter supplied by the component owner; it must not throw or retain the component pointer. */
    using PropertyGetterFn = Result<PropertyBindingValue> (*)(const void *component);

    /** @brief Typed setter supplied by the component owner; it must validate lifetime and authority before writing. */
    using PropertySetterFn = Result<void> (*)(void *component, const PropertyBindingValue &value);

    /** @brief Compatibility spellings matching the architecture decision's accessor vocabulary. */
    using PropertyGetter = PropertyGetterFn;
    using PropertySetter = PropertySetterFn;

    /** @brief Immutable metadata and owner callbacks for one animatable scene property. */
    struct PropertyBindingDescriptor final {
        PropertyBindingId id;
        Gameplay::ComponentTypeId componentType;
        Gameplay::ComponentPropertyId property;
        std::string propertyName; /**< Authoring lookup alias; runtime tracks use `property` and `id`. */
        PropertyType type{PropertyType::Float};
        PropertyGetterFn getter{};
        PropertySetterFn setter{};
        PropertyRangeConstraint range;
        PropertyWritePolicy writePolicy{PropertyWritePolicy::DirectScene};
    };

    /** @brief Hard ceiling for one host-composed property binding snapshot. */
    inline constexpr std::size_t MaximumPropertyBindings = 4'096;

    /**
     * @brief Explicitly composed registry of typed scene/component property bindings.
     * @note Registration is mutable only before Freeze; descriptors are inert metadata and never discover services.
     */
    class PropertyBindingRegistry final {
    public:
        /**
         * @brief Copies and validates one descriptor into the open composition transaction.
         * @param descriptor Stable property metadata and owner accessors.
         * @return Success or a typed descriptor, duplicate, capacity, or lifecycle error.
         */
        [[nodiscard]] Result<void> Register(PropertyBindingDescriptor descriptor);

        /**
         * @brief Sorts and seals the registry for activation.
         * @return Success or a previously detected registry error.
         */
        [[nodiscard]] Result<void> Freeze();

        /** @brief Reports whether further registration is closed. @return True after Freeze. */
        [[nodiscard]] bool IsFrozen() const noexcept;

        /** @brief Returns the deterministic descriptor snapshot. @return Borrowed descriptors owned by this registry. */
        [[nodiscard]] std::span<const PropertyBindingDescriptor> Descriptors() const noexcept;

        /**
         * @brief Finds a binding by its exact generation-checked identity.
         * @param id Stable binding identity.
         * @return Borrowed descriptor or null when the identity is absent/stale.
         */
        [[nodiscard]] const PropertyBindingDescriptor *Find(PropertyBindingId id) const noexcept;

        /**
         * @brief Finds the authoring descriptor by component and stable property name.
         * @param componentType Exact component type.
         * @param propertyName Stable property name used by authoring/inspector lookup.
         * @return Borrowed descriptor or null when no matching binding exists.
         * @note Runtime/cooked tracks retain the returned descriptor's typed identity rather than this string.
         */
        [[nodiscard]] const PropertyBindingDescriptor *FindByName(const Gameplay::ComponentTypeId &componentType,
                                                                  std::string_view propertyName) const noexcept;

    private:
        std::vector<PropertyBindingDescriptor> descriptors_;
        bool frozen_{false};
    };
}  // namespace Horo::Runtime
