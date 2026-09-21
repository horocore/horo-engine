#pragma once

/**
 * @file ComponentRegistry.h
 * @brief Transactional registry and editor-neutral inspection for project gameplay components.
 */

#include "Horo/Gameplay/Component.h"

#include <span>
#include <vector>

namespace Horo::Gameplay {
    /** @brief Compatibility state of one persistent payload against the active registry. */
    enum class ComponentInspectionStatus : std::uint8_t {
        Current,
        MigrationRequired,
        MissingDescriptor,
        UnsupportedOlderSchema,
        NewerSchema,
        InvalidEnvelope,
    };

    /** @brief Immutable inspection result suitable for headless tools or generic editor presentation. */
    struct ComponentInspection {
        ComponentInspectionStatus status{ComponentInspectionStatus::MissingDescriptor};
        const ComponentDescriptor *descriptor{};
        std::vector<ComponentMigrationDescriptor> migrationPath;
    };

    /** @brief Host-owned component registry frozen before any runtime scene activates. */
    class ComponentRegistry final {
    public:
        ComponentRegistry() = default;

        /**
         * @brief Copies one project component descriptor into the open registration transaction.
         * @param descriptor Complete stable serialization and authoring metadata.
         * @return Success or a typed validation, duplicate, or lifecycle error.
         */
        [[nodiscard]] Result<void> Register(ComponentDescriptor descriptor);

        /** @brief Sorts the complete descriptor snapshot and prevents further registration. */
        [[nodiscard]] Result<void> Freeze();
        /** @brief Reports whether registration is closed. */
        [[nodiscard]] bool IsFrozen() const noexcept;
        /** @brief Returns descriptors in deterministic type-ID order after freeze. */
        [[nodiscard]] std::span<const ComponentDescriptor> Descriptors() const noexcept;

        /**
         * @brief Finds one component descriptor by stable identity.
         * @param typeId Persistent component type identity.
         * @return Host-owned descriptor, or null when project code no longer provides it.
         */
        [[nodiscard]] const ComponentDescriptor *Find(const ComponentTypeId &typeId) const noexcept;

        /**
         * @brief Inspects compatibility without executing code or modifying the opaque payload.
         * @param component Persistent component envelope.
         * @return Current, missing, skewed, or migratable state with a deterministic migration path.
         */
        [[nodiscard]] Result<ComponentInspection> Inspect(const SerializedComponent &component) const;

    private:
        std::vector<ComponentDescriptor> descriptors_;
        bool frozen_{false};
    };
}  // namespace Horo::Gameplay
