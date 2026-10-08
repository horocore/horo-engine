#pragma once

/** @file PrefabTemplateProvider.h
 * @brief Registry-pinned canonical runtime prefab preparation and scene-bound lease admission.
 */

#include "Horo/Assets/AssetPayloadCache.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/AssetCookTargetId.h"
#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <memory>
#include <span>

namespace Horo::Prefab {
    namespace Detail {
        struct PrefabTemplateAllocation;
        struct PrefabTemplateRequest;
        struct PrefabTemplateProviderState;
    }  // namespace Detail

    /** @brief Exact verified runtime dependency, retaining its complete canonical artifact allocation. */
    struct PrefabTemplateDependencyLease final {
        Assets::AssetDependency metadata;
        Assets::AssetPayloadLease artifact;
    };

    /**
     * @brief Immutable complete template and resource closure, safe to retain after eviction or provider destruction.
     * @details Ownership is not spawn permission. ValidateAdmission rechecks the registry and active scene before
     * Scene-owned staging/publication. No source resolver, native callback or mutable scene storage is retained.
     */
    class PrefabTemplateLease final {
    public:
        PrefabTemplateLease() = default;
        /** @brief Returns the complete verified template. @return Null only for an empty lease. */
        [[nodiscard]] const CookedPrefab *Template() const noexcept;
        /** @brief Returns the captured registry revision. @return Zero for an empty lease. */
        [[nodiscard]] Assets::AssetRegistryRevision RegistryRevision() const noexcept;
        /** @brief Returns the scene incarnation captured by admission. @return Zero for an empty lease. */
        [[nodiscard]] Runtime::SceneRuntimeId Scene() const noexcept;
        /** @brief Returns canonical root envelope ownership. @return Empty only for an empty lease. */
        [[nodiscard]] Assets::AssetPayloadLease Artifact() const noexcept;
        /** @brief Returns the complete verified required resource closure. @return Borrow bounded by this lease. */
        [[nodiscard]] std::span<const PrefabTemplateDependencyLease> Dependencies() const noexcept;

    private:
        friend class PrefabTemplateProvider;
        explicit PrefabTemplateLease(std::shared_ptr<const Detail::PrefabTemplateAllocation> allocation,
                                     Runtime::SceneRuntimeId scene) noexcept;
        std::shared_ptr<const Detail::PrefabTemplateAllocation> allocation_;
        Runtime::SceneRuntimeId scene_;
    };

    /** @brief Owner-selected exact published root artifact; digest identity is supplied by the pinned cook/package generation. */
    struct PrefabTemplateLoadRequest final {
        Assets::AssetId asset;
        Sha256Digest artifactDigest; /**< SHA-256 of the entire canonical AssetCook envelope, not source or HPFB payload bytes. */
        AssetCookTargetId target;
    };

    /** @brief Finite per-provider request/cache bounds; outstanding external leases count against retained allocation capacity. */
    struct PrefabTemplateProviderLimits final {
        std::size_t maximumOutstanding{32};
        std::size_t maximumConcurrentLoads{8};
        std::size_t maximumTemplates{64};
        std::size_t maximumArtifactBytes{8U * 1024U * 1024U};
        std::size_t maximumRetainedBytes{64U * 1024U * 1024U};
        /** @brief Checks positive bounded capacities before any load is admitted. @return Whether usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Nonblocking preparation lifecycle; successful residency is distinct from scene admission. */
    enum class PrefabTemplateLoadState {
        Loading,
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Move-only observation handle; only cancellation may be called off the provider owner lane. */
    class PrefabTemplateLoadHandle final {
    public:
        PrefabTemplateLoadHandle() = default;
        /** @brief Requests cancellation when observation is abandoned; never joins loader work. */
        ~PrefabTemplateLoadHandle();
        PrefabTemplateLoadHandle(const PrefabTemplateLoadHandle &) = delete;
        PrefabTemplateLoadHandle &operator=(const PrefabTemplateLoadHandle &) = delete;
        PrefabTemplateLoadHandle(PrefabTemplateLoadHandle &&) noexcept = default;
        /** @brief Cancels the replaced request before transferring sole observation ownership. */
        PrefabTemplateLoadHandle &operator=(PrefabTemplateLoadHandle &&other) noexcept;
        /** @brief Observes preparation without waiting or I/O. @return Current state; empty handles are failed. */
        [[nodiscard]] PrefabTemplateLoadState State() const noexcept;
        /** @brief Requests cooperative cancellation; never waits for a worker. */
        void RequestCancel() const noexcept;

    private:
        friend class PrefabTemplateProvider;
        explicit PrefabTemplateLoadHandle(std::shared_ptr<Detail::PrefabTemplateRequest> request) noexcept;
        std::shared_ptr<Detail::PrefabTemplateRequest> request_;
    };

    /** @brief Owned prepared-group metadata and publication options, consumed synchronously on the Scene owner lane. */
    struct PrefabPreparedGroupOptions final {
        std::vector<std::vector<Runtime::GroupPhysicsBodyReference>>
            physicsReferences;                        /**< Dense body fixups resolved after reservation. */
        std::optional<Math::Transform> rootPlacement; /**< Replaces only the root local transform. */
        std::optional<Runtime::EntityRef> parent;     /**< Existing generation-qualified parent, revalidated at commit. */
        std::shared_ptr<const Runtime::SceneStructuralReceipt>
            *receipt{};                      /**< Optional borrowed output, written only on successful submission. */
        CancellationToken scopeCancellation; /**< Module revocation sampled again at publication. */
        std::vector<std::vector<Runtime::RuntimeGroupMemberIdentity>> members; /**< Complete occurrence metadata in dense entity order. */
        std::vector<std::vector<Runtime::RuntimeGroupReference>> references;   /**< Typed reference interfaces per entity. */
        std::vector<Assets::AssetId> spawnLineage; /**< Inherited creation lineage, bounded to 16 unique assets. */
    };

    /**
     * @brief Owner-lane runtime prefab preparation over host-composed Assets and Scene services.
     * @details The borrowed services outlive this provider. Loads capture one registry snapshot and active scene.
     * Workers belong exclusively to AssetLoadService; this provider never joins/waits or shuts that shared service down.
     * Advance is bounded load/preparation work, not a frame-hot query. Scene unload/replacement invalidates admission,
     * cancels pending work and removes resident cache pins; retained immutable leases remain readable.
     * Public gameplay spawn/despawn requests and initialization parameters belong to PFB-004.3, not this provider.
     */
    class PrefabTemplateProvider final : public Runtime::RuntimeLifecycleParticipant {
    public:
        /** @brief Constructs a provider without publishing or loading anything.
         * @param registry Canonical registry authority, outliving this provider.
         * @param loads Shared worker-owned asset loader, outliving this provider.
         * @param scenes Active-scene authority, outliving this provider.
         * @param profile Captured validated prefab policy.
         * @param limits Bounded provider capacities. Invalid limits fail Startup and LoadAsync.
         */
        PrefabTemplateProvider(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads, Runtime::RuntimeSceneService &scenes,
                               PrefabLimitProfile profile, PrefabTemplateProviderLimits limits = {});
        ~PrefabTemplateProvider() override;
        PrefabTemplateProvider(const PrefabTemplateProvider &) = delete;
        PrefabTemplateProvider &operator=(const PrefabTemplateProvider &) = delete;
        /** @brief Captures registry and active scene, then admits a bounded canonical load.
         * @param request Exact root identity, generation digest and cook target.
         * @param cancellation Parent cancellation ancestry.
         * @return Owned handle or typed admission/type/scene failure; no accepted work on failure.
         */
        [[nodiscard]] Result<PrefabTemplateLoadHandle> LoadAsync(PrefabTemplateLoadRequest request,
                                                                 const CancellationToken &cancellation = {});
        /** @brief Consumes completed worker results and schedules bounded dependency waves without waiting.
         * @return Owner-lane/limit failure; each request retains its original typed load failure.
         */
        [[nodiscard]] Result<void> Advance();
        /** @brief Transfers a complete successful lease only after current scene and registry admission checks.
         * @param handle This provider's terminal request; consumed exactly once.
         * @return Verified lease or original typed failure/not-ready/stale result.
         */
        [[nodiscard]] Result<PrefabTemplateLease> TakeResult(const PrefabTemplateLoadHandle &handle) const;
        /** @brief Rechecks residency provenance and scene incarnation immediately before Scene-owned publication.
         * @param lease Prepared complete lease from this provider.
         * @return Success or typed stale, foreign-provider or scene/shutdown failure. No scene mutation occurs.
         */
        [[nodiscard]] Result<void> ValidateAdmission(const PrefabTemplateLease &lease) const;
        /** @brief Checks exact host composition authority, including distinct services with equal numeric scene IDs.
         * @param scenes Host-selected Scene service.
         * @return Whether this provider borrows that exact service.
         */
        [[nodiscard]] bool UsesSceneService(const Runtime::RuntimeSceneService &scenes) const noexcept;
        /** @brief Hands a complete projected group to Scene's bounded all-or-nothing structural transaction.
         * @param lease Exact verified template/resource closure from this provider.
         * @param components Complete schema-projected runtime component sets, one per dense template entity.
         * @param cancellation Owning spawn operation's cooperative cancellation ancestry.
         * @param options Owned fixups, placement, cancellation scope and synchronous receipt output.
         * @return Deferred Scene tokens or rejection without queued work. Scene repeats generation/catalog/cancellation
         * checks at commit and retains real artifact allocations until the last group entity is destroyed.
         * @details The caller owns component schema projection and typed reference/binding initialization. This method
         * preserves cooked topology/transforms and never fabricates a missing projection or runs behavior hooks.
         * Public gameplay requests/placement/initialization parameter validation remain owned by PFB-004.3.
         */
        [[nodiscard]] Result<std::vector<Runtime::DeferredEntity>> QueuePreparedGroup(const PrefabTemplateLease &lease,
                                                                                      std::vector<Runtime::RuntimeComponentSet> components,
                                                                                      const CancellationToken &cancellation = {},
                                                                                      PrefabPreparedGroupOptions options = {});
        /** @brief Drops resident lookup pins for one asset; existing leases keep their immutable storage. @param asset Exact asset. */
        void Evict(Assets::AssetId asset) noexcept;
        /** @brief Reports canonical byte ownership, including evicted externally held allocations. @return Accounting snapshot. */
        [[nodiscard]] Assets::AssetPayloadCacheSnapshot PayloadSnapshot() const noexcept;
        /** @copydoc Runtime::RuntimeLifecycleParticipant::Startup */
        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        /** @copydoc Runtime::RuntimeLifecycleParticipant::OnPhase */
        [[nodiscard]] Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override;
        /** @copydoc Runtime::RuntimeLifecycleParticipant::OnFixedUpdate */
        [[nodiscard]] Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override;
        /** @brief Closes admission, cancels pending loads and releases resident pins without joining workers. */
        void Shutdown() noexcept override;

    private:
        std::unique_ptr<Detail::PrefabTemplateProviderState> state_;
    };
}  // namespace Horo::Prefab
