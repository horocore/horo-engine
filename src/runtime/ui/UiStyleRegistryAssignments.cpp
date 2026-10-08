#include "UiStyleInternal.h"

namespace Horo::Runtime::Ui::StyleInternal {
    namespace {
        /** @brief Qualifies every authored value against the complete immutable property and token tables. */
        Result<void> ValidateAssignments(const RuntimeStyleRegistry &registry, const std::span<const UiStyleAssignment> assignments) {
            for (const auto &assignment : assignments) {
                const auto *property = FindProperty(registry.Properties(), assignment.property);
                if (property == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (assignment.value.referencesToken) {
                    const auto value = registry.ResolveToken(assignment.value.token);
                    if (value.HasError())
                        return Result<void>::Failure(value.ErrorValue());
                    if (!IsValueCompatible(*property, value.Value()))
                        return Failure(UiErrors::StyleTypeMismatch);
                } else if (!IsValueCompatible(*property, assignment.value.literal)) {
                    return Failure(UiErrors::StyleTypeMismatch);
                }
            }
            return Result<void>::Success();
        }

        /** @brief Validates inactive state assignments before any future state can select them. */
        Result<void> ValidateStates(const RuntimeStyleRegistry &registry, const std::span<const UiStyleStateOverride> states) {
            for (const auto &state : states)
                if (const auto valid = ValidateAssignments(registry, state.assignments); valid.HasError())
                    return valid;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateRegistryAssignments */
    Result<void> ValidateRegistryAssignments(const RuntimeStyleRegistry &registry) {
        for (const auto &asset : registry.Assets()) {
            if (const auto valid = ValidateAssignments(registry, asset.assignments); valid.HasError())
                return valid;
            if (const auto valid = ValidateStates(registry, asset.states); valid.HasError())
                return valid;
            for (const auto &styleClass : asset.classes) {
                if (const auto valid = ValidateAssignments(registry, styleClass.assignments); valid.HasError())
                    return valid;
                if (const auto valid = ValidateStates(registry, styleClass.states); valid.HasError())
                    return valid;
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::StyleInternal
