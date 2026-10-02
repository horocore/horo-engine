#include "UiBindingStoreInternal.h"

#include <new>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Checks caller bounds against finite repository ceilings. */
        [[nodiscard]] bool ValidLimits(const UiBindingStoreLimits &limits) noexcept {
            return limits.providers > 0 && limits.providers <= MaximumUiBindingProviders && limits.bindings > 0 &&
                   limits.bindings <= MaximumUiBindingDescriptors && limits.changes > 0 && limits.changes <= MaximumUiBindingChanges &&
                   limits.valueBytes > 0 && limits.valueBytes <= MaximumUiBindingStorageBytes && limits.changeBytes > 0 &&
                   limits.changeBytes <= MaximumUiBindingChangeBytes;
        }

        /** @brief Derives target work from semantic effects and additional provider-declared dependencies. */
        [[nodiscard]] UiBindingDirty Categories(const UiBindingTargetProperty target, const UiBindingPropertyFlags flags) noexcept {
            auto result = UiBindingDirty::Paint | UiBindingDirty::Accessibility;
            if (target == UiBindingTargetProperty::Text || target == UiBindingTargetProperty::LocalizedText ||
                target == UiBindingTargetProperty::Visible || HasFlag(flags, UiBindingPropertyFlags::AffectsLayout))
                result = result | UiBindingDirty::Layout;
            if (target == UiBindingTargetProperty::Enabled || target == UiBindingTargetProperty::Visible ||
                target == UiBindingTargetProperty::BooleanValue || target == UiBindingTargetProperty::ScalarValue ||
                target == UiBindingTargetProperty::Selected || HasFlag(flags, UiBindingPropertyFlags::AffectsActions))
                result = result | UiBindingDirty::Actions;
            return result;
        }

        /** @brief Reserves text capacity during preparation; scalar targets keep value semantics. */
        void ReserveValue(UiBindingValue &value, const std::size_t bytes) {
            if (auto *text = std::get_if<std::string>(&value))
                text->reserve(bytes);
            else if (auto *message = std::get_if<UiBindingLocalizedMessage>(&value))
                message->key.reserve(bytes);
        }

        /** @brief Locates an initial property in a validated sorted snapshot without storing a borrow. */
        [[nodiscard]] const UiBindingValue *InitialValue(const std::span<const UiBindingPropertyUpdate> values,
                                                         const std::size_t property) noexcept {
            const auto found = std::ranges::lower_bound(values, property, {}, &UiBindingPropertyUpdate::property);
            return found != values.end() && found->property == property ? &found->value : nullptr;
        }
    }  // namespace

    /** @copydoc UiBindingStore::Storage::PrepareProviders */
    Result<void> UiBindingStore::Storage::PrepareProviders(const std::span<const UiBindingProviderRegistration> registrations) {
        for (std::size_t index = 0; index < registrations.size(); ++index) {
            const auto &registration = registrations[index];
            if (!registration.instance.IsValid() || !registration.revision.IsValid() || !registration.schema ||
                registration.scope >= UiBindingProviderScopeKind::Count)
                return Failure(UiErrors::BindingDescriptorInvalid);
            if (index != 0 && !(registrations[index - 1].instance < registration.instance))
                return Failure(UiErrors::BindingDescriptorConflict);
            if ((registration.schema->AllowedScopes() & UiBindingProviderScopeBit(registration.scope)) == 0)
                return Failure(UiErrors::BindingAccessInvalid);
            const auto properties = registration.schema->Properties();
            if (registration.values.size() > properties.size())
                return Failure(UiErrors::BindingCapacityExceeded);
            for (std::size_t valueIndex = 0; valueIndex < registration.values.size(); ++valueIndex) {
                const auto &value = registration.values[valueIndex];
                if (value.property >= properties.size())
                    return Failure(UiErrors::BindingPropertyUnknown);
                if (valueIndex != 0 && registration.values[valueIndex - 1].property >= value.property)
                    return Failure(UiErrors::BindingDescriptorConflict);
                const auto &property = properties[value.property];
                if (const auto valid = BindingInternal::ValidateValue(value.value, property.type, property.limits); valid.HasError())
                    return valid;
            }
            providers.push_back({registration.instance, *registration.schema, registration.revision, true,
                                 std::vector<std::vector<std::size_t>>(properties.size())});
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::PrepareTargets */
    Result<void> UiBindingStore::Storage::PrepareTargets(const UiElementTree &tree,
                                                         const std::span<const UiBindingProviderRegistration> registrations,
                                                         const std::span<const UiResolvedBindingDescriptor> bindings) {
        std::size_t reservedBytes = 0;
        for (const auto &resolved : bindings) {
            auto *provider = FindProvider(resolved.provider);
            if (!provider)
                return Failure(UiErrors::BindingProviderUnknown);
            const auto &binding = resolved.binding;
            if (const auto valid = ValidateUiBindingDescriptor(binding, provider->schema); valid.HasError())
                return valid;
            if (binding.direction == UiBindingDirection::TargetToSource)
                return Failure(UiErrors::BindingAccessInvalid);
            if (binding.converter.has_value())
                return Failure(UiErrors::BindingConverterInvalid);
            if (std::ranges::any_of(targets, [&binding](const Target &target) {
                return target.bound.binding == binding.id;
            }))
                return Failure(UiErrors::BindingDescriptorConflict);
            const auto handle = tree.Find(binding.target.element);
            if (handle.HasError())
                return Result<void>::Failure(handle.ErrorValue());
            if (std::ranges::any_of(targets, [&binding, &handle](const Target &target) {
                return target.bound.element == handle.Value() && target.bound.property == binding.target.property;
            }))
                return Failure(UiErrors::BindingDescriptorConflict);
            const auto *property = provider->schema.Find(binding.source.property);
            const auto propertySlot = static_cast<std::size_t>(property - provider->schema.Properties().data());
            const auto providerIndex = static_cast<std::size_t>(provider - providers.data());
            const auto *value = InitialValue(registrations[providerIndex].values, propertySlot);
            const auto origin = value ? UiBindingValueOrigin::Provider : UiBindingValueOrigin::Fallback;
            if (!value) {
                if (!binding.fallback)
                    return Failure(UiErrors::BindingProviderUnknown);
                value = &*binding.fallback;
            }
            if (const auto valid =
                    BindingInternal::ValidateValue(*value, *UiBindingTargetValueType(binding.target.property), binding.target.limits);
                valid.HasError())
                return valid;
            const bool text = property->type == UiBindingValueType::BoundedText || property->type == UiBindingValueType::LocalizedMessage;
            if (text) {
                reservedBytes += binding.target.limits.maximumBytes * (binding.fallback ? 2U : 1U);
                if (reservedBytes > limits.valueBytes)
                    return Failure(UiErrors::BindingCapacityExceeded);
            }
            Target target{{binding.id, handle.Value(), binding.target.property, *value, origin},
                          binding.target.limits,
                          binding.fallback,
                          Categories(binding.target.property, property->flags)};
            ReserveValue(target.bound.value, binding.target.limits.maximumBytes);
            target.pending = target.categories;
            provider->targets[propertySlot].push_back(targets.size());
            targets.push_back(std::move(target));
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Create */
    Result<UiBindingStore> UiBindingStore::Create(const UiElementTree &tree, const std::span<const UiBindingProviderRegistration> providers,
                                                  const std::span<const UiResolvedBindingDescriptor> bindings,
                                                  const UiBindingStoreLimits &limits) {
        if (tree.State() != UiElementTreeState::Active)
            return Failure<UiBindingStore>(UiErrors::BindingLifecycleUnavailable);
        if (!ValidLimits(limits) || providers.size() > limits.providers || bindings.size() > limits.bindings)
            return Failure<UiBindingStore>(UiErrors::BindingCapacityExceeded);
        try {
            auto storage = std::make_unique<Storage>(tree, limits);
            if (const auto prepared = storage->PrepareProviders(providers); prepared.HasError())
                return Result<UiBindingStore>::Failure(prepared.ErrorValue());
            if (const auto prepared = storage->PrepareTargets(tree, providers, bindings); prepared.HasError())
                return Result<UiBindingStore>::Failure(prepared.ErrorValue());
            return Result<UiBindingStore>::Success(UiBindingStore{std::move(storage)});
        } catch (const std::bad_alloc &) {
            return Failure<UiBindingStore>(UiErrors::BindingCapacityExceeded);
        }
    }
}  // namespace Horo::Runtime::Ui
