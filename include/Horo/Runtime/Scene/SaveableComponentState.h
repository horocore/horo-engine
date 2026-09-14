#pragma once

/**
 * @file SaveableComponentState.h
 * @brief Canonical gameplay component save descriptors, adapters, and validated Scene restore routing.
 */

#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Hard composition bound for saveable component adapter entries. */
    inline constexpr std::size_t MaximumSaveableComponentAdapters = 4096;

    /** @brief Whether a declared component save adapter is mandatory for Scene activation. */
    enum class SaveableComponentPresence : std::uint8_t {
        Optional,
        Required
    };

    /** @brief Host declaration of one component type's save participation requirement. */
    struct SaveableComponentRequirement final {
        Gameplay::ComponentTypeId type;                                          /**< Stable component type identity. */
        SaveableComponentPresence presence{SaveableComponentPresence::Optional}; /**< Missing-adapter policy. */
    };

    /** @brief Inert versioned metadata for one canonical component state adapter. */
    struct SaveableComponentAdapterDescriptor final {
        Gameplay::ComponentTypeId type;    /**< Stable component type owned by the adapter. */
        std::uint32_t schemaVersion{1};    /**< Current non-zero canonical payload schema. */
        std::size_t maximumPayloadBytes{}; /**< Finite payload admission bound. */
    };

    /** @brief One captured runtime override with its adapter-owned schema identity. */
    struct CapturedSaveableComponentState final {
        std::uint32_t schemaVersion{1}; /**< Adapter-owned runtime save schema, independent of authoring serialization. */
        CanonicalEncodedValue payload;  /**< Bounded canonical payload, never native component memory. */
    };

    /** @brief Durable canonical component state addressed only through stable entity and type identities. */
    struct SavedComponentStateRecord final {
        PersistentEntityId entity;             /**< Stable entity identity; never an ECS slot. */
        PersistentEntityGeneration generation; /**< Exact durable entity incarnation. */
        Gameplay::ComponentTypeId type;        /**< Stable component type identity. */
        std::uint32_t schemaVersion{1};        /**< Encoded canonical schema version. */
        std::vector<std::byte> payload;        /**< Bounded canonical adapter payload, never raw component memory. */
    };

    /** @brief Inactive adapter-owned component candidate with a no-fail publication transfer. */
    class IPreparedSaveableComponentState {
    public:
        virtual ~IPreparedSaveableComponentState() = default;
        /** @brief Publishes the already validated candidate at the Scene lifecycle commit boundary. */
        virtual void PublishPrepared() noexcept = 0;

    protected:
        IPreparedSaveableComponentState() = default;
    };

    /** @brief Runtime component authority used only to validate entity/type ownership before adapter calls. */
    class ISaveableComponentAuthority {
    public:
        virtual ~ISaveableComponentAuthority() = default;
        /**
         * @brief Reports whether an exact runtime entity owns the declared component type.
         * @param entity Valid runtime Scene and entity identity.
         * @param type Stable component type identity.
         * @return True only when the authority currently owns the exact pair.
         */
        [[nodiscard]] virtual bool Contains(EntityRef entity, const Gameplay::ComponentTypeId &type) const noexcept = 0;

    protected:
        ISaveableComponentAuthority() = default;
    };

    /** @brief Semantic owner adapter for canonical capture, migration, validation, defaults, and staged application. */
    class ISaveableComponentStateAdapter {
    public:
        virtual ~ISaveableComponentStateAdapter() = default;
        /** @brief Returns inert type, schema, and payload-bound metadata. @return Immutable adapter metadata. */
        [[nodiscard]] virtual const SaveableComponentAdapterDescriptor &Descriptor() const noexcept = 0;
        /**
         * @brief Captures canonical state or explicitly omits state without exposing native layout.
         * @param entity Validated runtime entity that owns this adapter's component type.
         * @return Canonical payload, explicit omission, or a semantic capture error.
         */
        [[nodiscard]] virtual Result<std::optional<CanonicalEncodedValue>> Capture(EntityRef entity) const = 0;
        /**
         * @brief Migrates one older bounded canonical payload to the descriptor schema.
         * @param sourceSchema Non-zero source runtime-save schema version.
         * @param source Call-scoped reader over validated bounded input.
         * @return Canonical current-schema payload or a semantic migration error.
         */
        [[nodiscard]] virtual Result<CanonicalEncodedValue> Migrate(std::uint32_t sourceSchema, CanonicalValueReader source) const = 0;
        /**
         * @brief Validates one current-schema canonical payload without mutating runtime state.
         * @param source Call-scoped reader over the current-schema payload.
         * @return Success or a semantic payload validation error.
         */
        [[nodiscard]] virtual Result<void> Validate(CanonicalValueReader source) const = 0;
        /**
         * @brief Builds an inactive current-schema component candidate for one validated runtime entity.
         * @param entity Validated runtime entity that owns this adapter's component type.
         * @param source Call-scoped reader over the validated current-schema payload.
         * @return Non-null inactive candidate or a semantic preparation error.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareApply(EntityRef entity,
                                                                                                    CanonicalValueReader source) const = 0;
        /**
         * @brief Builds the adapter's inactive runtime default when no saved override exists.
         * @param entity Validated runtime entity that owns this adapter's component type.
         * @return Non-null inactive default candidate or a semantic preparation error.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareDefault(EntityRef entity) const = 0;

    protected:
        ISaveableComponentStateAdapter() = default;
    };

    /** @brief Immutable host-composed adapter table that validates every entity/type route before invocation. */
    class SaveableComponentAdapterRegistry final {
    public:
        /**
         * @brief Validates requirements and adapter leases against the frozen gameplay component registry.
         * @param components Frozen gameplay component identity registry.
         * @param requirements Complete host-declared required and optional save participation set.
         * @param adapters Adapter leases to snapshot into the immutable registry.
         * @param maximumAdapters Non-zero bounded admission limit.
         * @return Immutable registry or a typed composition error that must block activation.
         */
        [[nodiscard]] static Result<SaveableComponentAdapterRegistry> Create(
            const Gameplay::ComponentRegistry &components, std::span<const SaveableComponentRequirement> requirements,
            std::span<const std::shared_ptr<const ISaveableComponentStateAdapter>> adapters,
            std::size_t maximumAdapters = MaximumSaveableComponentAdapters);

        /**
         * @brief Captures one validated entity/type pair; absent optional adapters return an omitted value.
         * @param entity Runtime entity proposed for capture.
         * @param type Stable component type proposed for capture.
         * @param authority Runtime component ownership authority.
         * @return Adapter-owned schema and canonical payload, explicit omission, or a typed error.
         */
        [[nodiscard]] Result<std::optional<CapturedSaveableComponentState>> Capture(EntityRef entity, const Gameplay::ComponentTypeId &type,
                                                                                    const ISaveableComponentAuthority &authority) const;

        /**
         * @brief Prepares a saved override after stable entity, generation, type, schema, and payload validation.
         * @param record Untrusted durable component state envelope.
         * @param identities Immutable stable-to-runtime entity identity map.
         * @param authority Runtime component ownership authority.
         * @return Inactive candidate, explicit optional absence, or a typed restore error.
         */
        [[nodiscard]] Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareRestore(
            const SavedComponentStateRecord &record, const PersistentEntityIdentityMap &identities,
            const ISaveableComponentAuthority &authority) const;

        /**
         * @brief Prepares the runtime default for one validated entity/type pair with no saved override.
         * @param entity Stable persistent entity identity.
         * @param generation Exact durable entity incarnation.
         * @param type Stable component type identity.
         * @param identities Immutable stable-to-runtime entity identity map.
         * @param authority Runtime component ownership authority.
         * @return Inactive default candidate, explicit optional absence, or a typed restore error.
         */
        [[nodiscard]] Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareDefault(
            PersistentEntityId entity, PersistentEntityGeneration generation, const Gameplay::ComponentTypeId &type,
            const PersistentEntityIdentityMap &identities, const ISaveableComponentAuthority &authority) const;

    private:
        struct Binding final {
            SaveableComponentRequirement requirement;
            SaveableComponentAdapterDescriptor descriptor;
            std::shared_ptr<const ISaveableComponentStateAdapter> adapter;
        };

        explicit SaveableComponentAdapterRegistry(std::vector<Binding> bindings) noexcept;
        [[nodiscard]] static Result<std::vector<Binding>> BuildBindings(const Gameplay::ComponentRegistry &components,
                                                                        std::span<const SaveableComponentRequirement> requirements);
        [[nodiscard]] static Result<void> BindAdapters(const Gameplay::ComponentRegistry &components,
                                                       std::span<const std::shared_ptr<const ISaveableComponentStateAdapter>> adapters,
                                                       std::vector<Binding> &bindings);
        [[nodiscard]] static Result<void> RequireMandatoryAdapters(std::span<const Binding> bindings);
        [[nodiscard]] static Result<CanonicalEncodedValue> MigratePayload(const ISaveableComponentStateAdapter &adapter,
                                                                          const SavedComponentStateRecord &record,
                                                                          const CanonicalCodecLimits &limits);
        [[nodiscard]] static Result<std::unique_ptr<IPreparedSaveableComponentState>> PrepareDecodedState(
            const ISaveableComponentStateAdapter &adapter, EntityRef entity, std::span<const std::byte> payload,
            const CanonicalCodecLimits &limits);
        [[nodiscard]] const Binding *Find(const Gameplay::ComponentTypeId &type) const noexcept;
        [[nodiscard]] Result<EntityRef> ResolveEntity(PersistentEntityId entity, PersistentEntityGeneration generation,
                                                      const Gameplay::ComponentTypeId &type, const PersistentEntityIdentityMap &identities,
                                                      const ISaveableComponentAuthority &authority) const;

        std::vector<Binding> bindings_;
    };
}  // namespace Horo::Runtime
