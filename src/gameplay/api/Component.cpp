#include "Horo/Gameplay/Component.h"

#include "GameplayIdentityValidation.h"
#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>

namespace Horo::Gameplay {
    /** @copydoc ComponentTypeId::Parse */
    Result<ComponentTypeId> ComponentTypeId::Parse(const std::string_view value) {
        if (!Detail::IsNamespacedGameplayId(value, MaximumComponentTypeIdBytes))
            return Result<ComponentTypeId>::Failure(MakeError(GameplayErrors::InvalidComponentTypeId));
        return Result<ComponentTypeId>::Success(ComponentTypeId{std::string{value}});
    }

    /** @copydoc ComponentTypeId::Value */
    const std::string &ComponentTypeId::Value() const noexcept {
        return value_;
    }

    /** @copydoc ComponentTypeId::IsValid */
    bool ComponentTypeId::IsValid() const noexcept {
        return !value_.empty();
    }

    /** @copydoc ComponentPropertyId::Parse */
    Result<ComponentPropertyId> ComponentPropertyId::Parse(const std::string_view value) {
        if (!Detail::IsLowercaseIdentifier(value, MaximumComponentPropertyIdBytes))
            return Result<ComponentPropertyId>::Failure(MakeError(GameplayErrors::InvalidComponentDescriptor));
        return Result<ComponentPropertyId>::Success(ComponentPropertyId{std::string{value}});
    }

    /** @copydoc ComponentPropertyId::Value */
    const std::string &ComponentPropertyId::Value() const noexcept {
        return value_;
    }

    /** @copydoc ComponentPropertyId::IsValid */
    bool ComponentPropertyId::IsValid() const noexcept {
        return !value_.empty();
    }

    /** @copydoc ValidateSerializedComponent */
    Result<void> ValidateSerializedComponent(const SerializedComponent &component) {
        if (!component.typeId.IsValid() || component.schemaVersion == 0 || component.encoding != ComponentPayloadEncoding::CanonicalJson ||
            component.payload.size() > MaximumSerializedComponentBytes)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidSerializedComponent));
        return Result<void>::Success();
    }

    /** @copydoc ValidateSerializedComponents */
    Result<void> ValidateSerializedComponents(const std::span<const SerializedComponent> components) {
        if (components.size() > MaximumSerializedComponentsPerObject)
            return Result<void>::Failure(MakeError(GameplayErrors::InvalidSerializedComponent));
        for (std::size_t index = 0; index < components.size(); ++index) {
            const SerializedComponent &component = components[index];
            if (const Result<void> valid = ValidateSerializedComponent(component); valid.HasError())
                return valid;
            if (std::ranges::find_if(components.begin(), components.begin() + index, [&component](const SerializedComponent &other) {
                return other.typeId == component.typeId;
            }) != components.begin() + index)
                return Result<void>::Failure(MakeError(GameplayErrors::DuplicateComponentType));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Gameplay
