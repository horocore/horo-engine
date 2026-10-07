#pragma once

/** @file PrefabSpawnService.h
 * @brief Bounded tick-addressed cooked prefab operations and revocable Gameplay capability.
 */

#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"

#include <string>

namespace Horo::Prefab {
    class PrefabSpawnService;

    namespace Detail {
        struct PrefabSpawnState;
        struct PrefabSpawnOperation;
        struct PrefabInstanceState;
    }  // namespace Detail

    /** @brief Read-only identity of one complete spawned group; never mutable Scene storage. */
    class PrefabInstance final {
    public:
        PrefabInstance() = default;
        /** @brief Returns the generation-qualified root. @return Invalid for an empty instance. */
        [[nodiscard]] Runtime::EntityRef Root() const noexcept;

    private:
        friend class PrefabSpawnService;
        friend struct Detail::PrefabSpawnState;
        friend struct Detail::PrefabSpawnOperation;
        std::shared_ptr<const Detail::PrefabInstanceState> state_;
    };

    /** @brief Explicit copied binding to one declared external interface; separate from initialization. */
    struct PrefabRuntimeBinding final {
        PrefabPropertyId id;
        Runtime::EntityRef target;
    };

    /** @brief Copied, bounded creation input. Placement is the root's local transform relative to the optional parent.
     * notBeforeTick addresses the first eligible safe point, not asynchronous load completion. The operation expires
     * after 64 ticks beyond that point. Initialization IDs are runtime declarations, never authoring paths/overrides.
     */
    struct PrefabSpawnRequest final {
        PrefabTemplateLoadRequest source;
        Math::Transform placement;
        std::optional<Runtime::EntityRef> parent;
        std::uint64_t notBeforeTick{1};
        std::vector<PrefabInitializationValue> initialization;
        std::vector<PrefabRuntimeBinding> bindings;
    };

    /** @brief Owner-lane observable operation states; only Cancel may run on another thread. */
    enum class PrefabSpawnState {
        Pending,
        Committed,
        Failed,
        Cancelled
    };

    /** @brief Retainable operation observation with no service/Scene/provider lifetime requirement. */
    class PrefabOperation final {
    public:
        PrefabOperation() = default;
        /** @brief Observes the last owner-lane completion. @return Failed for an empty handle. */
        [[nodiscard]] PrefabSpawnState State() const noexcept;
        /** @brief Obtains the committed group for a spawn operation. @return Group or original typed failure/not-ready. */
        [[nodiscard]] Result<PrefabInstance> Spawned() const;
        /** @brief Borrows the original typed operation error. @return Null for pending/committed work. */
        [[nodiscard]] const Error *Failure() const noexcept;
        /** @brief Requests cancellation before publication; never blocks or joins work. */
        void Cancel() const noexcept;

    private:
        friend struct Detail::PrefabSpawnState;
        std::shared_ptr<Detail::PrefabSpawnOperation> operation_;
    };

    /** @brief Explicit trusted module policy and unique Scene incarnation selected by application composition. */
    struct GameplayPrefabBinding final {
        std::string moduleId;
        Runtime::SceneRuntimeId scene;
        bool enabled{};
        bool permissionGranted{};
    };
}  // namespace Horo::Prefab

namespace Horo::Gameplay {
    /** @brief Copiable, explicitly granted cooked-prefab capability. It retains no mutable Scene or provider.
     * Requests run only on the service owner lane. Revoke is an atomic fence safe on any thread; copied clients and
     * pending Scene transactions observe it. Destruction revokes the module scope before owner teardown.
     */
    class GameplayPrefabContext final {
    public:
        ~GameplayPrefabContext();
        GameplayPrefabContext(const GameplayPrefabContext &) = delete;
        GameplayPrefabContext &operator=(const GameplayPrefabContext &) = delete;
        /** @brief Queues bounded cooked-template preparation and creation.
         * @param request Complete owned typed input.
         * @param cancellation External operation ancestry.
         * @return Observation handle or typed admission failure without accepted work.
         */
        [[nodiscard]] Result<Prefab::PrefabOperation> Spawn(const Prefab::PrefabSpawnRequest &request,
                                                            const CancellationToken &cancellation = {}) const;
        /** @brief Queues retirement of a group created by this exact service and module scope.
         * @param instance Committed group identity; arbitrary entities are not accepted.
         * @param notBeforeTick First eligible safe point, bounded to 64 ticks ahead.
         * @param cancellation External operation ancestry.
         * @return Observation handle or typed rejection without mutation.
         */
        [[nodiscard]] Result<Prefab::PrefabOperation> Despawn(const Prefab::PrefabInstance &instance, std::uint64_t notBeforeTick,
                                                              const CancellationToken &cancellation = {}) const;
        /** @brief Permanently closes this module grant before unload, failure or play stop. */
        void Revoke() const noexcept;
        /** @brief Borrows immutable explicit routing. @return Exact host grant. */
        [[nodiscard]] const Prefab::GameplayPrefabBinding &Binding() const noexcept;
        /** @brief Resolves one declared reference interface from an existing committed member.
         * @param owner Generation-qualified owning entity, never a mutable borrow.
         * @param member Exact existing member schema/occurrence.
         * @param property Cook-declared reference identity, separate from initialization IDs.
         * @return Copied typed reference or typed revoked/undeclared/unavailable-target failure. Optional absence is Unbound.
         */
        [[nodiscard]] Result<Runtime::RuntimeResolvedGroupReference> Reference(Runtime::EntityRef owner,
                                                                               const Runtime::RuntimeGroupMemberIdentity &member,
                                                                               Prefab::PrefabPropertyId property) const;

    private:
        friend class Prefab::PrefabSpawnService;
        friend class BehaviorRuntime;
        GameplayPrefabContext(std::shared_ptr<Prefab::Detail::PrefabSpawnState> state, Prefab::GameplayPrefabBinding binding,
                              std::uint64_t scope, CancellationToken parent = {}, std::vector<Assets::AssetId> lineage = {});
        /** @brief Derives an instance client once at creation, preserving immutable Scene-owned spawn lineage.
         * @param entity Exact committed attachment owner.
         * @return Child client or typed stale/lineage failure; neither module code nor callbacks may reset lineage.
         */
        [[nodiscard]] Result<std::shared_ptr<const GameplayPrefabContext>> ForEntity(Runtime::EntityRef entity) const;
        std::shared_ptr<Prefab::Detail::PrefabSpawnState> state_;
        Prefab::GameplayPrefabBinding binding_;
        CancellationSource revocation_;
        std::uint64_t scope_{};
        std::vector<Assets::AssetId> lineage_;
    };
}  // namespace Horo::Gameplay

namespace Horo::Prefab {
    /** @brief Scene-owner-lane runtime operations over explicitly composed cooked provider and Scene service.
     * Borrowed authorities outlive this service. Hosts advance it before Scene's lifecycle safe point, after updating
     * the fixed tick. One complete transaction is queued at a time. At most 32 pending and 128 retained operations
     * and 32 module scopes are admitted; no waiting, source parsing, global discovery or behavior hooks occur here.
     */
    class PrefabSpawnService final : public Runtime::RuntimeLifecycleParticipant {
    public:
        /** @brief Captures one active Scene incarnation and frozen optional custom component registry.
         * @param provider Verified runtime template authority, outliving this service.
         * @param scenes Scene transaction authority, outliving this service.
         * @param components Frozen metadata authority for opaque custom components; null rejects those members.
         */
        PrefabSpawnService(PrefabTemplateProvider &provider, Runtime::RuntimeSceneService &scenes,
                           const Gameplay::ComponentRegistry *components = nullptr);
        ~PrefabSpawnService() override;
        PrefabSpawnService(const PrefabSpawnService &) = delete;
        PrefabSpawnService &operator=(const PrefabSpawnService &) = delete;
        /** @brief Admits an explicitly permitted module to this exact active Scene.
         * @param binding Trusted host policy and routing; never inferred from a service locator.
         * @return Revocable module capability or typed admission failure.
         */
        [[nodiscard]] Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> Acquire(GameplayPrefabBinding binding);
        /** @brief Advances bounded load/completion work and at most one eligible Scene transaction.
         * @param tick Monotonic committed fixed tick; zero is allowed only before the first simulation tick.
         * @return Owner-lane/tick failure; individual operations preserve their own typed failure.
         */
        [[nodiscard]] Result<void> Advance(std::uint64_t tick);
        /** @copydoc Runtime::RuntimeLifecycleParticipant::Startup */
        [[nodiscard]] Result<void> Startup(const CancellationToken &cancellation) override;
        /** @copydoc Runtime::RuntimeLifecycleParticipant::OnPhase */
        [[nodiscard]] Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override;
        /** @copydoc Runtime::RuntimeLifecycleParticipant::OnFixedUpdate */
        [[nodiscard]] Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override;
        /** @brief Revokes pending publication and detaches borrowed authorities; retained handles stay safe. */
        void Shutdown() noexcept override;

    private:
        std::shared_ptr<Detail::PrefabSpawnState> state_;
    };
}  // namespace Horo::Prefab
