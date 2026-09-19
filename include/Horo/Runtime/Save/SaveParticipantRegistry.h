#pragma once

/**
 * @file SaveParticipantRegistry.h
 * @brief Inert participant descriptors and immutable generation-checked registry snapshots.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveIdentity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace SaveParticipantRegistryDetail {
        struct SnapshotStorage;
    }
    struct CanonicalCaptureContext;
    class ICanonicalCaptureSink;
    enum class CanonicalCaptureDisposition : std::uint8_t;

    /** @brief Maximum participants admitted by one runtime-save registry generation. */
    inline constexpr std::size_t MaximumSaveParticipantCount = 256;

    /** @brief Semantic state scope exclusively owned by one save participant. */
    enum class SaveParticipantScope : std::uint8_t {
        RuntimeScene,
        SlotPlayer,
        PersistentWorld,
    };

    /** @brief Capture and restore roles supported by a participant adapter. */
    enum class SaveParticipantRole : std::uint8_t {
        None = 0,
        Capture = 1U << 0U,
        Restore = 1U << 1U,
    };

    /** @brief Whether an absent participant dependency rejects plan publication. */
    enum class SaveParticipantDependencyRequirement : std::uint8_t {
        Required,
        Optional
    };

    /** @brief Operation phase in which one participant must precede another. */
    enum class SaveParticipantDependencyPhase : std::uint8_t {
        Capture,
        Restore,
        CaptureAndRestore
    };

    /** @brief Typed dependency edge used to build deterministic capture and restore plans. */
    struct SaveParticipantDependency final {
        SaveParticipantId participant; /**< Stable dependency identity. */
        SaveParticipantDependencyRequirement requirement{SaveParticipantDependencyRequirement::Required}; /**< Absence policy. */
        SaveParticipantDependencyPhase phase{SaveParticipantDependencyPhase::CaptureAndRestore};          /**< Ordered phase. */

        SaveParticipantDependency() = default;

        /** @brief Creates one typed edge while preserving the former identity-only required-both spelling.
         * @param value Stable participant identity.
         * @param dependencyRequirement Whether absence rejects plan publication.
         * @param dependencyPhase Operation phase ordered by this edge.
         */
        SaveParticipantDependency(
            SaveParticipantId value,
            SaveParticipantDependencyRequirement dependencyRequirement = SaveParticipantDependencyRequirement::Required,
            SaveParticipantDependencyPhase dependencyPhase = SaveParticipantDependencyPhase::CaptureAndRestore)
            : participant(std::move(value)), requirement(dependencyRequirement), phase(dependencyPhase) {}

        [[nodiscard]] auto operator<=>(const SaveParticipantDependency &) const noexcept = default;
    };

    /** @brief Combines participant roles without converting them to loosely typed integers. */
    [[nodiscard]] constexpr SaveParticipantRole operator|(const SaveParticipantRole left, const SaveParticipantRole right) noexcept {
        return static_cast<SaveParticipantRole>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /** @brief Tests whether all requested roles are present. @param value Declared roles.
     * @param requested Roles to test. @return True when every requested role is present.
     */
    [[nodiscard]] constexpr bool HasSaveParticipantRole(const SaveParticipantRole value, const SaveParticipantRole requested) noexcept {
        return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(requested)) == static_cast<std::uint8_t>(requested);
    }

    /** @brief Participant-local limits counted within, never in addition to, operation-wide budgets. */
    struct SaveParticipantLimits final {
        std::uint64_t maximumPayloadBytes{}; /**< Maximum owned canonical payload bytes. */
        std::uint32_t maximumRecordCount{};  /**< Maximum canonical records emitted by one capture. */
        std::uint16_t maximumNestingDepth{}; /**< Maximum canonical value nesting admitted by its codec. */

        [[nodiscard]] constexpr auto operator<=>(const SaveParticipantLimits &) const noexcept = default;
    };

    /**
     * @brief Inert owned metadata describing one canonical state participant.
     *
     * Construction, copying, and validation never register services, call an adapter, inspect
     * ambient state, or invoke lifecycle behavior. One descriptor owns one semantic record set.
     */
    struct CanonicalStateParticipantDescriptor final {
        SaveParticipantId participant;                                  /**< Stable identity independent of RTTI. */
        ParticipantSchemaVersion schemaVersion;                         /**< Independent canonical payload schema. */
        SaveParticipantScope scope{SaveParticipantScope::RuntimeScene}; /**< Exclusive persistence scope. */
        SaveParticipantRole roles{SaveParticipantRole::None};           /**< Declared capture/restore roles. */
        bool required{true};                                            /**< Whether absence rejects restore. */
        SaveParticipantLimits limits;                                   /**< Finite participant-local cost declaration. */
        std::vector<SaveParticipantDependency> dependencies;            /**< Required/optional phase-specific ordering edges. */
        std::vector<SaveRecordId> ownedRecords;                         /**< Exclusive stable semantic record identities. */

        [[nodiscard]] auto operator<=>(const CanonicalStateParticipantDescriptor &) const noexcept = default;
    };

    /**
     * @brief Polymorphic lifetime anchor for a host-bound canonical state adapter.
     *
     * Capture and restore operation methods are introduced by their owning contracts. The registry
     * owns this lease so module code cannot unload while an accepted operation retains a snapshot.
     */
    class ICanonicalStateAdapter {
    public:
        virtual ~ICanonicalStateAdapter() = default;

        /**
         * @brief Captures one coherent participant through a host-owned bounded sink.
         * @param context Exact safe-point provenance and remaining admitted allocation budget.
         * @param sink Call-scoped host sink; adapters must not retain it.
         * @return Captured/omitted disposition or a typed participant failure.
         */
        [[nodiscard]] virtual Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &context,
                                                                          ICanonicalCaptureSink &sink) const = 0;
    };

    /** @brief One immutable descriptor and its owned adapter lease. */
    class SaveParticipantBinding final {
    public:
        /** @brief Returns inert participant metadata. @return Borrowed descriptor owned by this binding. */
        [[nodiscard]] const CanonicalStateParticipantDescriptor &Descriptor() const noexcept;
        /** @brief Returns the pinned adapter lease. @return Shared immutable adapter ownership. */
        [[nodiscard]] const std::shared_ptr<const ICanonicalStateAdapter> &Adapter() const noexcept;

    private:
        struct RegistryConstructionTag final {};
        friend class CanonicalStateParticipantRegistry;

    public:
        SaveParticipantBinding(RegistryConstructionTag, const CanonicalStateParticipantDescriptor &descriptor,
                               const std::shared_ptr<const ICanonicalStateAdapter> &adapter);

    private:
        CanonicalStateParticipantDescriptor descriptor_;
        std::shared_ptr<const ICanonicalStateAdapter> adapter_;
    };

    /** @brief Successful registration evidence tied to the resulting mutable registry generation. */
    struct SaveParticipantRegistration final {
        SaveParticipantId participant;      /**< Accepted stable participant identity. */
        std::uint64_t registryGeneration{}; /**< Exact non-zero generation after registration. */
    };

    /**
     * @brief Immutable owning registry view pinned by an accepted save/restore operation.
     *
     * Copies share immutable storage and adapter leases. Later unregister, rebind, or registry
     * shutdown cannot mutate this view or unload code while the snapshot remains alive.
     */
    class SaveParticipantRegistrySnapshot final {
    public:
        SaveParticipantRegistrySnapshot() = default;
        /** @brief Reports whether this is an issued snapshot. @return True for non-zero generation and owned storage. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the exact observed registry generation. @return Zero only for an invalid default snapshot. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;
        /** @brief Returns bindings in stable participant-identity order. @return Borrowed immutable bindings. */
        [[nodiscard]] std::span<const SaveParticipantBinding> Bindings() const noexcept;
        /** @brief Returns capture-capable bindings in stable dependency order. @return Borrowed immutable capture plan. */
        [[nodiscard]] std::span<const SaveParticipantBinding> CaptureBindings() const noexcept;
        /** @brief Returns restore-capable bindings in stable dependency order. @return Borrowed immutable restore plan. */
        [[nodiscard]] std::span<const SaveParticipantBinding> RestoreBindings() const noexcept;
        /** @brief Finds one pinned binding. @param participant Stable participant identity.
         * @return Borrowed binding, or null when this generation does not contain it.
         */
        [[nodiscard]] const SaveParticipantBinding *Find(const SaveParticipantId &participant) const noexcept;

    private:
        friend class CanonicalStateParticipantRegistry;
        SaveParticipantRegistrySnapshot(std::uint64_t generation,
                                        std::shared_ptr<const SaveParticipantRegistryDetail::SnapshotStorage> storage);

        std::uint64_t generation_{};
        std::shared_ptr<const SaveParticipantRegistryDetail::SnapshotStorage> storage_;
    };

    /** @brief Host-owned explicit participant composition registry with bounded immutable snapshots. */
    class CanonicalStateParticipantRegistry final {
    public:
        CanonicalStateParticipantRegistry();
        ~CanonicalStateParticipantRegistry();
        CanonicalStateParticipantRegistry(const CanonicalStateParticipantRegistry &) = delete;
        CanonicalStateParticipantRegistry &operator=(const CanonicalStateParticipantRegistry &) = delete;

        /** @brief Validates and copies one descriptor and adapter lease. @param descriptor Inert metadata.
         * @param adapter Owned adapter lease bound by host composition.
         * @return Registration evidence or a typed validation, duplicate, capacity, allocation, or lifecycle error.
         * @pre Called by the owning composition thread while the registry is quiescent.
         */
        [[nodiscard]] Result<SaveParticipantRegistration> Register(const CanonicalStateParticipantDescriptor &descriptor,
                                                                   std::shared_ptr<const ICanonicalStateAdapter> adapter);

        /** @brief Removes one live registration before shutdown. @param participant Identity to remove.
         * @return Whether a registration was removed, or a lifecycle/generation error.
         * @pre Called by the owning composition thread while the registry is quiescent.
         * @post Already issued snapshots and their adapter leases remain unchanged.
         */
        [[nodiscard]] Result<bool> Unregister(const SaveParticipantId &participant);

        /** @brief Validates the complete graph and publishes an owning immutable view.
         * @return Identity-sorted bindings plus stable plans, or a typed dependency, allocation, or lifecycle error.
         * @pre Called by the owning composition thread; concurrent registry mutation is forbidden.
         */
        [[nodiscard]] Result<SaveParticipantRegistrySnapshot> Snapshot() const;

        /** @brief Closes admission and releases live registrations; issued snapshots remain safe.
         * @pre Called by the owning composition thread after closing new operation admission.
         */
        void Close() noexcept;
        /** @brief Reports whether admission and new snapshot publication are closed. @return Current lifecycle state. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Returns the mutable registry generation. @return Non-zero exact generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;

    private:
        std::vector<SaveParticipantBinding> bindings_;
        std::uint64_t generation_{1};
        bool closed_{false};
    };
}  // namespace Horo::Runtime
