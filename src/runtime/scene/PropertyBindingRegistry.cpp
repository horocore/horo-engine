#include "Horo/Runtime/Scene/PropertyBindingRegistry.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using PropertyBindingErrors::DuplicateBinding;
        using PropertyBindingErrors::InvalidDescriptor;
        using PropertyBindingErrors::RegistryFrozen;

        [[nodiscard]] bool IsValidType(const PropertyBindingType type) noexcept {
            return type < PropertyBindingType::Count;
        }

        [[nodiscard]] bool IsValidWritePolicy(const PropertyWritePolicy policy) noexcept {
            return policy < PropertyWritePolicy::Count;
        }

        [[nodiscard]] bool IsValidRange(const PropertyRangeConstraint &range) noexcept {
            if (range.minimum && !std::isfinite(*range.minimum))
                return false;
            if (range.maximum && !std::isfinite(*range.maximum))
                return false;
            return !range.minimum || !range.maximum || *range.minimum <= *range.maximum;
        }

        [[nodiscard]] Result<void> ValidateDescriptor(PropertyBindingDescriptor &descriptor) {
            if (!descriptor.id.IsValid() || !descriptor.componentType.IsValid())
                return Result<void>::Failure(MakeError(InvalidDescriptor));

            if (!descriptor.property.IsValid()) {
                if (descriptor.propertyName.empty())
                    return Result<void>::Failure(MakeError(InvalidDescriptor));
                auto parsed = Gameplay::ComponentPropertyId::Parse(descriptor.propertyName);
                if (parsed.HasError())
                    return Result<void>::Failure(MakeError(InvalidDescriptor));
                descriptor.property = std::move(parsed).Value();
            } else if (!descriptor.propertyName.empty() && descriptor.propertyName != descriptor.property.Value()) {
                return Result<void>::Failure(MakeError(InvalidDescriptor));
            }

            if (descriptor.propertyName.empty())
                descriptor.propertyName = descriptor.property.Value();
            if (!IsValidType(descriptor.type) || descriptor.getter == nullptr || descriptor.setter == nullptr ||
                !IsValidWritePolicy(descriptor.writePolicy) || !IsValidRange(descriptor.range))
                return Result<void>::Failure(MakeError(InvalidDescriptor));
            return Result<void>::Success();
        }

        [[nodiscard]] bool SameComponentProperty(const PropertyBindingDescriptor &left,
                                                  const PropertyBindingDescriptor &right) noexcept {
            return left.componentType == right.componentType && left.property == right.property;
        }
    }  // namespace

    /** @copydoc PropertyBindingRegistry::Register */
    Result<void> PropertyBindingRegistry::Register(PropertyBindingDescriptor descriptor) {
        if (frozen_)
            return Result<void>::Failure(MakeError(RegistryFrozen));
        if (descriptors_.size() >= MaximumPropertyBindings)
            return Result<void>::Failure(MakeError(InvalidDescriptor, "The property binding registry capacity was exceeded."));
        if (auto valid = ValidateDescriptor(descriptor); valid.HasError())
            return valid;
        if (Find(descriptor.id) != nullptr ||
            std::ranges::any_of(descriptors_, [&descriptor](const PropertyBindingDescriptor &current) {
                return SameComponentProperty(current, descriptor);
            }))
            return Result<void>::Failure(MakeError(DuplicateBinding));
        descriptors_.push_back(std::move(descriptor));
        return Result<void>::Success();
    }

    /** @copydoc PropertyBindingRegistry::Freeze */
    Result<void> PropertyBindingRegistry::Freeze() {
        if (frozen_)
            return Result<void>::Success();
        std::ranges::sort(descriptors_, {}, &PropertyBindingDescriptor::id);
        frozen_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PropertyBindingRegistry::IsFrozen */
    bool PropertyBindingRegistry::IsFrozen() const noexcept {
        return frozen_;
    }

    /** @copydoc PropertyBindingRegistry::Descriptors */
    std::span<const PropertyBindingDescriptor> PropertyBindingRegistry::Descriptors() const noexcept {
        return descriptors_;
    }

    /** @copydoc PropertyBindingRegistry::Find */
    const PropertyBindingDescriptor *PropertyBindingRegistry::Find(const PropertyBindingId id) const noexcept {
        if (!id.IsValid())
            return nullptr;
        if (!frozen_) {
            const auto found = std::ranges::find(descriptors_, id, &PropertyBindingDescriptor::id);
            return found == descriptors_.end() ? nullptr : std::to_address(found);
        }
        const auto found = std::ranges::lower_bound(descriptors_, id, {}, &PropertyBindingDescriptor::id);
        if (found == descriptors_.end() || found->id != id)
            return nullptr;
        return std::to_address(found);
    }

    /** @copydoc PropertyBindingRegistry::FindByName */
    const PropertyBindingDescriptor *PropertyBindingRegistry::FindByName(const Gameplay::ComponentTypeId &componentType,
                                                                         const std::string_view propertyName) const noexcept {
        if (!componentType.IsValid() || propertyName.empty())
            return nullptr;
        const auto found = std::ranges::find_if(descriptors_, [&componentType, propertyName](const PropertyBindingDescriptor &descriptor) {
            return descriptor.componentType == componentType &&
                   (descriptor.propertyName == propertyName || descriptor.property.Value() == propertyName);
        });
        return found == descriptors_.end() ? nullptr : std::to_address(found);
    }
}  // namespace Horo::Runtime
