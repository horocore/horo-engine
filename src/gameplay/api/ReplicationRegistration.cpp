#include "Horo/Gameplay/ReplicationRegistration.h"

#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Horo::Gameplay {
    struct GameplayReplicationLease::State final {
        State(Network::ReplicationDescriptorSnapshotPtr descriptorsValue, Network::ReplicationSerializerRegistry serializersValue,
              std::vector<GameplayReplicationBinding> registrationsValue)
            : descriptors(std::move(descriptorsValue)), serializers(std::move(serializersValue)),
              registrations(std::move(registrationsValue)) {}

        Network::ReplicationDescriptorSnapshotPtr descriptors;
        Network::ReplicationSerializerRegistry serializers;
        std::vector<GameplayReplicationBinding> registrations;
    };

    namespace {
        template <typename T> [[nodiscard]] Result<T> Fail(const ErrorCodeDescriptor &code) {
            return Result<T>::Failure(MakeError(code));
        }

        [[nodiscard]] bool HasModulePrefix(const std::string_view identity, const std::string_view moduleId) noexcept {
            return identity.size() > moduleId.size() && identity.starts_with(moduleId) && identity[moduleId.size()] == '.';
        }

        [[nodiscard]] bool ValidPhaseSchedule(const GameplayReplicationSchedule &schedule) noexcept {
            using enum GameplaySystemPhase;
            const bool validCapture = schedule.capturePhase == PostPhysics || schedule.capturePhase == Gameplay;
            const bool validApply = schedule.applyPhase == PrePhysics || schedule.applyPhase == Gameplay;
            return validCapture && validApply && schedule.affinity == GameplayThreadAffinity::RuntimeOwner &&
                   schedule.captureAccess.writes.empty();
        }

        [[nodiscard]] const ComponentDescriptor *FindComponent(const std::span<const ComponentDescriptor> components,
                                                               const ComponentTypeId &id) noexcept {
            const auto found = std::ranges::find(components, id, &ComponentDescriptor::typeId);
            return found == components.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool HasBehavior(const std::span<const BehaviorRegistration> behaviors, const BehaviorTypeId &id) noexcept {
            return std::ranges::find(behaviors, id, [](const BehaviorRegistration &registration) {
                return registration.descriptor.typeId;
            }) != behaviors.end();
        }

        [[nodiscard]] bool HasService(const std::span<const GameplayServiceRegistration> services, const GameplayServiceId &id) noexcept {
            return std::ranges::find(services, id, [](const GameplayServiceRegistration &registration) {
                return registration.descriptor.id;
            }) != services.end();
        }

        [[nodiscard]] bool AccessExists(const GameplayComponentAccessSet &access,
                                        const std::span<const ComponentDescriptor> components) noexcept {
            if (access.reads.size() > MaximumGameplayComponentAccesses || access.writes.size() > MaximumGameplayComponentAccesses ||
                access.reads.size() + access.writes.size() > MaximumGameplayComponentAccesses)
                return false;
            if (const auto valid = [components](const ComponentTypeId &id) {
                return id.IsValid() && FindComponent(components, id) != nullptr;
            }; !std::ranges::all_of(access.reads, valid) || !std::ranges::all_of(access.writes, valid))
                return false;
            const auto hasDuplicates = [](const std::vector<ComponentTypeId> &ids) {
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    if (std::ranges::find(ids.begin() + static_cast<std::ptrdiff_t>(index + 1), ids.end(), ids[index]) != ids.end())
                        return true;
                }
                return false;
            };
            return !hasDuplicates(access.reads) && !hasDuplicates(access.writes);
        }

        [[nodiscard]] bool ValidOwner(const GameplayReplicationRegistration &registration,
                                      const std::span<const ComponentDescriptor> components,
                                      const std::span<const BehaviorRegistration> behaviors,
                                      const std::span<const GameplayServiceRegistration> services) noexcept {
            return std::visit([&]<typename Owner>(const Owner &owner) {
                if constexpr (std::is_same_v<Owner, ComponentTypeId>) {
                    return owner.IsValid() && FindComponent(components, owner) != nullptr &&
                           std::ranges::find(registration.schedule.captureAccess.reads, owner) !=
                               registration.schedule.captureAccess.reads.end() &&
                           std::ranges::find(registration.schedule.applyAccess.writes, owner) !=
                               registration.schedule.applyAccess.writes.end();
                } else if constexpr (std::is_same_v<Owner, BehaviorTypeId>) {
                    return owner.IsValid() && HasBehavior(behaviors, owner);
                } else {
                    return owner.IsValid() && HasService(services, owner);
                }
            }, registration.owner);
        }

        [[nodiscard]] auto SerializerKey(const Network::ReplicationSerializerDescriptor &descriptor) noexcept {
            return std::tuple{descriptor.owner.value, descriptor.valueType.Value(), descriptor.codec.Value()};
        }

        [[nodiscard]] Result<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>> CollectSerializers(
            const std::span<const GameplayReplicationRegistration> registrations) {
            std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>> result;
            for (const GameplayReplicationRegistration &registration : registrations) {
                for (const auto &serializer : registration.serializers) {
                    if (serializer == nullptr)
                        return Fail<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>>(
                            GameplayErrors::InvalidReplicationRegistration);
                    const auto key = SerializerKey(serializer->Descriptor());
                    const auto existing = std::ranges::find_if(result, [&](const auto &candidate) {
                        return SerializerKey(candidate->Descriptor()) == key;
                    });
                    if (existing == result.end())
                        result.push_back(serializer);
                    else if (existing->get() != serializer.get())
                        return Fail<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>>(
                            Network::NetworkErrors::ReplicationSerializerConflict);
                }
            }
            return Result<std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>>>::Success(std::move(result));
        }

        [[nodiscard]] Result<void> ValidateResolvedRegistrations(const std::span<const GameplayReplicationRegistration> registrations,
                                                                 const std::span<const ComponentDescriptor> components,
                                                                 const std::span<const BehaviorRegistration> behaviors,
                                                                 const std::span<const GameplayServiceRegistration> services) {
            for (const GameplayReplicationRegistration &registration : registrations) {
                if (!AccessExists(registration.schedule.captureAccess, components) ||
                    !AccessExists(registration.schedule.applyAccess, components) ||
                    !ValidOwner(registration, components, behaviors, services))
                    return Fail<void>(GameplayErrors::ReplicationOwnerMissing);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> StoreRegistration(std::vector<GameplayReplicationRegistration> &registrations,
                                                     GameplayReplicationRegistration registration) {
            try {
                registrations.push_back(std::move(registration));
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                return Fail<void>(GameplayErrors::InvalidReplicationRegistration);
            }
        }
    }  // namespace

    GameplayReplicationLease::GameplayReplicationLease(std::shared_ptr<void> generationLease, std::shared_ptr<const State> state) noexcept
        : generationLease_(std::move(generationLease)), state_(std::move(state)) {}

    /** @copydoc GameplayReplicationLease::Descriptors */
    const Network::ReplicationDescriptorSnapshotPtr &GameplayReplicationLease::Descriptors() const noexcept {
        return state_->descriptors;
    }

    /** @copydoc GameplayReplicationLease::Serializers */
    const Network::ReplicationSerializerRegistry &GameplayReplicationLease::Serializers() const noexcept {
        return state_->serializers;
    }

    /** @copydoc GameplayReplicationLease::Registrations */
    std::span<const GameplayReplicationBinding> GameplayReplicationLease::Registrations() const noexcept {
        return state_->registrations;
    }

    /** @copydoc GameplayReplicationLease::IsValid */
    bool GameplayReplicationLease::IsValid() const noexcept {
        return state_ != nullptr;
    }

    /** @copydoc ReplicationRegistrationRegistry::ReplicationRegistrationRegistry */
    ReplicationRegistrationRegistry::ReplicationRegistrationRegistry(std::string moduleId, const GameplayReplicationRegistryLimits &limits)
        : moduleId_(std::move(moduleId)), limits_(limits) {}

    /** @copydoc ReplicationRegistrationRegistry::Register */
    Result<void> ReplicationRegistrationRegistry::Register(GameplayReplicationRegistration registration) {
        if (frozen_)
            return Fail<void>(GameplayErrors::ReplicationRegistryFrozen);
        if (registrations_.size() >= limits_.maximumRegistrations)
            return Fail<void>(GameplayErrors::InvalidReplicationRegistration);
        if (!HasModulePrefix(std::visit(
                                 [](const auto &owner) -> std::string_view {
            return owner.Value();
        }, registration.owner),
                             moduleId_) ||
            registration.schema.owner.value != moduleId_ || !ValidPhaseSchedule(registration.schedule) || registration.serializers.empty())
            return Fail<void>(GameplayErrors::InvalidReplicationRegistration);
        if (const Result<void> valid = Network::ValidateReplicationSchemaDescriptor(registration.schema, limits_.descriptors);
            valid.HasError())
            return valid;
        if (std::ranges::find(registrations_, registration.schema.id, [](const GameplayReplicationRegistration &value) {
            return value.schema.id;
        }) != registrations_.end())
            return Fail<void>(GameplayErrors::DuplicateReplicationSchema);
        return StoreRegistration(registrations_, std::move(registration));
    }

    /** @copydoc ReplicationRegistrationRegistry::Freeze */
    Result<void> ReplicationRegistrationRegistry::Freeze(const std::span<const ComponentDescriptor> components,
                                                         const std::span<const BehaviorRegistration> behaviors,
                                                         const std::span<const GameplayServiceRegistration> services) {
        if (frozen_)
            return Result<void>::Success();
        if (registrations_.empty()) {
            frozen_ = true;
            return Result<void>::Success();
        }
        if (const Result<void> valid = ValidateResolvedRegistrations(registrations_, components, behaviors, services); valid.HasError())
            return valid;
        try {
            std::vector<Network::ReplicationSchemaDescriptor> schemas;
            schemas.reserve(registrations_.size());
            for (const GameplayReplicationRegistration &registration : registrations_)
                schemas.push_back(registration.schema);
            auto descriptors = Network::BuildReplicationDescriptorSnapshot(schemas, limits_.descriptors);
            if (descriptors.HasError())
                return Result<void>::Failure(descriptors.ErrorValue());
            auto serializers = CollectSerializers(registrations_);
            if (serializers.HasError())
                return Result<void>::Failure(serializers.ErrorValue());
            auto serializerRegistry =
                Network::ReplicationSerializerRegistry::Create(descriptors.Value(), serializers.Value(), limits_.serializers);
            if (serializerRegistry.HasError())
                return Result<void>::Failure(serializerRegistry.ErrorValue());
            std::ranges::sort(registrations_, {}, [](const GameplayReplicationRegistration &registration) {
                return registration.schema.id;
            });
            std::vector<GameplayReplicationBinding> bindings;
            bindings.reserve(registrations_.size());
            for (const GameplayReplicationRegistration &registration : registrations_)
                bindings.emplace_back(registration.owner, registration.schema.id, registration.schedule);
            state_ = std::make_shared<GameplayReplicationLease::State>(std::move(descriptors).Value(),
                                                                       std::move(serializerRegistry).Value(), std::move(bindings));
            frozen_ = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Fail<void>(GameplayErrors::InvalidReplicationRegistration);
        }
    }

    /** @copydoc ReplicationRegistrationRegistry::IsFrozen */
    bool ReplicationRegistrationRegistry::IsFrozen() const noexcept {
        return frozen_;
    }

    /** @copydoc ReplicationRegistrationRegistry::Acquire */
    Result<GameplayReplicationLease> ReplicationRegistrationRegistry::Acquire() const {
        if (!frozen_ || state_ == nullptr)
            return Fail<GameplayReplicationLease>(GameplayErrors::InvalidReplicationRegistration);
        std::shared_ptr<void> generationLease;
        if (generationLeaseAdmission_ != nullptr) {
            generationLease = generationLease_.lock();
            if (generationLease == nullptr || !generationLeaseAdmission_->load(std::memory_order_acquire))
                return Fail<GameplayReplicationLease>(GameplayErrors::GameplayReloadRestartRequired);
        }
        return Result<GameplayReplicationLease>::Success(GameplayReplicationLease{std::move(generationLease), state_});
    }
}  // namespace Horo::Gameplay
