#pragma once

/** @file UiSceneReconciliation.h
 * @brief Transactional semantic UI ownership, scene replacement and retained publisher retirement.
 */

#include "Horo/Runtime/Scene/SceneIdentity.h"
#include "Horo/Runtime/Ui/UiHotReload.h"
#include "Horo/Runtime/Ui/UiOverlayLifecycle.h"

namespace Horo::Runtime::Ui {
    /** @brief Semantic lifetime authority, independent from audience, band, route, attachment and visibility. */
    enum class UiOwnerScopeKind : std::uint8_t {
        GameInstance,
        Player,
        Scene,
        Viewport,
        Count
    };

    /** @brief One immutable semantic owner; exactly the identity corresponding to kind is supplied. */
    struct UiSemanticOwner final {
        UiOwnerScopeKind kind{UiOwnerScopeKind::GameInstance};
        std::optional<UiFocusPlayerId> player;
        std::optional<SceneRuntimeId> scene;
        std::optional<UiOverlayViewportId> viewport;
        /** @brief Checks closed vocabulary and exact owner generation. @param ownership Service generation.
         * @return True only for a complete unambiguous owner.
         */
        [[nodiscard]] bool IsValid(UiOwnershipGeneration ownership) const noexcept;
    };

    /** @brief Host-admitted ownership; all Scene providers in this instance belong to providerScene.
     * @details A multi-scene provider composition must use separate instances, never infer nearest/current scene.
     */
    struct UiSceneInstanceDescriptor final {
        RuntimeUiInstanceId instance;
        UiSemanticOwner owner;
        std::optional<SceneRuntimeId> providerScene;
    };

    /** @brief Fixed admission and retirement budgets allocated before frame work. */
    struct UiSceneReconciliationLimits final {
        UiOwnershipGeneration ownership;
        std::uint32_t maximumInstances{32};
        std::uint32_t maximumRetiredInstances{32};
        std::uint32_t maximumPreparedTransitions{4};
        std::uint32_t previousInstanceSlot{}; /**< Ever-issued host instance slot high-water mark, including failed admissions. */
    };

    /** @brief Exact outgoing scene and explicit replacement; absence means unload. */
    struct UiSceneTransitionRequest final {
        SceneRuntimeId previous;
        std::optional<SceneRuntimeId> next;
    };

    /** @brief Full private same-document replacement for one persistent instance's Scene providers. */
    struct UiSceneBindingReplacement final {
        RuntimeUiInstanceId instance;
        UiReloadGeneration generation;
    };

    /** @brief Complete privately prepared incoming Scene UI; admission happens only with aggregate publication. */
    struct UiSceneActivation final {
        UiSceneInstanceDescriptor descriptor;
        UiReloadGeneration generation;
    };

    /** @brief Copied aggregate result; required-unavailable bindings remain fail-closed and observable. */
    struct UiSceneReconciliationResult final {
        std::uint32_t retiredSceneInstances{};
        std::uint32_t reboundPersistentInstances{};
        std::uint32_t activatedSceneInstances{};
        std::size_t requiredUnavailable{};
    };

    /** @brief Sole semantic owner of actual UiHotReload publishers through scene reconciliation.
     * @details All operations are serialized on the Runtime UI owner thread. Prepare is load-time and resolves no
     * provider, scene, renderer, platform or gameplay service. Hosts supply complete prepared Assets/runtime generations
     * and exact Scene-issued identities. Commit validates every source/candidate before any change, then retires only
     * matching Scene owners, reconciles persistent providers and activates incoming Scene UI at the ADR-073 cutoff.
     * Other semantic scopes and scenes survive. Foreign viewport attachments never transfer lifetime authority.
     * Frame operations use fixed slots, bounded scans and existing no-callback publication/retirement authorities.
     * Collection is load-time; old bindings, producers, resource and render leases retain their existing barriers.
     * The host commits its authoritative Scene transition only after preparation succeeds; failures preserve UI and
     * retain original typed errors. No UI callback commits Scene state. Required UI/provider failure policy belongs
     * to the host, which must reject a candidate when requiredUnavailable is unacceptable before aggregate commit.
     */
    class UiSceneReconciliation final {
    public:
        /** @brief Private exact-registry transition; destruction abandons only private candidates, never live state. */
        class Prepared final {
        public:
            ~Prepared();
            Prepared(Prepared &&) noexcept;
            Prepared &operator=(Prepared &&) noexcept;
            Prepared(const Prepared &) = delete;
            Prepared &operator=(const Prepared &) = delete;
            /** @brief Reads complete candidate evidence before host activation. @return Bounded reconciliation counts. */
            [[nodiscard]] const UiSceneReconciliationResult &Result() const noexcept;

        private:
            struct Storage;
            friend class UiSceneReconciliation;
            explicit Prepared(std::unique_ptr<Storage> storage) noexcept;
            std::unique_ptr<Storage> storage_;
        };

        /** @brief Reserves active/retired slots. @param limits Exact owner and capacities, each at most 64.
         * @return Service or typed capacity/ownership failure.
         */
        [[nodiscard]] static Result<UiSceneReconciliation> Create(const UiSceneReconciliationLimits &limits);
        ~UiSceneReconciliation();
        UiSceneReconciliation(UiSceneReconciliation &&) noexcept;
        UiSceneReconciliation &operator=(UiSceneReconciliation &&) noexcept;
        UiSceneReconciliation(const UiSceneReconciliation &) = delete;
        UiSceneReconciliation &operator=(const UiSceneReconciliation &) = delete;
        /** @brief Adopts an initial private publisher with immutable semantic ownership.
         * @param descriptor Exact host admission. @param publisher Actual publisher moved only on success.
         * @param point Owner lifecycle cutoff. @return Success or original typed failure.
         * @details Instance slots are monotonically issued and never reusable, even after failure/retirement.
         */
        [[nodiscard]] Result<void> Admit(const UiSceneInstanceDescriptor &descriptor, UiHotReload &&publisher,
                                         UiStructuralCommitPoint point);
        /** @brief Prepares every affected provider and required incoming UI before scene publication.
         * @param request Exact outgoing/new scene, never a stable definition ID.
         * @param bindings Exactly one replacement for each surviving instance tied to previous.
         * @param incoming Complete new Scene-owned instances tied to next.
         * @param cancellation Host cancellation ancestry. @return Complete candidate or typed failure without publication.
         */
        [[nodiscard]] Horo::Result<Prepared> Prepare(const UiSceneTransitionRequest &request,
                                                     std::vector<UiSceneBindingReplacement> bindings,
                                                     std::vector<UiSceneActivation> incoming = {},
                                                     const CancellationToken &cancellation = {});
        /** @brief Checks aggregate source identity, cancellation, safe point and retirement capacity.
         * @param prepared Exact candidate. @param point Owner cutoff. @return Success or original typed failure.
         */
        [[nodiscard]] Result<void> CanCommit(const Prepared &prepared, UiStructuralCommitPoint point) const;
        /** @brief Commits all owners or none without allocating, waiting or invoking a foreign callback.
         * @param prepared Candidate consumed once on success. @param point Owner cutoff.
         * @return Aggregate evidence or original typed failure preserving every last-good owner.
         */
        [[nodiscard]] Result<UiSceneReconciliationResult> Commit(Prepared &prepared, UiStructuralCommitPoint point);
        /** @brief Borrows one exact active publisher for a synchronous owner operation.
         * @param instance Exact semantic instance. @return Publisher or null after retirement/stale identity.
         * @details Never retain this pointer across registry commands; Acquire pins immutable generations instead.
         */
        [[nodiscard]] UiHotReload *Publisher(RuntimeUiInstanceId instance) noexcept;
        /** @brief Pins an exact active generation. @param instance Exact instance. @return Lease or typed stale/lifecycle failure. */
        [[nodiscard]] Result<UiReloadLease> Acquire(RuntimeUiInstanceId instance) const;
        /** @brief Drains retirement without blocking or freeing leased generations.
         * @return Reclaimed instance count or original error; callbacks cannot reenter the service during collection.
         */
        [[nodiscard]] Result<std::size_t> CollectRetired();
        /** @brief Closes admission and retires every scope idempotently before dependencies disappear. */
        void Shutdown() noexcept;
        /** @brief Checks complete candidate/producer/render drain after shutdown. @return Safe owner reclamation. */
        [[nodiscard]] bool CanReclaim() const noexcept;
        /** @brief Returns never-reusable instance slot high-water mark. @return Last issued slot. */
        [[nodiscard]] std::uint32_t LastIssuedInstanceSlot() const noexcept;

    private:
        struct Storage;
        explicit UiSceneReconciliation(std::shared_ptr<Storage> storage) noexcept;
        std::shared_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
