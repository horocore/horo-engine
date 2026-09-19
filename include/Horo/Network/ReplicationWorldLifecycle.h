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
#include <vector>

namespace Horo::Network {
    namespace Detail {
        struct ReplicationWorldRecord;
    }

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

        /** @brief Creates an empty lifecycle with prepared finite retirement/object bounds. */
        [[nodiscard]] static Result<ReplicationWorldLifecycle> Create(ReplicationWorldLimits limits = {});

        /** @brief Stages a complete world candidate without changing the active world. */
        [[nodiscard]] Result<void> Stage(const ReplicationWorldActivationDescriptor &descriptor,
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

        /** @brief Acquires an immutable capability only for an active matching world and declared phase. */
        [[nodiscard]] Result<ReplicationWorldReadLease> Acquire(const ReplicationWorldWorkRequest &request) const;
        /** @brief Acquires an immutable capability and checks one role-derived permission. */
        [[nodiscard]] Result<ReplicationWorldReadLease> AcquireFor(const ReplicationWorldWorkRequest &request,
                                                                   ReplicationWorldCapability capability) const;

        /** @brief Begins idempotent shutdown, revoking all staged, active, and retired worlds. */
        void BeginShutdown() noexcept;
        /** @brief Reclaims unpinned revoked worlds and completes shutdown when all leases drain. */
        [[nodiscard]] ReplicationWorldLifecycleState CollectRetired() noexcept;

        /** @brief Returns the active identity or a typed unavailable error. */
        [[nodiscard]] Result<ReplicationWorldActivationDescriptor> ActiveDescriptor() const;
        /** @brief Returns the owner-thread lifecycle state. */
        [[nodiscard]] ReplicationWorldLifecycleState State() const noexcept;
        /** @brief Returns the number of retained revoked worlds. */
        [[nodiscard]] std::size_t RetiredCount() const noexcept;
        /** @brief Reports whether a detached candidate is staged. */
        [[nodiscard]] bool HasStagedCandidate() const noexcept;

    private:
        ReplicationWorldLifecycle(ReplicationWorldLimits limits,
                                  std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired) noexcept;
        [[nodiscard]] bool CanRetireActive() const noexcept;
        [[nodiscard]] Result<ReplicationWorldReadLease> AcquireInternal(const ReplicationWorldWorkRequest &request,
                                                                        const ReplicationWorldCapability *capability) const;
        [[nodiscard]] Result<void> RequireWorldIdentity(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) const;
        [[nodiscard]] Result<void> RequireActive(Runtime::SceneRuntimeId scene, NetworkSessionGeneration session) const;
        static void RevokeRecord(const std::shared_ptr<Detail::ReplicationWorldRecord> &record) noexcept;
        void RetireActive() noexcept;

        ReplicationWorldLimits limits_;
        std::shared_ptr<Detail::ReplicationWorldRecord> staged_;
        std::shared_ptr<Detail::ReplicationWorldRecord> active_;
        std::vector<std::shared_ptr<Detail::ReplicationWorldRecord>> retired_;
        ReplicationWorldLifecycleState state_{ReplicationWorldLifecycleState::Empty};
    };
}  // namespace Horo::Network
