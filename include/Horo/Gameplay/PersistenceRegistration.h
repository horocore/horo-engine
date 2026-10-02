#pragma once

/** @file PersistenceRegistration.h
 * @brief Inert native gameplay declarations for opt-in durable runtime state.
 */

#include "Horo/Gameplay/Behavior.h"
#include "Horo/Gameplay/GameplayRegistration.h"
#include "Horo/Gameplay/PersistenceSource.h"
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"

#include <variant>

namespace Horo::Runtime {
    /** @brief Authority owning one explicitly durable semantic record. */
    enum class GameplayPersistenceOwner : std::uint8_t {
        BehaviorInstance,
        ModuleGlobal,
        Service,
        Session
    };

    /** @brief Inert runtime schema, independent of authoring fields and native reload state. */
    struct GameplayPersistenceDescriptor final {
        CanonicalStateParticipantDescriptor participant;
        SaveRecordId record;
        GameplayPersistenceOwner owner{GameplayPersistenceOwner::BehaviorInstance};
        SaveParticipantId moduleId;
        std::uint32_t moduleVersion{};
    };

    /** @brief Validates local metadata without invoking project code or registering state.
     * @param descriptor Stable schema, owner and finite record envelope.
     * @return True for a supported declaration; aggregate graph validation remains host-owned.
     */
    [[nodiscard]] bool IsValidGameplayPersistenceDescriptor(const GameplayPersistenceDescriptor &descriptor) noexcept;
}  // namespace Horo::Runtime

namespace Horo::Gameplay {
    class LoadedGameModule;

    /** @brief Existing gameplay identity, or no identity for module-global/session state. */
    using PersistenceOwnerIdentity = std::variant<std::monostate, BehaviorTypeId, GameplayServiceId>;

    /** @brief Explicit descriptor and runtime-only source bound to an existing semantic owner. */
    struct PersistenceRegistration final {
        Runtime::GameplayPersistenceDescriptor descriptor;
        PersistenceOwnerIdentity ownerIdentity;
        std::shared_ptr<IPersistenceSource> source;
    };

    /** @brief Host-owned registration transaction; project sources never escape without a generation pin. */
    class PersistenceRegistrationRegistry final {
    public:
        /** @brief Creates an empty module-scoped transaction. @param moduleId Exact loaded module identity. */
        explicit PersistenceRegistrationRegistry(std::string moduleId);
        /** @brief Copies a durable declaration without capture or activation.
         * @param registration Explicit metadata, owner and source for this generation.
         * @return Success or typed invalid, duplicate, capacity or frozen error.
         */
        [[nodiscard]] Result<void> Register(PersistenceRegistration registration);
        /** @brief Resolves generated behavior and registered service identities before module startup.
         * @param behaviors Frozen generated behavior registrations.
         * @param services Frozen project service registrations.
         * @return Success or a typed missing-owner error; no source callback is invoked.
         */
        [[nodiscard]] Result<void> Freeze(std::span<const BehaviorRegistration> behaviors,
                                          std::span<const GameplayServiceRegistration> services);
        /** @brief Reports successful descriptor freeze. @return Current transaction state. */
        [[nodiscard]] bool IsFrozen() const noexcept;

    private:
        friend class LoadedGameModule;
        std::string moduleId_;
        std::vector<PersistenceRegistration> registrations_;
        bool frozen_{};
    };
}  // namespace Horo::Gameplay
