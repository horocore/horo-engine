#include "Horo/Runtime/Scene/SaveableComponentState.h"

#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &code, std::string message = {}) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

        [[nodiscard]] bool IsValidPresence(const SaveableComponentPresence presence) noexcept {
            return presence == SaveableComponentPresence::Optional || presence == SaveableComponentPresence::Required;
        }

        [[nodiscard]] CanonicalCodecLimits PayloadLimits(const std::size_t maximumPayloadBytes) noexcept {
            CanonicalCodecLimits limits;
            limits.maximumBytes = maximumPayloadBytes;
            limits.maximumDecodedBytes = maximumPayloadBytes;
            limits.maximumStringBytes = maximumPayloadBytes;
            return limits;
        }

        [[nodiscard]] bool IsValidAdapterDescriptor(const SaveableComponentAdapterDescriptor &descriptor,
                                                    const Gameplay::ComponentRegistry &components) noexcept {
            return descriptor.type.IsValid() && descriptor.schemaVersion != 0 && descriptor.maximumPayloadBytes != 0 &&
                   descriptor.maximumPayloadBytes <= Gameplay::MaximumSerializedComponentBytes &&
                   components.Find(descriptor.type) != nullptr;
        }

        [[nodiscard]] bool IsPayloadWithinLimit(const std::span<const std::byte> payload, const std::size_t maximumPayloadBytes) noexcept {
            return !payload.empty() && payload.size() <= maximumPayloadBytes;
        }

        [[nodiscard]] Result<void> ValidateRestoreRecord(const SavedComponentStateRecord &record,
                                                         const SaveableComponentAdapterDescriptor &descriptor) {
            if (record.schemaVersion == 0)
                return Fail<void>(SceneErrors::SaveableComponentInvalid);
            if (!IsPayloadWithinLimit(record.payload, descriptor.maximumPayloadBytes))
                return Fail<void>(SceneErrors::SaveableComponentPayloadInvalid);
            if (record.schemaVersion > descriptor.schemaVersion)
                return Fail<void>(SceneErrors::SaveableComponentSchemaUnsupported);
            return Result<void>::Success();
        }
    }  // namespace

    SaveableComponentAdapterRegistry::SaveableComponentAdapterRegistry(std::vector<Binding> bindings) noexcept
        : bindings_(std::move(bindings)) {}

    Result<std::vector<SaveableComponentAdapterRegistry::Binding>> SaveableComponentAdapterRegistry::BuildBindings(
        const Gameplay::ComponentRegistry &components, const std::span<const SaveableComponentRequirement> requirements) {
        std::vector<Binding> bindings;
        bindings.reserve(requirements.size());
        for (const auto &requirement : requirements) {
            if (!requirement.type.IsValid() || !IsValidPresence(requirement.presence) || components.Find(requirement.type) == nullptr)
                return Fail<std::vector<Binding>>(SceneErrors::SaveableComponentInvalid);
            bindings.emplace_back(Binding{.requirement = requirement});
        }
        std::ranges::sort(bindings, {}, [](const Binding &binding) {
            return binding.requirement.type;
        });
        if (std::ranges::adjacent_find(bindings, {}, [](const Binding &binding) {
            return binding.requirement.type;
        }) != bindings.end())
            return Fail<std::vector<Binding>>(SceneErrors::SaveableComponentDuplicate);
        return Result<std::vector<Binding>>::Success(std::move(bindings));
    }

    Result<void> SaveableComponentAdapterRegistry::BindAdapters(
        const Gameplay::ComponentRegistry &components,
        const std::span<const std::shared_ptr<const ISaveableComponentStateAdapter>> adapters, std::vector<Binding> &bindings) {
        for (const auto &adapter : adapters) {
            if (adapter == nullptr)
                return Fail<void>(SceneErrors::SaveableComponentInvalid);
            const auto &descriptor = adapter->Descriptor();
            const auto binding = std::ranges::lower_bound(bindings, descriptor.type, {}, [](const Binding &value) {
                return value.requirement.type;
            });
            if (!IsValidAdapterDescriptor(descriptor, components) || binding == bindings.end() ||
                binding->requirement.type != descriptor.type)
                return Fail<void>(SceneErrors::SaveableComponentInvalid);
            if (binding->adapter != nullptr)
                return Fail<void>(SceneErrors::SaveableComponentDuplicate);
            binding->descriptor = descriptor;
            binding->adapter = adapter;
        }
        return Result<void>::Success();
    }

    Result<void> SaveableComponentAdapterRegistry::RequireMandatoryAdapters(const std::span<const Binding> bindings) {
        const auto missing = std::ranges::find_if(bindings, [](const Binding &binding) {
            return binding.requirement.presence == SaveableComponentPresence::Required && binding.adapter == nullptr;
        });
        if (missing == bindings.end())
            return Result<void>::Success();
        return Fail<void>(SceneErrors::SaveableComponentMissingAdapter,
                          std::format("Required saveable component '{}' has no adapter.", missing->requirement.type.Value()));
    }

    /** @copydoc SaveableComponentAdapterRegistry::Create */
    Result<SaveableComponentAdapterRegistry> SaveableComponentAdapterRegistry::Create(
        const Gameplay::ComponentRegistry &components, const std::span<const SaveableComponentRequirement> requirements,
        const std::span<const std::shared_ptr<const ISaveableComponentStateAdapter>> adapters, const std::size_t maximumAdapters) {
        if (!components.IsFrozen() || maximumAdapters == 0 || maximumAdapters > MaximumSaveableComponentAdapters ||
            requirements.size() > maximumAdapters || adapters.size() > maximumAdapters)
            return Fail<SaveableComponentAdapterRegistry>(SceneErrors::SaveableComponentInvalid);
        try {
            auto bindings = BuildBindings(components, requirements);
            if (bindings.HasError())
                return Result<SaveableComponentAdapterRegistry>::Failure(bindings.ErrorValue());
            auto bindingsValue = std::move(bindings).Value();
            if (auto bound = BindAdapters(components, adapters, bindingsValue); bound.HasError())
                return Result<SaveableComponentAdapterRegistry>::Failure(bound.ErrorValue());
            if (auto required = RequireMandatoryAdapters(bindingsValue); required.HasError())
                return Result<SaveableComponentAdapterRegistry>::Failure(required.ErrorValue());
            return Result<SaveableComponentAdapterRegistry>::Success(SaveableComponentAdapterRegistry{std::move(bindingsValue)});
        } catch (const std::bad_alloc &) {
            return Fail<SaveableComponentAdapterRegistry>(SceneErrors::SaveableComponentAllocationFailed);
        }
    }

    const SaveableComponentAdapterRegistry::Binding *SaveableComponentAdapterRegistry::Find(
        const Gameplay::ComponentTypeId &type) const noexcept {
        const auto found = std::ranges::lower_bound(bindings_, type, {}, [](const Binding &binding) {
            return binding.requirement.type;
        });
        return found == bindings_.end() || found->requirement.type != type ? nullptr : std::to_address(found);
    }

    Result<EntityRef> SaveableComponentAdapterRegistry::ResolveEntity(const PersistentEntityId entity,
                                                                      const PersistentEntityGeneration generation,
                                                                      const Gameplay::ComponentTypeId &type,
                                                                      const PersistentEntityIdentityMap &identities,
                                                                      const ISaveableComponentAuthority &authority) const {
        if (!type.IsValid())
            return Fail<EntityRef>(SceneErrors::SaveableComponentIdentityMismatch);
        auto resolved = identities.Resolve(entity, generation);
        if (resolved.HasError())
            return Result<EntityRef>::Failure(resolved.ErrorValue());
        if (!authority.Contains(resolved.Value(), type))
            return Fail<EntityRef>(SceneErrors::SaveableComponentIdentityMismatch,
                                   std::format("Entity does not own component '{}'.", type.Value()));
        return resolved;
    }

    /** @copydoc SaveableComponentAdapterRegistry::Capture */
    Result<std::optional<CapturedSaveableComponentState>> SaveableComponentAdapterRegistry::Capture(
        const EntityRef entity, const Gameplay::ComponentTypeId &type, const ISaveableComponentAuthority &authority) const {
        const Binding *binding = Find(type);
        if (binding == nullptr)
            return Fail<std::optional<CapturedSaveableComponentState>>(SceneErrors::SaveableComponentInvalid);
        if (!entity.IsValid() || !authority.Contains(entity, type))
            return Fail<std::optional<CapturedSaveableComponentState>>(SceneErrors::SaveableComponentIdentityMismatch);
        if (binding->adapter == nullptr)
            return Result<std::optional<CapturedSaveableComponentState>>::Success(std::nullopt);
        auto captured = binding->adapter->Capture(entity);
        if (captured.HasError())
            return Result<std::optional<CapturedSaveableComponentState>>::Failure(captured.ErrorValue());
        auto capturedValue = std::move(captured).Value();
        if (!capturedValue.has_value()) {
            if (binding->requirement.presence == SaveableComponentPresence::Required)
                return Fail<std::optional<CapturedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
            return Result<std::optional<CapturedSaveableComponentState>>::Success(std::nullopt);
        }
        if (const auto bytes = capturedValue->Bytes(); !IsPayloadWithinLimit(bytes, binding->descriptor.maximumPayloadBytes))
            return Fail<std::optional<CapturedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
        return Result<std::optional<CapturedSaveableComponentState>>::Success(
            CapturedSaveableComponentState{.schemaVersion = binding->descriptor.schemaVersion, .payload = std::move(*capturedValue)});
    }

    Result<CanonicalEncodedValue> SaveableComponentAdapterRegistry::MigratePayload(const ISaveableComponentStateAdapter &adapter,
                                                                                   const SavedComponentStateRecord &record,
                                                                                   const CanonicalCodecLimits &limits) {
        auto source = CanonicalValueReader::Create(record.payload, limits);
        if (source.HasError())
            return Fail<CanonicalEncodedValue>(SceneErrors::SaveableComponentPayloadInvalid);
        return adapter.Migrate(record.schemaVersion, std::move(source).Value());
    }

    Result<std::unique_ptr<IPreparedSaveableComponentState>> SaveableComponentAdapterRegistry::PrepareDecodedState(
        const ISaveableComponentStateAdapter &adapter, const EntityRef entity, const std::span<const std::byte> payload,
        const CanonicalCodecLimits &limits) {
        auto validationReader = CanonicalValueReader::Create(payload, limits);
        if (validationReader.HasError())
            return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
        if (auto validated = adapter.Validate(std::move(validationReader).Value()); validated.HasError())
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(validated.ErrorValue());
        auto applyReader = CanonicalValueReader::Create(payload, limits);
        if (applyReader.HasError())
            return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
        auto prepared = adapter.PrepareApply(entity, std::move(applyReader).Value());
        if (prepared.HasValue() && prepared.Value() == nullptr)
            return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
        return prepared;
    }

    /** @copydoc SaveableComponentAdapterRegistry::PrepareRestore */
    Result<std::unique_ptr<IPreparedSaveableComponentState>> SaveableComponentAdapterRegistry::PrepareRestore(
        const SavedComponentStateRecord &record, const PersistentEntityIdentityMap &identities,
        const ISaveableComponentAuthority &authority) const {
        const Binding *binding = Find(record.type);
        if (binding == nullptr)
            return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentInvalid);
        if (binding->adapter == nullptr)
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(nullptr);
        const auto &descriptor = binding->descriptor;
        if (auto valid = ValidateRestoreRecord(record, descriptor); valid.HasError())
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(valid.ErrorValue());
        auto entity = ResolveEntity(record.entity, record.generation, record.type, identities, authority);
        if (entity.HasError())
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(entity.ErrorValue());

        std::span<const std::byte> payload = record.payload;
        const auto limits = PayloadLimits(descriptor.maximumPayloadBytes);
        std::optional<CanonicalEncodedValue> migrated;
        if (record.schemaVersion < descriptor.schemaVersion) {
            auto migration = MigratePayload(*binding->adapter, record, limits);
            if (migration.HasError())
                return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(migration.ErrorValue());
            migrated.emplace(std::move(migration).Value());
            payload = migrated->Bytes();
            if (!IsPayloadWithinLimit(payload, descriptor.maximumPayloadBytes))
                return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
        }
        return PrepareDecodedState(*binding->adapter, entity.Value(), payload, limits);
    }

    /** @copydoc SaveableComponentAdapterRegistry::PrepareDefault */
    Result<std::unique_ptr<IPreparedSaveableComponentState>> SaveableComponentAdapterRegistry::PrepareDefault(
        const PersistentEntityId entity, const PersistentEntityGeneration generation, const Gameplay::ComponentTypeId &type,
        const PersistentEntityIdentityMap &identities, const ISaveableComponentAuthority &authority) const {
        const Binding *binding = Find(type);
        if (binding == nullptr)
            return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentInvalid);
        if (binding->adapter == nullptr)
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Success(nullptr);
        auto resolved = ResolveEntity(entity, generation, type, identities, authority);
        if (resolved.HasError())
            return Result<std::unique_ptr<IPreparedSaveableComponentState>>::Failure(resolved.ErrorValue());
        auto prepared = binding->adapter->PrepareDefault(resolved.Value());
        if (prepared.HasValue() && prepared.Value() == nullptr)
            return Fail<std::unique_ptr<IPreparedSaveableComponentState>>(SceneErrors::SaveableComponentPayloadInvalid);
        return prepared;
    }
}  // namespace Horo::Runtime
