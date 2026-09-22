#include "UiStyleInternal.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace Horo::Runtime::Ui::StyleInternal {
    WorkingProperty *WorkingStyle::Find(const UiStylePropertyId id) noexcept {
        for (std::uint32_t index = 0; index < count; ++index)
            if (properties[index].value.property == id)
                return &properties[index];
        return nullptr;
    }

    const WorkingProperty *WorkingStyle::Find(const UiStylePropertyId id) const noexcept {
        for (std::uint32_t index = 0; index < count; ++index)
            if (properties[index].value.property == id)
                return &properties[index];
        return nullptr;
    }

    namespace {
        [[nodiscard]] Result<void> ApplyValue(WorkingStyle &style, const UiStylePropertyDescriptor &descriptor, const UiStyleValue &value,
                                              const UiStyleProvenance &provenance, const bool sealed,
                                              const std::uint32_t propertyCapacity) {
            if (!IsValueCompatible(descriptor, value))
                return Failure(UiErrors::StyleTypeMismatch);
            auto *existing = style.Find(descriptor.id);
            if (existing != nullptr) {
                if (existing->sealed)
                    return Failure(UiErrors::StyleReferenceInvalid);
                existing->value.value = value;
                existing->value.provenance = provenance;
                existing->sealed = sealed;
                return Result<void>::Success();
            }
            if (style.count == propertyCapacity || style.count == MaximumUiStyleProperties)
                return Failure(UiErrors::CapacityExceeded);
            style.properties[style.count++] = {{descriptor.id, value, provenance}, sealed};
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyAssignment(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                   const UiStyleAssignment &assignment, const UiStyleOrigin origin,
                                                   const RuntimeStyleAssetId asset, const UiStyleClassId styleClass,
                                                   const std::uint32_t propertyCapacity) {
            const auto *descriptor = FindProperty(registry.Properties(), assignment.property);
            if (descriptor == nullptr)
                return Failure(UiErrors::StyleReferenceInvalid);
            UiStyleValue value{};
            UiStyleTokenId token{};
            RuntimeStyleAssetId provenanceAsset = asset;
            if (assignment.value.referencesToken) {
                if (!assignment.value.token.IsValid())
                    return Failure(UiErrors::StyleReferenceInvalid);
                const auto resolved = registry.ResolveToken(assignment.value.token);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
                value = resolved.Value();
                token = assignment.value.token.id;
                provenanceAsset = assignment.value.token.asset;
            } else {
                value = assignment.value.literal;
            }
            const UiStyleProvenance provenance{origin, provenanceAsset, styleClass, token, registry.Generation()};
            return ApplyValue(style, *descriptor, value, provenance, assignment.sealed, propertyCapacity);
        }

        [[nodiscard]] Result<void> BuildAssetChain(const RuntimeStyleRegistry &registry, const RuntimeStyleAssetId asset,
                                                   std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> &chain,
                                                   std::uint32_t &count) {
            count = 0;
            auto current = asset;
            while (current.IsValid()) {
                if (count == chain.size())
                    return Failure(UiErrors::StyleCycle);
                for (std::uint32_t previous = 0; previous < count; ++previous)
                    if (chain[previous] == current)
                        return Failure(UiErrors::StyleCycle);
                const auto *definition = FindAsset(registry.Assets(), current);
                if (definition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                chain[count++] = current;
                current = definition->base.value_or(RuntimeStyleAssetId{});
            }
            for (std::uint32_t left = 0; left < count / 2; ++left)
                std::swap(chain[left], chain[count - left - 1]);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildClassChain(const RuntimeStyleRegistry &registry, const UiStyleClassReference classReference,
                                                   std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> &chain,
                                                   std::uint32_t &count) {
            count = 0;
            auto current = classReference;
            while (current.IsValid()) {
                if (count == chain.size())
                    return Failure(UiErrors::StyleCycle);
                for (std::uint32_t previous = 0; previous < count; ++previous)
                    if (chain[previous] == current)
                        return Failure(UiErrors::StyleCycle);
                const auto *definition = FindClass(registry.Assets(), current);
                if (definition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                chain[count++] = current;
                current = definition->base.value_or(UiStyleClassReference{});
            }
            for (std::uint32_t left = 0; left < count / 2; ++left)
                std::swap(chain[left], chain[count - left - 1]);
            return Result<void>::Success();
        }

        struct StateApplication final {
            const UiStyleStateOverride *state{};
            UiStyleOrigin origin{UiStyleOrigin::VisualState};
            RuntimeStyleAssetId asset;
            UiStyleClassId styleClass;
            std::uint32_t order{};
        };

        [[nodiscard]] Result<void> AddStateApplications(const UiStyleAssetDefinition &asset, const UiStyleOrigin origin,
                                                        const UiStyleClassId styleClass,
                                                        std::array<StateApplication, MaximumUiStyleStateBlocks> &applications,
                                                        std::uint32_t &count, std::uint32_t &order) {
            for (const auto &state : asset.states) {
                if (count == applications.size())
                    return Failure(UiErrors::CapacityExceeded);
                applications[count++] = {&state, origin, asset.id, styleClass, order++};
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AddClassStateApplications(const RuntimeStyleRegistry &registry,
                                                             const UiStyleClassReference classReference, const UiStyleOrigin origin,
                                                             std::array<StateApplication, MaximumUiStyleStateBlocks> &applications,
                                                             std::uint32_t &count, std::uint32_t &order) {
            std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> chain{};
            std::uint32_t chainCount = 0;
            if (const auto result = BuildClassChain(registry, classReference, chain, chainCount); result.HasError())
                return result;
            for (std::uint32_t index = 0; index < chainCount; ++index) {
                const auto *definition = FindClass(registry.Assets(), chain[index]);
                if (definition == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                for (const auto &state : definition->states) {
                    if (count == applications.size())
                        return Failure(UiErrors::CapacityExceeded);
                    applications[count++] = {&state, origin, chain[index].asset, chain[index].id, order++};
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyClassAssignments(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                         const UiStyleClassReference classReference, const UiStyleOrigin origin,
                                                         const std::uint32_t propertyCapacity) {
            std::array<UiStyleClassReference, MaximumUiStyleInheritanceDepth> chain{};
            std::uint32_t count = 0;
            if (const auto result = BuildClassChain(registry, classReference, chain, count); result.HasError())
                return result;
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto *styleClass = FindClass(registry.Assets(), chain[index]);
                if (styleClass == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                for (const auto &assignment : styleClass->assignments)
                    if (const auto result =
                            ApplyAssignment(style, registry, assignment, origin, chain[index].asset, chain[index].id, propertyCapacity);
                        result.HasError())
                        return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyRegisteredDefaults(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                           const std::uint32_t propertyCapacity) {
            for (const auto &property : registry.Properties()) {
                const UiStyleProvenance provenance{UiStyleOrigin::RegisteredDefault, RuntimeStyleAssetId{}, UiStyleClassId{},
                                                   UiStyleTokenId{}, registry.Generation()};
                if (const auto result = ApplyValue(style, property, property.defaultValue, provenance, false, propertyCapacity);
                    result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyAssetAssignments(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                         const std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> &chain,
                                                         const std::uint32_t count, const std::uint32_t propertyCapacity) {
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto *asset = FindAsset(registry.Assets(), chain[index]);
                if (asset == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                for (const auto &assignment : asset->assignments)
                    if (const auto result = ApplyAssignment(style, registry, assignment, UiStyleOrigin::StyleAsset, asset->id,
                                                            UiStyleClassId{}, propertyCapacity);
                        result.HasError())
                        return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyInheritedValues(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                        const WorkingStyle *parent, const std::uint32_t propertyCapacity) {
            if (parent == nullptr)
                return Result<void>::Success();
            for (std::uint32_t index = 0; index < parent->count; ++index) {
                const auto &parentProperty = parent->properties[index].value;
                const auto *descriptor = FindProperty(registry.Properties(), parentProperty.property);
                if (descriptor == nullptr || !descriptor->inheritsToChildren)
                    continue;
                UiStyleProvenance provenance = parentProperty.provenance;
                provenance.origin = UiStyleOrigin::Inherited;
                provenance.generation = registry.Generation();
                if (const auto result = ApplyValue(style, *descriptor, parentProperty.value, provenance, false, propertyCapacity);
                    result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyInlineOrPolicy(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                       const std::span<const UiStyleAssignment> assignments, const UiStyleOrigin origin,
                                                       const RuntimeStyleAssetId asset, const std::uint32_t propertyCapacity) {
            for (const auto &assignment : assignments)
                if (const auto result = ApplyAssignment(style, registry, assignment, origin, asset, UiStyleClassId{}, propertyCapacity);
                    result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CollectStateApplications(
            const RuntimeStyleRegistry &registry, const UiStyleElementInput &input,
            const std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> &assetChain, const std::uint32_t assetCount,
            std::array<StateApplication, MaximumUiStyleStateBlocks> &applications, std::uint32_t &applicationCount) {
            std::uint32_t order = 0;
            for (std::uint32_t index = 0; index < assetCount; ++index) {
                const auto *asset = FindAsset(registry.Assets(), assetChain[index]);
                if (asset == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (const auto result =
                        AddStateApplications(*asset, UiStyleOrigin::VisualState, UiStyleClassId{}, applications, applicationCount, order);
                    result.HasError())
                    return result;
            }
            if (input.typeClass.IsValid())
                if (const auto result = AddClassStateApplications(registry, input.typeClass, UiStyleOrigin::VisualState, applications,
                                                                  applicationCount, order);
                    result.HasError())
                    return result;
            for (const auto classReference : input.classes)
                if (const auto result = AddClassStateApplications(registry, classReference, UiStyleOrigin::VisualState, applications,
                                                                  applicationCount, order);
                    result.HasError())
                    return result;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyStateApplications(WorkingStyle &style, const RuntimeStyleRegistry &registry,
                                                          const UiStyleElementInput &input,
                                                          std::array<StateApplication, MaximumUiStyleStateBlocks> &applications,
                                                          const std::uint32_t applicationCount, const std::uint32_t propertyCapacity) {
            std::sort(applications.begin(), applications.begin() + applicationCount,
                      [](const StateApplication &left, const StateApplication &right) {
                if (left.state->layer != right.state->layer)
                    return left.state->layer < right.state->layer;
                const auto leftSpecificity = std::popcount(left.state->required.bits);
                const auto rightSpecificity = std::popcount(right.state->required.bits);
                if (leftSpecificity != rightSpecificity)
                    return leftSpecificity < rightSpecificity;
                return left.order < right.order;
            });
            for (std::uint32_t index = 0; index < applicationCount; ++index) {
                const auto &application = applications[index];
                if (!input.state.Contains(application.state->required) || (input.state.bits & application.state->forbidden.bits) != 0)
                    continue;
                for (const auto &assignment : application.state->assignments)
                    if (const auto result = ApplyAssignment(style, registry, assignment, application.origin, application.asset,
                                                            application.styleClass, propertyCapacity);
                        result.HasError())
                        return result;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAssignments(const RuntimeStyleRegistry &registry,
                                                       const std::span<const UiStyleAssignment> assignments,
                                                       const std::uint32_t propertyCapacity) {
            if (assignments.size() > propertyCapacity)
                return Failure(UiErrors::StyleInvalid);
            for (const auto &assignment : assignments) {
                if (!assignment.value.IsValid() || assignment.value.referencesToken)
                    return Failure(UiErrors::StyleInvalid);
                const auto *property = FindProperty(registry.Properties(), assignment.property);
                if (property == nullptr)
                    return Failure(UiErrors::StyleReferenceInvalid);
                if (!IsValueCompatible(*property, assignment.value.literal))
                    return Failure(UiErrors::StyleTypeMismatch);
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateElementInput(const RuntimeStyleRegistry &registry, const UiStyleElementInput &input,
                                      const std::uint32_t propertyCapacity) {
        if (!input.element.IsValid() || !input.asset.IsValid() || !registry.HasAsset(input.asset) || !input.state.IsValid() ||
            input.classes.size() > MaximumUiStyleClasses)
            return Failure(UiErrors::StyleInvalid);
        if (input.typeClass.IsValid() && !registry.HasClass(input.typeClass))
            return Failure(UiErrors::StyleReferenceInvalid);
        if (!input.typeClass.IsValid() && (input.typeClass.asset.IsValid() || input.typeClass.id.IsValid()))
            return Failure(UiErrors::StyleReferenceInvalid);
        for (const auto &classReference : input.classes)
            if (!classReference.IsValid() || !registry.HasClass(classReference))
                return Failure(UiErrors::StyleReferenceInvalid);
        if (const auto valid = ValidateAssignments(registry, input.inlineProperties, propertyCapacity); valid.HasError())
            return valid;
        return ValidateAssignments(registry, input.policyProperties, propertyCapacity);
    }

    Result<WorkingStyle> ResolveElement(const RuntimeStyleRegistry &registry, const UiStyleElementInput &input, const WorkingStyle *parent,
                                        const std::uint32_t propertyCapacity) {
        WorkingStyle style{};
        if (const auto result = ApplyRegisteredDefaults(style, registry, propertyCapacity); result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());

        std::array<RuntimeStyleAssetId, MaximumUiStyleInheritanceDepth> assetChain{};
        std::uint32_t assetCount = 0;
        if (const auto result = BuildAssetChain(registry, input.asset, assetChain, assetCount); result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());
        if (const auto result = ApplyAssetAssignments(style, registry, assetChain, assetCount, propertyCapacity); result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());
        if (const auto result = ApplyInheritedValues(style, registry, parent, propertyCapacity); result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());
        if (input.typeClass.IsValid())
            if (const auto result =
                    ApplyClassAssignments(style, registry, input.typeClass, UiStyleOrigin::ElementTypeClass, propertyCapacity);
                result.HasError())
                return Result<WorkingStyle>::Failure(result.ErrorValue());
        for (const auto classReference : input.classes)
            if (const auto result = ApplyClassAssignments(style, registry, classReference, UiStyleOrigin::AuthoredClass, propertyCapacity);
                result.HasError())
                return Result<WorkingStyle>::Failure(result.ErrorValue());
        if (const auto result =
                ApplyInlineOrPolicy(style, registry, input.inlineProperties, UiStyleOrigin::Inline, input.asset, propertyCapacity);
            result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());

        std::array<StateApplication, MaximumUiStyleStateBlocks> applications{};
        std::uint32_t applicationCount = 0;
        if (const auto result = CollectStateApplications(registry, input, assetChain, assetCount, applications, applicationCount);
            result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());
        if (const auto result = ApplyStateApplications(style, registry, input, applications, applicationCount, propertyCapacity);
            result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());
        if (const auto result = ApplyInlineOrPolicy(style, registry, input.policyProperties, UiStyleOrigin::AccessibilityPolicy,
                                                    input.asset, propertyCapacity);
            result.HasError())
            return Result<WorkingStyle>::Failure(result.ErrorValue());
        return Result<WorkingStyle>::Success(std::move(style));
    }
}  // namespace Horo::Runtime::Ui::StyleInternal
