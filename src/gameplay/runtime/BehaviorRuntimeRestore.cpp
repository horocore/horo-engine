#include "BehaviorRuntimeRestoreInternal.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"

#include <limits>
#include <new>
#include <tuple>

namespace Horo::Gameplay {
    namespace {
        bool OnOwner(const auto &state) noexcept {
            return state.owner == std::this_thread::get_id();
        }

        bool CandidateCanBind(const BehaviorRuntimeRestoreCandidate::State &state) noexcept {
            return OnOwner(state) && !state.closed && state.runtime && state.registry &&
                   BehaviorRuntimeRestoreAccess::BindingAdmitted(*state.runtime);
        }

        class TargetAccess final : public Detail::IBehaviorPersistenceTargetAccess {
        public:
            TargetAccess(std::weak_ptr<BehaviorRuntimeRestoreCandidate::State> state, BehaviorTypeId type,
                         std::shared_ptr<BehaviorRestoreBindingProgress> progress, std::vector<BehaviorAttachmentProjection> identities)
                : state_(state), type_(std::move(type)), progress_(std::move(progress)), epoch_(state.lock()->bindingEpoch),
                  identities_(std::move(identities)) {}

            bool CanUse() const noexcept override {
                if (std::this_thread::get_id() != owner_)
                    return false;
                const auto state = state_.lock();
                return state && OnOwner(*state) && !state->closed && state->runtime && state->bindingEpoch == epoch_ &&
                       (state->published || state->prehookReady) && BehaviorRuntimeRestoreAccess::BindingAdmitted(*state->runtime);
            }

            Result<void> Visit(IBehaviorPersistenceVisitor &visitor) const override {
                const auto state = state_.lock();
                if (!CanUse())
                    return Result<void>::Failure(BehaviorRestoreError("Candidate borrow is revoked or foreign-thread."));
                auto projections = BehaviorRuntimeRestoreAccess::Project(*state->runtime);
                if (projections.HasError())
                    return Result<void>::Failure(projections.ErrorValue());
                auto projected = std::move(projections).Value();
                const auto range = std::ranges::equal_range(projected, type_, {}, &BehaviorRestoreInstance::type);
                std::span<BehaviorRestoreInstance> actual{range.begin(), range.end()};
                if (auto decorated = Decorate(actual); decorated.HasError())
                    return decorated;
                return visitor.Visit(actual);
            }

            bool InactiveRestore() const noexcept override {
                const auto state = state_.lock();
                return state && state->restoring && !state->published && !state->closed;
            }

            bool SameOwner(const IBehaviorPersistenceTargetAccess &other) const noexcept override {
                const auto *actual = dynamic_cast<const TargetAccess *>(&other);
                return actual && epoch_ == actual->epoch_ && state_.lock() == actual->state_.lock() && type_ == actual->type_ &&
                       std::ranges::equal(identities_, actual->identities_, [](const auto &left, const auto &right) {
                    return left.runtime == right.runtime && left.identity == right.identity && left.type == right.type;
                });
            }

            void MarkMaterialized() const noexcept override {
                progress_->materialized = true;
            }

            void MarkFixed() const noexcept override {
                progress_->fixed = true;
            }

        private:
            Result<void> Decorate(std::span<BehaviorRestoreInstance> actual) const {
                if (actual.size() != identities_.size())
                    return Result<void>::Failure(
                        BehaviorRestoreError("Actual attachment inventory changed from the admitted identity map."));
                using Key = std::tuple<Runtime::EntityRef, std::uint64_t, BehaviorTypeId>;
                const auto key = [](const BehaviorAttachmentProjection &value) {
                    return Key{value.runtime, value.identity.instance.value, value.type};
                };
                for (auto &instance : actual) {
                    const Runtime::EntityRef entity{{instance.entity.scene}, {instance.entity.index, instance.entity.generation}};
                    const Key expected{entity, instance.instance.value, instance.type};
                    const auto found = std::ranges::lower_bound(identities_, expected, {}, key);
                    if (found == identities_.end() || key(*found) != expected)
                        return Result<void>::Failure(BehaviorRestoreError("Attachment is absent from the admitted actual identity map."));
                    instance.persistentEntity = found->identity.entity;
                    instance.persistentGeneration = found->identity.generation;
                }
                return Result<void>::Success();
            }

            std::weak_ptr<BehaviorRuntimeRestoreCandidate::State> state_;
            BehaviorTypeId type_;
            std::shared_ptr<BehaviorRestoreBindingProgress> progress_;
            std::uint64_t epoch_{};
            std::vector<BehaviorAttachmentProjection> identities_;
            const std::thread::id owner_{std::this_thread::get_id()};
        };

        class OwnedScene final : public Runtime::SceneActivationCandidate {
        public:
            OwnedScene(std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner, std::shared_ptr<BehaviorRuntimeRestoreCandidate> candidate)
                : owner_(std::move(owner)), candidate_(std::move(candidate)) {}

            Result<void> ValidatePublication() const override {
                return candidate_->ValidatePublication();
            }

            void Publish() noexcept override {
                BehaviorRuntimeRestoreAccess::PublishCandidate(*candidate_, *owner_);
            }

            void Shutdown() noexcept override {
                BehaviorRuntimeRestoreAccess::ShutdownCandidate(*candidate_);
                if (owner_->pending == candidate_)
                    owner_->pending.reset();
            }

        private:
            std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner_;
            std::shared_ptr<BehaviorRuntimeRestoreCandidate> candidate_;
        };

        class SceneParticipant final : public Runtime::SceneActivationParticipant {
        public:
            explicit SceneParticipant(std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner) : owner_(std::move(owner)) {}

            Result<void> PrepareOwnedScene(Runtime::RuntimeScene &scene) override {
                if (owner_->pending) {
                    if (owner_->pending->Runner() && owner_->pending->Runner()->InstanceCount() <= owner_->limits.maximumInstances) {
                        if (auto matched = ValidatePending(scene); matched.HasError())
                            return matched;
                        return PreparePendingHooks();
                    }
                    return Result<void>::Failure(BehaviorRestoreError("Pending behavior candidate is unavailable."));
                }
                auto prepared = BehaviorRuntimeRestoreAccess::Prepare(owner_, scene, false);
                if (prepared.HasError())
                    return Result<void>::Failure(prepared.ErrorValue());
                owner_->pending = std::move(prepared).Value();
                return Result<void>::Success();
            }

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &,
                                                                               const Runtime::RuntimeSceneView &) override {
                if (!owner_->pending)
                    return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(BehaviorRestoreError("No owned candidate."));
                auto result = BehaviorRuntimeRestoreAccess::SceneCandidate(owner_, owner_->pending);
                return result;
            }

        private:
            Result<void> PreparePendingHooks() {
                auto result = BehaviorRuntimeRestoreAccess::PrepareHooks(*owner_->pending);
                if (result.HasError()) {
                    BehaviorRuntimeRestoreAccess::ShutdownCandidate(*owner_->pending);
                    owner_->pending.reset();
                }
                return result;
            }

            Result<void> ValidatePending(const Runtime::RuntimeScene &scene) const {
                return BehaviorRuntimeRestoreAccess::Matches(*owner_->pending, scene.View().RuntimeId())
                           ? Result<void>::Success()
                           : Result<void>::Failure(BehaviorRestoreError("Pending candidate belongs to another actual Scene."));
            }

            std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner_;
        };
    }  // namespace

    BehaviorRuntimeRestoreCandidate::BehaviorRuntimeRestoreCandidate(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    BehaviorRuntimeRestoreCandidate::~BehaviorRuntimeRestoreCandidate() {
        Shutdown();
    }

    BehaviorRuntime *BehaviorRuntimeRestoreCandidate::Runner() const noexcept {
        return OnOwner(*state_) && !state_->closed ? state_->runtime.get() : nullptr;
    }

    Result<BehaviorPersistenceTarget> BehaviorRuntimeRestoreCandidate::Binding(const BehaviorTypeId &type,
                                                                               const Runtime::PersistentEntityIdentityMap &map) {
        using Outcome = Result<BehaviorPersistenceTarget>;
        if (!CandidateCanBind(*state_) || (!state_->published && (!state_->restoring || !state_->prehookReady)) ||
            (!state_->published && state_->bindings.size() >= Runtime::MaximumSaveParticipantCount))
            return Outcome::Failure(BehaviorRestoreError("Cannot bind this inactive candidate."));
        if (!state_->registry->Find(type))
            return Outcome::Failure(BehaviorRestoreError("Registered owner type is unavailable."));
        auto identities = BehaviorRuntimeRestoreAccess::AttachmentIdentities(*state_->runtime, type, map);
        if (identities.HasError())
            return Outcome::Failure(identities.ErrorValue());
        auto progress = std::make_shared<BehaviorRestoreBindingProgress>();
        auto access = std::make_shared<TargetAccess>(state_, type, progress, std::move(identities).Value());
        if (!state_->published)
            state_->bindings.push_back(std::move(progress));
        return Outcome::Success(BehaviorPersistenceTarget{std::move(access), state_->generation, type});
    }

    Result<void> BehaviorRuntimeRestoreCandidate::StageLifecycle(const BehaviorLifecycleSnapshot &saved,
                                                                 const Runtime::PersistentEntityIdentityMap &identities) {
        if (!OnOwner(*state_) || state_->closed || state_->published || !state_->restoring || state_->staged)
            return Result<void>::Failure(BehaviorRestoreError("Lifecycle staging is not available."));
        auto matched = BehaviorRuntimeRestoreAccess::Match(*state_->runtime, saved, identities);
        if (matched.HasError())
            return Result<void>::Failure(matched.ErrorValue());
        state_->lifecycle = std::move(matched).Value();
        state_->staged = true;
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntimeRestoreCandidate::ValidatePublication() const {
        if (!OnOwner(*state_) || state_->closed || state_->published)
            return Result<void>::Failure(BehaviorRestoreError("Candidate publication is stale."));
        if (auto current = BehaviorRuntimeRestoreAccess::ValidateGeneration(*state_->runtime); current.HasError())
            return current;
        if (!state_->restoring)
            return Result<void>::Success();
        if (!state_->prehookReady || !state_->staged || std::ranges::any_of(state_->bindings, [](const auto &binding) {
            return !binding->materialized || !binding->fixed;
        }))
            return Result<void>::Failure(BehaviorRestoreError("Durable state, lifecycle or references are not fully prepared."));
        return Result<void>::Success();
    }

    void BehaviorRuntimeRestoreCandidate::PublishLifecycle() noexcept {
        if (!state_->staged || state_->closed || state_->lifecyclePublished)
            std::terminate();
        BehaviorRuntimeRestoreAccess::Publish(*state_->runtime, state_->lifecycle);
        state_->lifecyclePublished = true;
    }

    void BehaviorRuntimeRestoreCandidate::Shutdown() noexcept {
        if (state_->closed)
            return;
        state_->closed = true;
        if (state_->runtime)
            state_->runtime->Shutdown();
        state_->runtime.reset();
        state_->generation.reset();
        state_->registry = nullptr;
        state_->bindings.clear();
    }

    Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>> BehaviorRuntimeRestoreAccess::Prepare(
        std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner, Runtime::RuntimeScene &scene, bool restoring,
        BehaviorRuntimeServiceBindings services, std::shared_ptr<const GameplayPrefabContext> prefabs) {
        using Outcome = Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>>;
        if (!OnOwner(*owner))
            return Outcome::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!owner->registry)
            return Outcome::Failure(BehaviorRestoreError("Gameplay module owner is quiesced."));
        if (prefabs && prefabs->Binding().scene != scene.View().RuntimeId())
            return Outcome::Failure(MakeError(Prefab::PrefabErrors::SceneUnavailable));
        try {
            auto state = std::make_shared<BehaviorRuntimeRestoreCandidate::State>();
            state->scene = &scene;
            state->registry = owner->registry;
            state->restoring = restoring;
            auto runtime = Construct(scene, *owner->registry, owner->limits);
            if (runtime.HasError())
                return Outcome::Failure(runtime.ErrorValue());
            state->runtime = std::move(runtime).Value();
            auto candidate = std::shared_ptr<BehaviorRuntimeRestoreCandidate>{new BehaviorRuntimeRestoreCandidate{state}};
            if (restoring)
                if (auto configured = Configure(*state->runtime, std::move(services), std::move(prefabs), owner->scenes);
                    configured.HasError())
                    return Outcome::Failure(configured.ErrorValue());
            state->generation = Generation(*state->runtime);
            return Outcome::Success(std::move(candidate));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::RestoreAllocationFailed));
        } catch (...) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::LifecycleCallbackFailed));
        }
    }

    Result<void> BehaviorRuntimeRestoreAccess::PrepareHooks(BehaviorRuntimeRestoreCandidate &candidate) {
        auto &state = *candidate.state_;
        if (!state.restoring || state.prehookReady)
            return Result<void>::Success();
        auto prepared = Prehook(*state.runtime);
        if (prepared.HasValue())
            state.prehookReady = true;
        return prepared;
    }

    Result<std::unique_ptr<Runtime::SceneActivationCandidate>> BehaviorRuntimeRestoreAccess::SceneCandidate(
        std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner, std::shared_ptr<BehaviorRuntimeRestoreCandidate> candidate) {
        return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
            std::make_unique<OwnedScene>(std::move(owner), std::move(candidate)));
    }

    std::unique_ptr<Runtime::SceneActivationParticipant> BehaviorRuntimeRestoreAccess::Participant(
        std::shared_ptr<BehaviorRuntimeSceneOwner::State> owner) {
        return std::make_unique<SceneParticipant>(std::move(owner));
    }

    Result<void> BehaviorRuntimeRestoreAccess::StageFlags(BehaviorRuntimeRestoreCandidate &candidate,
                                                          std::vector<BehaviorLifecycleFlags> flags) {
        auto &state = *candidate.state_;
        if (!OnOwner(state) || state.closed || state.published || !state.restoring || state.staged ||
            flags.size() != state.runtime->InstanceCount())
            return Result<void>::Failure(BehaviorRestoreError("Lifecycle receipt is stale or incomplete."));
        state.lifecycle = std::move(flags);
        state.staged = true;
        return Result<void>::Success();
    }

    void BehaviorRuntimeRestoreAccess::PublishLifecycle(BehaviorRuntimeRestoreCandidate &candidate) noexcept {
        candidate.PublishLifecycle();
    }

    void BehaviorRuntimeRestoreAccess::PublishCandidate(BehaviorRuntimeRestoreCandidate &candidate,
                                                        BehaviorRuntimeSceneOwner::State &owner) noexcept {
        auto &state = *candidate.state_;
        if (state.restoring && !state.lifecyclePublished)
            std::terminate();
        state.published = true;
        owner.active = candidate.shared_from_this();
        if (owner.pending.get() == &candidate)
            owner.pending.reset();
    }

    bool BehaviorRuntimeRestoreAccess::Matches(const BehaviorRuntimeRestoreCandidate &candidate, Runtime::SceneRuntimeId scene) noexcept {
        return OnOwner(*candidate.state_) && !candidate.state_->closed && candidate.state_->scene->View().RuntimeId() == scene;
    }

    void BehaviorRuntimeRestoreAccess::ShutdownCandidate(BehaviorRuntimeRestoreCandidate &candidate) noexcept {
        candidate.Shutdown();
    }

    BehaviorRuntimeSceneOwner::BehaviorRuntimeSceneOwner(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

    Result<std::shared_ptr<BehaviorRuntimeSceneOwner>> BehaviorRuntimeSceneOwner::Create(Runtime::RuntimeSceneService &scenes,
                                                                                         const BehaviorRegistry &registry,
                                                                                         BehaviorRuntimeLimits limits) {
        if (!registry.IsFrozen() || limits.maximumInstances == 0 || limits.maximumInstances > 16'384)
            return Result<std::shared_ptr<BehaviorRuntimeSceneOwner>>::Failure(
                BehaviorRestoreError("Registry must be frozen and lifecycle instance bound must be 1..16384."));
        auto state = std::make_shared<State>();
        state->scenes = &scenes;
        state->registry = &registry;
        state->limits = limits;
        return Result<std::shared_ptr<BehaviorRuntimeSceneOwner>>::Success(
            std::shared_ptr<BehaviorRuntimeSceneOwner>{new BehaviorRuntimeSceneOwner{std::move(state)}});
    }

    std::unique_ptr<Runtime::SceneActivationParticipant> BehaviorRuntimeSceneOwner::MakeActivationParticipant() {
        return BehaviorRuntimeRestoreAccess::Participant(state_);
    }

    Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>> BehaviorRuntimeSceneOwner::PrepareRestoreCandidate(
        Runtime::RuntimeScene &scene, BehaviorRuntimeServiceBindings services, std::shared_ptr<const GameplayPrefabContext> prefabs) {
        if (prefabs && !state_->scenes->IsRestoreCandidate(scene))
            return Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>>::Failure(
                BehaviorRestoreError("Prepared prefab services must belong to the exact actual unpublished Scene candidate."));
        if (state_->pending)
            return Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>>::Failure(
                BehaviorRestoreError("Another Scene candidate is pending."));
        auto prepared = BehaviorRuntimeRestoreAccess::Prepare(state_, scene, true, std::move(services), std::move(prefabs));
        if (prepared.HasValue())
            state_->pending = prepared.Value();
        return prepared;
    }

    void BehaviorRuntimeSceneOwner::CancelPrepared(Runtime::SceneRuntimeId scene) noexcept {
        if (auto pending = Prepared(scene)) {
            pending->Shutdown();
            state_->pending.reset();
        }
    }

    std::shared_ptr<BehaviorRuntimeRestoreCandidate> BehaviorRuntimeSceneOwner::Prepared(Runtime::SceneRuntimeId scene) const noexcept {
        if (!OnOwner(*state_) || !state_->pending || state_->pending->state_->scene->View().RuntimeId() != scene)
            return {};
        return state_->pending;
    }

    BehaviorRuntime *BehaviorRuntimeSceneOwner::Active() const noexcept {
        if (!OnOwner(*state_))
            return nullptr;
        const auto active = state_->active.lock();
        return active ? active->Runner() : nullptr;
    }

    Result<BehaviorPersistenceTarget> BehaviorRuntimeSceneOwner::BindingForActive(const BehaviorTypeId &type,
                                                                                  const Runtime::PersistentEntityIdentityMap &identities) {
        const auto active = state_->active.lock();
        if (!OnOwner(*state_) || !active || !active->Runner())
            return Result<BehaviorPersistenceTarget>::Failure(BehaviorRestoreError("No active candidate binding."));
        return active->Binding(type, identities);
    }

    Result<void> BehaviorRuntimeSceneOwner::ActivatePublished(BehaviorRuntimeServiceBindings services,
                                                              std::shared_ptr<const GameplayPrefabContext> prefabs) {
        const auto active = state_->active.lock();
        if (!OnOwner(*state_) || !active || active->state_->restoring || active->state_->ordinaryActivated)
            return Result<void>::Failure(BehaviorRestoreError("Ordinary activation is unavailable for this candidate."));
        if (auto configured =
                BehaviorRuntimeRestoreAccess::Configure(*active->state_->runtime, std::move(services), std::move(prefabs), state_->scenes);
            configured.HasError()) {
            active->Shutdown();
            return configured;
        }
        auto activated = BehaviorRuntimeRestoreAccess::Activate(*active->state_->runtime);
        if (activated.HasValue())
            active->state_->ordinaryActivated = true;
        else
            active->Shutdown();
        return activated;
    }

    Result<void> BehaviorRuntimeSceneOwner::QuiesceForReload() {
        const auto active = state_->active.lock();
        if (!OnOwner(*state_) || !active || !active->state_->runtime || state_->pending)
            return Result<void>::Failure(BehaviorRestoreError("Cannot quiesce while Scene restoration is pending or runtime is absent."));
        auto retired = state_->scenes->RetirePublishedRestorePreparation();
        if (retired.HasError())
            return Result<void>::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Result<void>::Failure(
                MakeError(GameplayErrors::GameplayReloadRestartRequired, "Published preparation still retains native owners."));
        if (active->state_->bindingEpoch == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(BehaviorRestoreError("Binding epoch is exhausted."));
        try {
            auto flags = BehaviorRuntimeRestoreAccess::LiveFlags(*active->state_->runtime);
            ++active->state_->bindingEpoch;
            active->state_->runtime->Shutdown();
            active->state_->runtime.reset();
            active->state_->generation.reset();
            active->state_->bindings.clear();
            active->state_->reloadFlags = std::move(flags);
            active->state_->registry = nullptr;
            state_->registry = nullptr;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreAllocationFailed));
        }
    }

    Result<void> BehaviorRuntimeSceneOwner::ResumeAfterReload(const BehaviorRegistry &registry,
                                                              const BehaviorRuntimeReloadSnapshot &snapshot,
                                                              BehaviorRuntimeServiceBindings services,
                                                              std::shared_ptr<const GameplayPrefabContext> prefabs) {
        const auto active = state_->active.lock();
        if (!OnOwner(*state_) || !active || active->state_->runtime || !state_->scenes->ActiveScene() ||
            state_->scenes->ActiveScene()->RuntimeId() != active->state_->scene->View().RuntimeId())
            return Result<void>::Failure(BehaviorRestoreError("Same-Scene reload slot is not quiesced/current."));
        auto prepared = BehaviorRuntimeRestoreAccess::PrepareReloadRunner(*active->state_->scene, registry, state_->limits, snapshot,
                                                                          active->state_->reloadFlags, std::move(services),
                                                                          std::move(prefabs), state_->scenes);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        auto runtime = std::move(prepared).Value();
        active->state_->generation = BehaviorRuntimeRestoreAccess::Generation(*runtime);
        active->state_->runtime = std::move(runtime);
        active->state_->registry = &registry;
        state_->registry = &registry;
        active->state_->ordinaryActivated = true;
        active->state_->restoring = false;
        return Result<void>::Success();
    }

    Result<void> BehaviorRuntimeSceneOwner::ValidateKnownNoServiceRestoreRequirements() const {
        if (!OnOwner(*state_))
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        const auto *runtime = Active();
        if (!runtime)
            return Result<void>::Failure(
                BehaviorRestoreError("No actual active behavior runtime is available for known requirement inspection."));
        return BehaviorRuntimeRestoreAccess::ValidateKnownNoServiceRequirements(*runtime);
    }

    Result<BehaviorLifecycleSnapshot> BehaviorRuntimeSceneOwner::CaptureLifecycle(
        const Runtime::PersistentEntityIdentityMap &identities) const {
        const auto *active = Active();
        if (!active)
            return Result<BehaviorLifecycleSnapshot>::Failure(BehaviorRestoreError("No actual active behavior runtime."));
        return BehaviorRuntimeRestoreAccess::Capture(*active, identities);
    }
}  // namespace Horo::Gameplay
