#pragma once

/**
 * @file ReplicationWorldLifecycle.h
 * @brief Generation-fenced replication ownership for one active runtime Scene and session.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkObjectMapping.h"
#include "Horo/Network/ReplicationRoles.h"
#include "Horo/Runtime/FrameScheduler.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Network {
    class NetworkMetrics;
    struct NetworkPrefabLifecycleRecord;

    namespace Detail {
        class NetworkModeWorldState;
        struct ReplicationWorldRecord;
    }  // namespace Detail

    /** @brief Owner-thread lifecycle state of one staged or published replication world. */
    enum class ReplicationWorldLifecycleState : std::uint8_t {
        Empty,
        Active,
        Paused,
        ShuttingDown,
        Closed,
    };

    /** @brief Narrow capabilities exposed to Scene/Gameplay for one world role. */
    enum class ReplicationWorldCapability : std::uint8_t {
        CaptureCanonicalState,
        ApplyAuthoritativeState,
        SubmitCommands,
        PublishAuthority,
        Count,
    };

    /** @brief Immutable role-derived capabilities; no Scene or Gameplay storage is exposed. */
    struct ReplicationWorldCapabilities final {
        bool captureCanonicalState{};
        bool applyAuthoritativeState{};
        bool submitCommands{};
        bool publishAuthority{};

        /** @brief Tests one role-derived capability. @param capability Capability to query. @return Whether it is admitted. */
        [[nodiscard]] constexpr bool Contains(const ReplicationWorldCapability capability) const noexcept {
            switch (capability) {
                case ReplicationWorldCapability::CaptureCanonicalState:
                    return captureCanonicalState;
                case ReplicationWorldCapability::ApplyAuthoritativeState:
                    return applyAuthoritativeState;
                case ReplicationWorldCapability::SubmitCommands:
                    return submitCommands;
                case ReplicationWorldCapability::PublishAuthority:
                    return publishAuthority;
                case ReplicationWorldCapability::Count:
                    return false;
            }
            return false;
        }
    };

    /** @brief Bounded set of canonical runtime phases admitted by one world activation. */
    struct ReplicationWorldPhaseSet final {
        std::uint32_t bits{};

        /** @brief Builds a phase set from the canonical runtime phases. @param phases Declared phases. @return Phase set. */
        [[nodiscard]] static constexpr ReplicationWorldPhaseSet From(const std::initializer_list<Runtime::RuntimePhase> phases) noexcept {
            ReplicationWorldPhaseSet result;
            for (const Runtime::RuntimePhase phase : phases) {
                const auto value = static_cast<std::uint8_t>(phase);
                if (value < 32)
                    result.bits |= (1U << value);
            }
            return result;
        }

        /** @brief Returns the default network-safe phases. @return Fixed and network safe-point phases. */
        [[nodiscard]] static constexpr ReplicationWorldPhaseSet Default() noexcept {
            return From({Runtime::RuntimePhase::NetworkPoll, Runtime::RuntimePhase::FixedUpdate, Runtime::RuntimePhase::NetworkFlush,
                         Runtime::RuntimePhase::CommitDeferredLifecycleChanges});
        }

        /** @brief Tests whether a canonical runtime phase is declared. @param phase Phase to test. @return True when admitted. */
        [[nodiscard]] constexpr bool Contains(const Runtime::RuntimePhase phase) const noexcept {
            const auto value = static_cast<std::uint8_t>(phase);
            return value < 32 && (bits & (1U << value)) != 0;
        }

        /** @brief Validates that at least one known runtime phase is declared. @return True for usable phase evidence. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            constexpr auto knownPhaseCount = static_cast<std::uint8_t>(Runtime::RuntimePhase::EndFrame) + 1U;
            constexpr std::uint32_t knownBits = (1U << knownPhaseCount) - 1U;
            return bits != 0 && (bits & ~knownBits) == 0;
        }

        constexpr auto operator<=>(const ReplicationWorldPhaseSet &) const noexcept = default;
    };

    /** @brief Complete identity and phase contract for one networked runtime Scene. */
    struct ReplicationWorldActivationDescriptor final {
        Runtime::SceneRuntimeId scene;
        NetworkSessionGeneration session;
        ReplicationAuthorityEpoch authority;
        ReplicationExecutionRole role{ReplicationExecutionRole::Count};
        ReplicationWorldPhaseSet phases{ReplicationWorldPhaseSet::Default()};

        /** @brief Validates the complete world/session/authority/role contract. @return True when usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        constexpr auto operator<=>(const ReplicationWorldActivationDescriptor &) const noexcept = default;
    };

    /** @brief Setup bounds for one lifecycle and each detached object mapping candidate. */
    struct ReplicationWorldLimits final {
        std::size_t maximumObjects{4096};
        std::uint32_t maximumRetiredWorlds{64};
    };

    /** @brief One declared authored object from an exact committed Scene, before the authority assigns a network identity. */
    struct ReplicationAuthoredSceneObject final {
        Runtime::EntityRef entity;          /**< Live entity occurrence in the selected Scene generation. */
        NetworkObjectProvenance provenance; /**< Stable schema and required authored Scene object identity. */
    };

    /** @brief The authority-issued initial identity for one authored object, without process-local entity handles. */
    struct ReplicationAuthoredObjectBinding final {
        NetworkObjectId object;
        NetworkObjectProvenance provenance;
        bool operator==(const ReplicationAuthoredObjectBinding &) const noexcept = default;
    };

    /** @brief Authored-only baseline identities from one exact published authority world. */
    struct ReplicationAuthoredSceneInventory final {
        NetworkSessionGeneration session;
        ReplicationAuthorityEpoch authority;
        std::vector<ReplicationAuthoredObjectBinding> objects;
    };

    /** @brief A newly reserved Scene entity and its admitted dynamic replication schema. */
    struct ReplicationDynamicSceneObject final {
        Runtime::EntityRef entity;
        ReplicationSchemaId schema;
        ReplicationSchemaVersion version;
        std::optional<NetworkPrefabMemberSource> prefab;
        std::shared_ptr<const std::vector<std::byte>> prefabIntent; /**< Explicit bytes retained by the first mapped member. */
    };

    /** @brief Exact Scene/session/phase identity supplied by a caller requesting replication work. */
    struct ReplicationWorldWorkRequest final {
        Runtime::SceneRuntimeId scene;
        NetworkSessionGeneration session;
        Runtime::RuntimePhase phase{Runtime::RuntimePhase::EndFrame};
        std::uint64_t simulationTick{};
        CancellationToken cancellation;
    };

    /**
     * @brief Immutable capability lease for one active replication world.
     * @details The lease carries only identity, role-derived capabilities, cancellation, and an immutable mapping snapshot.
     * It never exposes mutable Scene, ECS, Gameplay, or transport storage.
     */
    class ReplicationWorldReadLease final {
    public:
        ReplicationWorldReadLease() noexcept = default;
        ReplicationWorldReadLease(const ReplicationWorldReadLease &) noexcept = default;
        ReplicationWorldReadLease &operator=(const ReplicationWorldReadLease &) noexcept = default;
        ReplicationWorldReadLease(ReplicationWorldReadLease &&) noexcept = default;
        ReplicationWorldReadLease &operator=(ReplicationWorldReadLease &&) noexcept = default;
        ~ReplicationWorldReadLease();

        /** @brief Reports whether the lease refers to a published world. @return True for a live lease pin. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the exact activation identity. @pre IsValid() is true. */
        [[nodiscard]] const ReplicationWorldActivationDescriptor &Descriptor() const noexcept;
        /** @brief Returns the immutable role-derived capabilities. @pre IsValid() is true. */
        [[nodiscard]] const ReplicationWorldCapabilities &Capabilities() const noexcept;
        /** @brief Returns the immutable object/entity mapping snapshot. @pre IsValid() is true. */
        [[nodiscard]] const NetworkObjectMappingSnapshot &Mapping() const noexcept;
        /** @brief Returns the world-revocation cancellation token. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;
        /** @brief Reports logical revocation; paused worlds remain valid and uncancelled. */
        [[nodiscard]] bool IsRevoked() const noexcept;

    private:
        friend class ReplicationWorldLifecycle;
        ReplicationWorldReadLease(std::shared_ptr<const Detail::ReplicationWorldRecord> record,
                                  NetworkObjectMappingSnapshot mapping) noexcept;

        std::shared_ptr<const Detail::ReplicationWorldRecord> record_;
        std::optional<NetworkObjectMappingSnapshot> mapping_;
    };

    /**
     * @brief Copies only initial authored identities from a published authority mapping.
     * @param world Published authority lease with the exact Scene occurrence.
     * @return Ordered inventory excluding dynamic groups, or a typed invalid/revoked world error.
     */
    [[nodiscard]] Result<ReplicationAuthoredSceneInventory> CaptureReplicationAuthoredScene(const ReplicationWorldReadLease &world);

    /** @brief Allocation-free owner-thread capture pin; it never copies the object mapping.
     * Mapping lookup is permitted only synchronously on the lifecycle owner thread. Generation
     * validity may be read from any thread; any structural mapping change revokes old reads.
     */
    class ReplicationWorldCaptureRead final {
    public:
        ReplicationWorldCaptureRead() noexcept = default;
        /** @brief Tests complete logical generation validity without consulting mutable mapping storage. @return Current pin. */
        [[nodiscard]] bool IsCurrent() const noexcept;
        /** @brief Returns immutable world identity. @pre IsCurrent(). @return Pinned descriptor. */
        [[nodiscard]] const ReplicationWorldActivationDescriptor &Descriptor() const noexcept;
        /** @brief Returns captured structural mapping revision. @return Nonzero revision for a valid pin. */
        [[nodiscard]] std::uint64_t MappingRevision() const noexcept;
        /** @brief Returns the exact NetworkFlush simulation tick that opened this read. @return Positive committed tick. */
        [[nodiscard]] std::uint64_t SimulationTick() const noexcept;
        /** @brief Copies one exact mapping on the lifecycle owner thread. @param object Exact occurrence. @return Entry or stale error. */
        [[nodiscard]] Result<NetworkObjectMappingEntry> Resolve(NetworkObjectId object) const;

    private:
        friend class ReplicationWorldLifecycle;
        ReplicationWorldCaptureRead(std::shared_ptr<const Detail::ReplicationWorldRecord> record, std::uint64_t revision,
                                    std::uint64_t tick) noexcept;
        std::shared_ptr<const Detail::ReplicationWorldRecord> record_;
        std::uint64_t revision_{};
        std::uint64_t tick_{};
    };

    /**
     * @brief Owner-thread publication authority for one active Scene/session replication world.
     * @details Stage builds all bounded mapping storage before publication. Safe-point commit swaps complete immutable
     * identity/capability records, revokes the old generation, and preserves it on any failure.
     */
    class ReplicationWorldLifecycle final {
    public:
        static constexpr std::uint32_t MaximumRetiredWorlds = 64;

        ReplicationWorldLifecycle(const ReplicationWorldLifecycle &) = delete;
        ReplicationWorldLifecycle &operator=(const ReplicationWorldLifecycle &) = delete;
        ReplicationWorldLifecycle(ReplicationWorldLifecycle &&other) noexcept;
        ReplicationWorldLifecycle &operator=(ReplicationWorldLifecycle &&) = delete;
        ~ReplicationWorldLifecycle();

        /** @brief Creates an empty lifecycle with prepared bounds and optional owner-thread metrics observer.
         * @param limits Finite retirement/object capacity.
         * @param metrics Borrowed observer kept alive until this lifecycle is destroyed.
         * @return Prepared lifecycle or typed capacity/invalid error.
         */
        [[nodiscard]] static Result<ReplicationWorldLifecycle> Create(ReplicationWorldLimits limits = {},
                                                                      NetworkMetrics *metrics = nullptr);

        /** @brief Stages a complete world candidate without changing the active world. */
        [[nodiscard]] Result<void> Stage(const ReplicationWorldActivationDescriptor &descriptor,
                                         const CancellationToken &cancellation = {});
        /**
         * @brief Stages an authority world with all of its authored mappings unpublished.
         * @param descriptor Exact world occurrence being activated.
         * @param objects Admitted declarations resolved against one committed Scene generation.
         * @param cancellation Owner activation token, rechecked before publication.
         * @return Success or a typed failure with no partial candidate.
         * @details The owner assigns slots in stable authored-object order. Clients receive authority-issued
         * identities through admitted network state and cannot originate these mappings.
         */
        [[nodiscard]] Result<void> StageAuthoredScene(const ReplicationWorldActivationDescriptor &descriptor,
                                                      std::span<const ReplicationAuthoredSceneObject> objects,
                                                      const CancellationToken &cancellation = {});
        /**
         * @brief Stages a client candidate using the exact authored identities received from its authenticated authority.
         * @param descriptor Exact local client Scene and authority world occurrence.
         * @param localObjects Admitted declarations from the client's committed Scene.
         * @param inventory Complete ordered authority inventory for this occurrence.
         * @param cancellation Owner activation token, rechecked before publication.
         * @return Success or a typed failure with the active world and its mapping unchanged.
         * @details Transport authentication and message admission belong to the calling world owner.
         */
        [[nodiscard]] Result<void> StageReplicaAuthoredScene(const ReplicationWorldActivationDescriptor &descriptor,
                                                             std::span<const ReplicationAuthoredSceneObject> localObjects,
                                                             const ReplicationAuthoredSceneInventory &inventory,
                                                             const CancellationToken &cancellation = {});
        /** @brief Publishes the staged candidate at the declared lifecycle safe point. */
        [[nodiscard]] Result<void> CommitAtSafePoint(Runtime::SceneRuntimeId expectedScene, NetworkSessionGeneration expectedSession);
        /** @brief Explicit phase-bearing commit overload used by runtime integrations. */
        [[nodiscard]] Result<void> CommitAtSafePoint(Runtime::SceneRuntimeId expectedScene, NetworkSessionGeneration expectedSession,
                                                     Runtime::RuntimePhase phase);

        /** @brief Pauses admission while retaining the active world and its worker leases. */
        [[nodiscard]] Result<void> Pause(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) noexcept;
        /** @brief Resumes admission for the unchanged active world. */
        [[nodiscard]] Result<void> Resume(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) noexcept;
        /** @brief Revokes and retires the exact active world. */
        [[nodiscard]] Result<void> Revoke(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) noexcept;
        /** @brief Alias for callers expressing revocation as Scene unload. */
        [[nodiscard]] Result<void> Unload(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) noexcept;

        /** @brief Registers one exact object/entity occurrence in the active world mapping. */
        [[nodiscard]] Result<void> RegisterObject(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session,
                                                  const NetworkObjectMappingEntry &entry);
        /** @brief Retires one exact object occurrence without affecting a newer generation. */
        [[nodiscard]] Result<void> RetireObject(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session, NetworkObjectId object);

        /**
         * @brief Reserves a complete authority mapping change for the same structural transaction as its Scene entities.
         * @param request Exact active Scene and session, lifecycle safe point, and operation cancellation.
         * @param added Exact object-to-entity associations for the detached Scene candidate.
         * @param retired Exact live object generations retired by that candidate.
         * @return Detached Scene participant result, or a typed failure preserving all live mappings.
         * @details Retirements occur before additions in the detached copy. Dropping the candidate
         * leaves live mappings, slot tombstones, capture revisions, and metrics untouched.
         * @pre The owner submits at most one mapping candidate for an aggregate Scene publication.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareObjectBatch(
            const ReplicationWorldWorkRequest &request, std::span<const NetworkObjectMappingEntry> added,
            std::span<const NetworkObjectId> retired) const;

        /**
         * @brief Issues and retires dynamic occurrences within one detached authority mapping change.
         * @param request Exact active Scene, session, lifecycle safe point and cancellation.
         * @param added Entity reservations in canonical group order with admitted schemas.
         * @param retired Exact Scene entity generations whose dynamic associations should retire.
         * @return The unpublished candidate or a typed error leaving the live mapping unchanged.
         * @details Slot selection and generation reuse are private to the candidate. Already authored
         * mappings cannot be retired through this API. A Prefab spawn supplies one recipe with
         * strictly increasing prepared entity slots. The authority issues its common occurrence.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareDynamicObjectBatch(
            const ReplicationWorldWorkRequest &request, std::span<const ReplicationDynamicSceneObject> added,
            std::span<const Runtime::EntityRef> retired) const;
        /**
         * @brief Prepares the mappings that publish with one client Scene group.
         * @param request Exact client Scene, session, structural phase and cancellation.
         * @param spawn Retained authenticated spawn record, also used to verify exact retirement.
         * @param members Full detached group in the verified cooked order, including local-only members.
         * @param creating True for reconstruction and false for retirement.
         * @return A detached mapping candidate or the typed admission or mapping error.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> PrepareReplicaPrefab(
            const ReplicationWorldWorkRequest &request, const NetworkPrefabLifecycleRecord &spawn,
            std::span<const Runtime::EntityRef> members, bool creating) const;

        /** @brief Acquires an immutable capability only for an active matching world and declared phase. */
        [[nodiscard]] Result<ReplicationWorldReadLease> Acquire(const ReplicationWorldWorkRequest &request) const;
        /** @brief Acquires an immutable capability and checks one role-derived permission. */
        [[nodiscard]] Result<ReplicationWorldReadLease> AcquireFor(const ReplicationWorldWorkRequest &request,
                                                                   ReplicationWorldCapability capability) const;

        /** @brief Opens allocation-free canonical capture at the declared post-commit network phase.
         * @param request Exact active scene/session, NetworkFlush phase and positive committed tick.
         * @return Authority-server capture pin or a typed phase, role, cancellation or lifecycle error.
         * @pre Caller serializes lifecycle mutation and capture on its owner thread.
         * @post No mapping vector is copied and no allocation occurs on success.
         */
        [[nodiscard]] Result<ReplicationWorldCaptureRead> AcquireCaptureRead(const ReplicationWorldWorkRequest &request) const;

        /** @brief Begins idempotent shutdown, revoking all staged, active, and retired worlds. */
        void BeginShutdown() noexcept;
        /** @brief Reclaims unpinned revoked worlds and completes shutdown when all leases drain. */
        [[nodiscard]] ReplicationWorldLifecycleState CollectRetired() noexcept;

        /** @brief Returns the active identity or a typed unavailable error. */
        [[nodiscard]] Result<ReplicationWorldActivationDescriptor> ActiveDescriptor() const;
        /** @brief Reads the active mapping revision without materializing a snapshot.
         * @pre Caller is the world owner and serializes structural mutation.
         * @return Active revision or zero when no mapping is active. */
        [[nodiscard]] std::uint64_t ObjectMappingRevision() const noexcept;
        /** @brief Reverse-resolves an exact live authority object without snapshot allocation.
         * @param entity Existing exact Scene entity. @param session Exact current world session.
         * @return The mapped network occurrence or a typed world or mapping error. */
        [[nodiscard]] Result<NetworkObjectId> FindAuthorityObject(Runtime::EntityRef entity, NetworkSessionGeneration session) const;
        /** @brief Resolves an exact admitted replica identity without snapshot allocation.
         * @param scene Exact client Scene. @param session Exact current world session.
         * @param object Existing network occurrence. @return The local entity or a typed world or mapping error. */
        [[nodiscard]] Result<Runtime::EntityRef> ResolveReplicaObject(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session,
                                                                      NetworkObjectId object) const;
        /** @brief Returns the owner-thread lifecycle state. */
        [[nodiscard]] ReplicationWorldLifecycleState State() const noexcept;
        /** @brief Returns the number of retained revoked worlds. */
        [[nodiscard]] std::size_t RetiredCount() const noexcept;
        /** @brief Reports whether a detached candidate is staged. */
        [[nodiscard]] bool HasStagedCandidate() const noexcept;

    private:
        friend class Detail::NetworkModeWorldState;
        /** @brief Copies prepared authored IDs for a provisional control publication. */
        [[nodiscard]] Result<ReplicationAuthoredSceneInventory> CaptureStagedAuthoredScene() const;
        /** @brief Checks whether another detached world occurrence may be prepared. */
        [[nodiscard]] Result<void> ValidateStage(const ReplicationWorldActivationDescriptor &descriptor,
                                                 const CancellationToken &cancellation) const;
        /** @brief Retains complete mapping storage only while the activation token remains live. */
        [[nodiscard]] Result<void> StoreStagedWorld(const ReplicationWorldActivationDescriptor &descriptor,
                                                    ReplicationWorldCapabilities capabilities, NetworkObjectMapping mapping,
                                                    const CancellationToken &cancellation);
        ReplicationWorldLifecycle(ReplicationWorldLimits limits, std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired,
                                  NetworkMetrics *metrics) noexcept;
        [[nodiscard]] bool CanRetireActive() const noexcept;
        [[nodiscard]] Result<ReplicationWorldReadLease> AcquireInternal(const ReplicationWorldWorkRequest &request,
                                                                        const ReplicationWorldCapability *capability) const;
        [[nodiscard]] Result<void> RequireWorldIdentity(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) const;
        [[nodiscard]] Result<void> RequireActive(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) const;
        /** @brief Validates structural mapping admission before allocating a detached copy. */
        [[nodiscard]] Result<void> ValidateObjectBatch(const ReplicationWorldWorkRequest &request,
                                                       ReplicationWorldCapability capability) const;
        /** @brief Captures the exact authority indexes and tombstones after batch admission. */
        [[nodiscard]] Result<NetworkObjectMapping> CopyMappingForBatch(
            const ReplicationWorldWorkRequest &request, std::size_t added, std::size_t retired,
            ReplicationWorldCapability capability = ReplicationWorldCapability::PublishAuthority) const;
        /** @brief Applies replica additions or verifies and retires the exact previously published rows. */
        [[nodiscard]] static Result<void> ApplyReplicaPrefab(NetworkObjectMapping &mapping, const NetworkPrefabLifecycleRecord &spawn,
                                                             std::span<const Runtime::EntityRef> members, bool creating);
        /** @brief Assigns dynamic rows and one common identity for a prepared Prefab group. */
        [[nodiscard]] static Result<void> AddDynamicObjects(NetworkObjectMapping &mapping,
                                                            std::span<const ReplicationDynamicSceneObject> added);
        /** @brief Wraps prepared indexes with the no-fail publication and rollback fences. */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> FinishObjectBatch(NetworkObjectMapping mapping,
                                                                                                   const CancellationToken &cancellation,
                                                                                                   std::size_t added,
                                                                                                   std::size_t retired) const;
        static void RevokeRecord(const std::shared_ptr<Detail::ReplicationWorldRecord> &record) noexcept;
        void RetireActive() noexcept;

        ReplicationWorldLimits limits_;
        std::shared_ptr<Detail::ReplicationWorldRecord> staged_;
        std::shared_ptr<Detail::ReplicationWorldRecord> active_;
        std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired_;
        ReplicationWorldLifecycleState state_{ReplicationWorldLifecycleState::Empty};
        NetworkMetrics *metrics_{};
    };
}  // namespace Horo::Network
