#include "Horo/Gameplay/PersistenceRegistration.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Checks the single-record canonical shape independently of the gameplay owner. */
        [[nodiscard]] bool HasDurableRecordShape(const GameplayPersistenceDescriptor &descriptor) noexcept {
            const auto &participant = descriptor.participant;
            return participant.participant.IsValid() && participant.schemaVersion.IsValid() &&
                   participant.roles == (SaveParticipantRole::Capture | SaveParticipantRole::Restore) && descriptor.record.IsValid() &&
                   participant.ownedRecords.size() == 1 && participant.ownedRecords.front() == descriptor.record;
        }

        /** @brief Checks finite envelope capacity and exact module compatibility metadata. */
        [[nodiscard]] bool HasDurableEnvelope(const GameplayPersistenceDescriptor &descriptor) noexcept {
            constexpr std::uint64_t prefixBytes = 10;
            return descriptor.moduleId.IsValid() && descriptor.moduleVersion != 0 &&
                   descriptor.participant.limits.maximumRecordCount == 1 && descriptor.participant.limits.maximumNestingDepth != 0 &&
                   descriptor.participant.limits.maximumPayloadBytes >= prefixBytes + descriptor.moduleId.Value().size();
        }

        /** @brief Checks which canonical scopes can host each supported gameplay authority. */
        [[nodiscard]] bool HasDurableOwnerScope(const GameplayPersistenceDescriptor &descriptor) noexcept {
            using enum GameplayPersistenceOwner;
            using enum SaveParticipantScope;
            return descriptor.owner >= BehaviorInstance && descriptor.owner <= Session && descriptor.participant.scope >= RuntimeScene &&
                   descriptor.participant.scope <= PersistentWorld &&
                   (descriptor.owner != BehaviorInstance || descriptor.participant.scope == RuntimeScene);
        }
    }  // namespace

    /** @copydoc IsValidGameplayPersistenceDescriptor */
    bool IsValidGameplayPersistenceDescriptor(const GameplayPersistenceDescriptor &descriptor) noexcept {
        return HasDurableRecordShape(descriptor) && HasDurableEnvelope(descriptor) && HasDurableOwnerScope(descriptor);
    }
}  // namespace Horo::Runtime

namespace Horo::Gameplay {
    namespace {
        /** @brief Checks that a declaration names the identity type required by its authority. */
        [[nodiscard]] bool HasMatchingOwnerShape(const PersistenceRegistration &registration) noexcept {
            using enum Runtime::GameplayPersistenceOwner;
            switch (registration.descriptor.owner) {
                case BehaviorInstance:
                    return std::holds_alternative<BehaviorTypeId>(registration.ownerIdentity);
                case Service:
                    return std::holds_alternative<GameplayServiceId>(registration.ownerIdentity);
                case ModuleGlobal:
                case Session:
                    return std::holds_alternative<std::monostate>(registration.ownerIdentity);
            }
            return false;
        }

        /** @brief Checks semantic owner identities against the exact module namespace. */
        [[nodiscard]] bool HasMatchingOwnerNamespace(const PersistenceOwnerIdentity &owner, const std::string &moduleId) {
            return std::visit([&]<typename Identity>(const Identity &identity) {
                if constexpr (std::is_same_v<Identity, std::monostate>)
                    return true;
                else {
                    const auto &value = identity.Value();
                    return identity.IsValid() && value.size() > moduleId.size() && value.starts_with(moduleId) &&
                           value[moduleId.size()] == '.';
                }
            }, owner);
        }

        /** @brief Validates local module authority without activation or ambient registry access. */
        [[nodiscard]] bool IsLocalRegistrationValid(const PersistenceRegistration &registration, const std::string &moduleId) {
            return Runtime::IsValidGameplayPersistenceDescriptor(registration.descriptor) && registration.source &&
                   registration.descriptor.moduleId.Value() == moduleId && HasMatchingOwnerShape(registration) &&
                   HasMatchingOwnerNamespace(registration.ownerIdentity, moduleId);
        }

        /** @brief Rejects conflicting participant and record authority before insertion. */
        [[nodiscard]] bool HasDuplicateAuthority(const std::span<const PersistenceRegistration> registrations,
                                                 const Runtime::GameplayPersistenceDescriptor &descriptor) {
            return std::ranges::any_of(registrations, [&](const PersistenceRegistration &existing) {
                return existing.descriptor.participant.participant == descriptor.participant.participant ||
                       existing.descriptor.record == descriptor.record;
            });
        }
    }  // namespace

    /** @copydoc PersistenceRegistrationRegistry::PersistenceRegistrationRegistry */
    PersistenceRegistrationRegistry::PersistenceRegistrationRegistry(std::string moduleId) : moduleId_(std::move(moduleId)) {}

    /** @copydoc PersistenceRegistrationRegistry::Register */
    Result<void> PersistenceRegistrationRegistry::Register(PersistenceRegistration registration) {
        using namespace Runtime;
        if (frozen_)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        if (!IsLocalRegistrationValid(registration, moduleId_) || registrations_.size() >= MaximumSaveParticipantCount)
            return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
        if (auto valid = ValidateCanonicalStateParticipantDescriptor(registration.descriptor.participant); valid.HasError())
            return valid;
        if (HasDuplicateAuthority(registrations_, registration.descriptor))
            return Result<void>::Failure(MakeError(SaveErrors::ParticipantDescriptorInvalid));
        try {
            registrations_.push_back(std::move(registration));
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::ParticipantRegistryAllocationFailed));
        }
    }

    /** @copydoc PersistenceRegistrationRegistry::Freeze */
    Result<void> PersistenceRegistrationRegistry::Freeze(const std::span<const BehaviorRegistration> behaviors,
                                                         const std::span<const GameplayServiceRegistration> services) {
        for (const auto &registration : registrations_) {
            if (const auto *behavior = std::get_if<BehaviorTypeId>(&registration.ownerIdentity)) {
                if (!behavior->IsValid() || std::ranges::find(behaviors, *behavior, [](const BehaviorRegistration &value) {
                    return value.descriptor.typeId;
                }) == behaviors.end())
                    return Result<void>::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
            }
            if (const auto *service = std::get_if<GameplayServiceId>(&registration.ownerIdentity)) {
                if (!service->IsValid() || std::ranges::find(services, *service, [](const GameplayServiceRegistration &value) {
                    return value.descriptor.id;
                }) == services.end())
                    return Result<void>::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
            }
        }
        frozen_ = true;
        return Result<void>::Success();
    }

    /** @copydoc PersistenceRegistrationRegistry::IsFrozen */
    bool PersistenceRegistrationRegistry::IsFrozen() const noexcept {
        return frozen_;
    }
}  // namespace Horo::Gameplay
