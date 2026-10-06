#include "UiAnimationLayoutProjection.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>

namespace Horo::Runtime::Ui::AnimationInternal {
    namespace {
        /** @brief Applies one closed field in canonical logical units; style validation rejects negative sizes and invalid constraints. */
        Result<void> ApplyField(UiLayoutStyle &style, const UiAnimationLayoutField field, const std::int32_t value) {
            switch (field) {
                case UiAnimationLayoutField::Width:
                    style.width = UiLength::Dip(value);
                    break;
                case UiAnimationLayoutField::Height:
                    style.height = UiLength::Dip(value);
                    break;
                case UiAnimationLayoutField::MinimumWidth:
                    style.minimumWidth = UiLength::Dip(value);
                    break;
                case UiAnimationLayoutField::MinimumHeight:
                    style.minimumHeight = UiLength::Dip(value);
                    break;
                case UiAnimationLayoutField::MaximumWidth:
                    style.maximumWidth = UiLength::Dip(value);
                    break;
                case UiAnimationLayoutField::MaximumHeight:
                    style.maximumHeight = UiLength::Dip(value);
                    break;
                case UiAnimationLayoutField::OffsetLeft:
                    style.offsets.left = value;
                    break;
                case UiAnimationLayoutField::OffsetTop:
                    style.offsets.top = value;
                    break;
                case UiAnimationLayoutField::Count:
                    return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
                default:
                    return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            }
            return Result<void>::Success();
        }

        /** @brief Reads only the actual immutable computed property's typed Dimension category. */
        Result<std::int32_t> Dimension(const UiComputedStyleSnapshot &styles, const LayoutBinding &binding) {
            const auto record = styles.Get(binding.element);
            if (record.HasError())
                return Result<std::int32_t>::Failure(record.ErrorValue());
            const auto properties = styles.Properties(record.Value());
            const auto property = std::ranges::find(properties, binding.property, &UiComputedStyleProperty::property);
            if (property == properties.end())
                return Result<std::int32_t>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            const auto *dimension = std::get_if<UiStyleDimension>(&property->value);
            if (!dimension)
                return Result<std::int32_t>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            return Result<std::int32_t>::Success(dimension->value);
        }
    }  // namespace

    /** @copydoc ProjectLayout */
    Result<void> ProjectLayout(const UiComputedStyleSnapshot &styles, const std::span<const LayoutBinding> bindings,
                               const std::span<UiLayoutElementDescriptor> descriptors) {
        for (const auto &binding : bindings) {
            const auto descriptor = std::ranges::find(descriptors, binding.element, &UiLayoutElementDescriptor::element);
            if (descriptor == descriptors.end())
                return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
            const auto value = Dimension(styles, binding);
            if (value.HasError())
                return Result<void>::Failure(value.ErrorValue());
            if (const auto applied = ApplyField(descriptor->style, binding.field, value.Value()); applied.HasError())
                return applied;
        }
        if (std::ranges::any_of(descriptors, [](const auto &descriptor) {
            return !descriptor.IsValid();
        }))
            return Result<void>::Failure(MakeError(UiErrors::AnimationPolicyInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::AnimationInternal
