#pragma once

/**
 * @file ReplicationRegistration.h
 * @brief Native gameplay ownership bindings for Network-owned replication contracts.
 */

#include "Horo/Gameplay/Behavior.h"
#include "Horo/Gameplay/GameplayRegistration.h"
#include "Horo/Network/ReplicationDescriptorRegistry.h"
#include "Horo/Network/ReplicationSerializer.h"

#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Gameplay::Detail {
    struct GenerationLeaseBinding;
}

namespace Horo::Gameplay {
    inline constexpr std::size_t MaximumGameplayReplicationRegistrations = 512;

    /** @brief Existing gameplay identity that owns one replication schema and its safe points. */
    using GameplayReplicationOwner = std::variant<ComponentTypeId, BehaviorTypeId, GameplayServiceId>;

    /** @brief Explicit owner-thread schedule and component access for capture and apply callbacks. */
    struct GameplayReplicationSchedule final {
        GameplaySystemPhase capturePhase{GameplaySystemPhase::Gameplay};       /**< Fixed phase after the owner's committed mutation. */
        GameplaySystemPhase applyPhase{GameplaySystemPhase::PrePhysics};       /**< Fixed phase where staged remote state may commit. */
        GameplayThreadAffinity affinity{GameplayThreadAffinity::RuntimeOwner}; /**< Required mutation/read affinity. */
        GameplayComponentAccessSet captureAccess; /**< Components read while producing canonical network values. */
        GameplayComponentAccessSet applyAccess;   /**< Components read/written while committing staged values. */
    };

    /** @brief One native gameplay owner paired with a Network schema and exact typed serializers. */
    struct GameplayReplicationRegistration final {
        GameplayReplicationOwner owner;              /**< Existing component, behavior, or service identity. */
        Network::ReplicationSchemaDescriptor schema; /**< Inert Network-owned schema metadata. */
        std::vector<std::shared_ptr<const Network::IReplicationFieldSerializer>> serializers; /**< Exact-generation codecs. */
        GameplayReplicationSchedule schedule;                                                 /**< Owner-safe capture/apply boundary. */
    };

    /** @brief Immutable owner/schema/schedule binding without a copyable native-code adapter reference. */
    struct GameplayReplicationBinding final {
        GameplayReplicationOwner owner;       /**< Existing component, behavior, or service identity. */
        Network::ReplicationSchemaId schema;  /**< Schema published in the pinned Network generation. */
        GameplayReplicationSchedule schedule; /**< Owner-safe capture/apply boundary. */
    };

    /** @brief Finite host construction envelope for one gameplay replication generation. */
    struct GameplayReplicationRegistryLimits final {
        std::size_t maximumRegistrations{MaximumGameplayReplicationRegistrations};
        Network::ReplicationDescriptorLimits descriptors{512, 256, 160, 1024 * 1024, 16 * 1024 * 1024};
        Network::ReplicationSerializerRegistryLimits serializers{};
    };

    /** @brief Immutable generation lease exposing validated Network registries while native code remains loaded. */
    class GameplayReplicationLease final {
    public:
        /**
         * @brief Returns the pinned canonical schema generation.
         * @return Non-null immutable schema snapshot for this lease.
         */
        [[nodiscard]] const Network::ReplicationDescriptorSnapshotPtr &Descriptors() const noexcept;
        /**
         * @brief Returns the schema-bound typed serializer generation.
         * @return Immutable serializer registry whose adapters are pinned by this lease.
         */
        [[nodiscard]] const Network::ReplicationSerializerRegistry &Serializers() const noexcept;
        /**
         * @brief Returns canonicalized gameplay owner/schedule bindings in schema order.
         * @return Lease-owned immutable bindings without independently copyable native adapters.
         */
        [[nodiscard]] std::span<const GameplayReplicationBinding> Registrations() const noexcept;
        /**
         * @brief Reports whether this value owns a complete published generation.
         * @return True while the immutable state and native module generation are pinned.
         */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        struct State;
        friend class ReplicationRegistrationRegistry;
        GameplayReplicationLease(std::shared_ptr<void> generationLease, std::shared_ptr<const State> state) noexcept;

        std::shared_ptr<void> generationLease_;
        std::shared_ptr<const State> state_;
    };

    /** @brief Transactional native gameplay replication registry; it does not replace gameplay owner registries. */
    class ReplicationRegistrationRegistry final {
    public:
        /**
         * @brief Creates an open registry restricted to one project module namespace.
         * @param moduleId Canonical project module identity owning all declarations.
         * @param limits Finite host construction envelope.
         */
        explicit ReplicationRegistrationRegistry(std::string moduleId, GameplayReplicationRegistryLimits limits = {});

        /**
         * @brief Copies one inert owner/schema/serializer contribution into the open transaction.
         * @param registration Complete project-owned replication declaration.
         * @return Success or a typed validation, duplicate, capacity, or lifecycle error.
         */
        [[nodiscard]] Result<void> Register(GameplayReplicationRegistration registration);

        /**
         * @brief Resolves every owner/access identity and publishes one immutable Network generation.
         * @param components Frozen canonical component descriptors.
         * @param behaviors Frozen generated behavior registrations.
         * @param services Frozen service registrations.
         * @return Success or a typed owner, schedule, schema, serializer, or lifecycle error.
         */
        [[nodiscard]] Result<void> Freeze(std::span<const ComponentDescriptor> components, std::span<const BehaviorRegistration> behaviors,
                                          std::span<const GameplayServiceRegistration> services);

        /**
         * @brief Reports whether registration is closed and validation completed.
         * @return True after a successful freeze, including an empty declaration set.
         */
        [[nodiscard]] bool IsFrozen() const noexcept;

        /**
         * @brief Acquires a generation pin before native module unload admission closes.
         * @return Immutable lease, invalid-registration for an empty/unfrozen registry, or restart-required while retiring.
         */
        [[nodiscard]] Result<GameplayReplicationLease> Acquire() const;

    private:
        friend struct Detail::GenerationLeaseBinding;

        std::string moduleId_;
        GameplayReplicationRegistryLimits limits_;
        std::vector<GameplayReplicationRegistration> registrations_;
        std::shared_ptr<const GameplayReplicationLease::State> state_;
        std::weak_ptr<void> generationLease_;
        const std::atomic_bool *generationLeaseAdmission_{};
        bool frozen_{false};
    };
}  // namespace Horo::Gameplay
