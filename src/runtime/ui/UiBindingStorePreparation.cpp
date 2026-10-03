#include "UiBindingStoreInternal.h"

#include <new>

namespace Horo::Runtime::Ui {
    using BindingStoreInternal::Failure;

    namespace {

        /** @brief Checks caller bounds against finite repository ceilings. */
        [[nodiscard]] bool ValidLimits(const UiBindingStoreLimits &limits) noexcept {
            return limits.providers > 0 && limits.providers <= MaximumUiBindingProviders && limits.bindings > 0 &&
                   limits.bindings <= MaximumUiBindingDescriptors && limits.changes > 0 && limits.changes <= MaximumUiBindingChanges &&
                   limits.valueBytes > 0 && limits.valueBytes <= MaximumUiBindingStorageBytes && limits.changeBytes > 0 &&
                   limits.changeBytes <= MaximumUiBindingChangeBytes;
        }

        /** @brief Identifies target properties that change intrinsic content or visibility. */
        [[nodiscard]] bool LayoutTarget(const UiBindingTargetProperty target) noexcept {
            using enum UiBindingTargetProperty;
            return target == Text || target == LocalizedText || target == Visible;
        }

        /** @brief Identifies target properties that alter interaction availability or control semantics. */
        [[nodiscard]] bool ActionTarget(const UiBindingTargetProperty target) noexcept {
            using enum UiBindingTargetProperty;
            return target == Enabled || target == Visible || target == BooleanValue || target == ScalarValue || target == Selected;
        }

        /** @brief Derives target work from semantic effects and additional provider-declared dependencies. */
        [[nodiscard]] UiBindingDirty Categories(const UiBindingTargetProperty target, const UiBindingPropertyFlags flags) noexcept {
            using enum UiBindingDirty;
            auto result = Paint | Accessibility;
            if (LayoutTarget(target) || HasFlag(flags, UiBindingPropertyFlags::AffectsLayout))
                result = result | Layout;
            if (ActionTarget(target) || HasFlag(flags, UiBindingPropertyFlags::AffectsActions))
                result = result | Actions;
            return result;
        }

        /** @brief Reserves text capacity during preparation; scalar targets keep value semantics. */
        void ReserveValue(UiBindingValue &value, const std::size_t bytes) {
            std::visit([bytes]<typename Value>(Value &target) {
                if constexpr (std::is_same_v<Value, std::string>)
                    target.reserve(bytes);
                else if constexpr (std::is_same_v<Value, UiBindingLocalizedMessage>)
                    target.key.reserve(bytes);
            }, value);
        }

        /** @brief Locates an initial property in a validated sorted snapshot without storing a borrow. */
        [[nodiscard]] const UiBindingValue *InitialValue(const std::span<const UiBindingPropertyUpdate> values,
                                                         const std::size_t property) noexcept {
            const auto found = std::ranges::lower_bound(values, property, {}, &UiBindingPropertyUpdate::property);
            return found != values.end() && found->property == property ? &found->value : nullptr;
        }

        /** @brief Resolves one authored write-only seed or readable provider/fallback value without retaining its borrow. */
        [[nodiscard]] Result<const UiBindingValue *> InitialTargetValue(const UiResolvedBindingDescriptor &resolved,
                                                                        const std::span<const UiBindingPropertyUpdate> values,
                                                                        const std::size_t property) {
            const bool writeOnly = resolved.binding.direction == UiBindingDirection::TargetToSource;
            if (writeOnly != resolved.initialTarget.has_value())
                return Failure<const UiBindingValue *>(UiErrors::BindingValueInvalid);
            if (writeOnly)
                return Result<const UiBindingValue *>::Success(&*resolved.initialTarget);
            if (const auto *value = InitialValue(values, property))
                return Result<const UiBindingValue *>::Success(value);
            if (!resolved.binding.fallback)
                return Failure<const UiBindingValue *>(UiErrors::BindingProviderUnknown);
            return Result<const UiBindingValue *>::Success(&*resolved.binding.fallback);
        }

        /** @brief Validates explicit host registration identity and schema-admitted provider scope. */
        [[nodiscard]] Result<void> ValidateRegistration(const UiBindingProviderRegistration &registration) {
            if (!registration.instance.IsValid() || !registration.revision.IsValid() || !registration.schema ||
                registration.scope >= UiBindingProviderScopeKind::Count)
                return Failure(UiErrors::BindingDescriptorInvalid);
            if ((static_cast<unsigned>(registration.schema->AllowedScopes()) &
                 static_cast<unsigned>(UiBindingProviderScopeBit(registration.scope))) == 0)
                return Failure(UiErrors::BindingAccessInvalid);
            return Result<void>::Success();
        }

        /** @brief Checks initial snapshot ordering and value contracts before provider metadata is copied. */
        [[nodiscard]] Result<void> ValidateInitial(const UiBindingProviderRegistration &registration) {
            const auto properties = registration.schema->Properties();
            if (registration.values.size() > properties.size())
                return Failure(UiErrors::BindingCapacityExceeded);
            for (std::size_t index = 0; index < registration.values.size(); ++index) {
                const auto &value = registration.values[index];
                if (value.property >= properties.size())
                    return Failure(UiErrors::BindingPropertyUnknown);
                if (index != 0 && registration.values[index - 1].property >= value.property)
                    return Failure(UiErrors::BindingDescriptorConflict);
                const auto &property = properties[value.property];
                if (const auto valid = BindingInternal::ValidateValue(value.value, property.type, property.limits); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        /** @brief Admits the worst-case target/fallback text storage before constructing the owned target. */
        [[nodiscard]] Result<void> AdmitTextStorage(const UiBindingDescriptor &binding, const UiBindingValueType type,
                                                    const std::size_t capacity, std::size_t &reservedBytes) {
            if (type == UiBindingValueType::BoundedText || type == UiBindingValueType::LocalizedMessage) {
                reservedBytes += binding.target.limits.maximumBytes * (binding.fallback ? 2U : 1U);
                if (reservedBytes > capacity)
                    return Failure(UiErrors::BindingCapacityExceeded);
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiBindingStore::Storage::PrepareProviders */
    Result<void> UiBindingStore::Storage::PrepareProviders(const std::span<const UiBindingProviderRegistration> registrations) {
        for (std::size_t index = 0; index < registrations.size(); ++index) {
            const auto &registration = registrations[index];
            if (const auto valid = ValidateRegistration(registration); valid.HasError())
                return valid;
            if (index != 0 && !(registrations[index - 1].instance < registration.instance))
                return Failure(UiErrors::BindingDescriptorConflict);
            if (const auto valid = ValidateInitial(registration); valid.HasError())
                return valid;
            providers.emplace_back(registration.instance, *registration.schema, registration.revision, true,
                                   std::vector<std::vector<std::size_t>>(registration.schema->Properties().size()), registration.scope);
        }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::ResolveTarget */
    Result<UiElementHandle> UiBindingStore::Storage::ResolveTarget(const UiElementTree &tree, const UiBindingDescriptor &binding,
                                                                   const UiBindingProviderSchema &schema) const {
        if (const auto valid = ValidateUiBindingDescriptor(binding, schema); valid.HasError())
            return Result<UiElementHandle>::Failure(valid.ErrorValue());
        if (binding.converter.has_value())
            return Failure<UiElementHandle>(UiErrors::BindingConverterInvalid);
        const auto handle = tree.Find(binding.target.element);
        if (handle.HasError())
            return handle;
        if (std::ranges::any_of(targets, [&binding, &handle](const Target &target) {
            return target.bound.binding == binding.id ||
                   (target.bound.element == handle.Value() && target.bound.property == binding.target.property);
        }))
            return Failure<UiElementHandle>(UiErrors::BindingDescriptorConflict);
        return handle;
    }

    /** @copydoc UiBindingStore::Storage::PrepareTarget */
    Result<void> UiBindingStore::Storage::PrepareTarget(const UiElementTree &tree, const UiResolvedBindingDescriptor &resolved,
                                                        const std::span<const UiBindingProviderRegistration> registrations,
                                                        std::size_t &reservedBytes) {
        auto *provider = FindProvider(resolved.provider);
        if (!provider)
            return Failure(UiErrors::BindingProviderUnknown);
        const auto &binding = resolved.binding;
        const auto handle = ResolveTarget(tree, binding, provider->schema);
        if (handle.HasError())
            return Result<void>::Failure(handle.ErrorValue());
        const auto *property = provider->schema.Find(binding.source.property);
        const auto propertySlot = static_cast<std::size_t>(property - provider->schema.Properties().data());
        const auto providerIndex = static_cast<std::size_t>(provider - providers.data());
        const auto type = *UiBindingTargetValueType(binding.target.property);
        const bool writeOnly = binding.direction == UiBindingDirection::TargetToSource;
        const auto initial = InitialTargetValue(resolved, registrations[providerIndex].values, propertySlot);
        if (initial.HasError())
            return Result<void>::Failure(initial.ErrorValue());
        const auto *value = initial.Value();
        auto origin = UiBindingValueOrigin::UiLocal;
        if (!writeOnly)
            origin = InitialValue(registrations[providerIndex].values, propertySlot) ? UiBindingValueOrigin::Provider
                                                                                     : UiBindingValueOrigin::Fallback;
        if (const auto valid =
                BindingInternal::ValidateValue(*value, *UiBindingTargetValueType(binding.target.property), binding.target.limits);
            valid.HasError())
            return valid;
        if (writeOnly)
            if (const auto valid = BindingInternal::ValidateValue(*value, property->type, property->limits); valid.HasError())
                return valid;
        if (const auto admitted = AdmitTextStorage(binding, property->type, limits.valueBytes, reservedBytes); admitted.HasError())
            return admitted;
        Target target{{binding.id, handle.Value(), binding.target.property, *value, origin},
                      binding.target.limits,
                      binding.fallback,
                      Categories(binding.target.property, property->flags)};
        target.provider = providerIndex;
        target.property = static_cast<std::uint16_t>(propertySlot);
        target.direction = binding.direction;
        if (const auto prepared = PrepareDraftStorage(target, type, reservedBytes); prepared.HasError())
            return prepared;
        target.pending = target.categories;
        provider->targets[propertySlot].push_back(targets.size());
        targets.push_back(std::move(target));
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::PrepareDraftStorage */
    Result<void> UiBindingStore::Storage::PrepareDraftStorage(Target &target, const UiBindingValueType type,
                                                              std::size_t &reservedBytes) const {
        target.draft = target.bound.value;
        if (type == UiBindingValueType::BoundedText || type == UiBindingValueType::LocalizedMessage) {
            reservedBytes += target.limits.maximumBytes;
            if (reservedBytes > limits.valueBytes)
                return Failure(UiErrors::BindingCapacityExceeded);
        }
        ReserveValue(target.bound.value, target.limits.maximumBytes);
        ReserveValue(target.draft, target.limits.maximumBytes);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::Storage::PrepareTargets */
    Result<void> UiBindingStore::Storage::PrepareTargets(const UiElementTree &tree,
                                                         const std::span<const UiBindingProviderRegistration> registrations,
                                                         const std::span<const UiResolvedBindingDescriptor> bindings) {
        std::size_t reservedBytes = 0;
        for (const auto &binding : bindings)
            if (const auto prepared = PrepareTarget(tree, binding, registrations, reservedBytes); prepared.HasError())
                return prepared;
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
