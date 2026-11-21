#include "ScenePhysicsHost.h"

#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/SceneAnimationRuntime.h"

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Application::Internal {
    namespace {
        using Scope = Physics::Detail::PhysicsSceneHostScope;
        using FixedAccess = Character::Detail::CharacterSceneFixedAccess;
        using Attempt = Character::Detail::CharacterSceneTickAttempt;
        using QueryContext = Character::CharacterPhysicsQueryContext;

        /** @brief Matches a current authored actor to the controller published with its native aggregate. */
        Result<Character::CharacterControllerHandle> FindCharacterController(const Scope &scope, const Runtime::RuntimeEntityView &entity) {
            using Controller = Result<Character::CharacterControllerHandle>;
            const auto &component = entity.components->characterController;
            if (!entity.authoredObject || !component || !component->enabled)
                return Controller::Failure(MakeError(Physics::PhysicsErrors::HandleStale));
            const auto binding = std::ranges::find_if(scope.characterBindings, [&](const auto &row) {
                return row.owner == entity.entity && row.object == *entity.authoredObject && row.component == component->id;
            });
            return binding == scope.characterBindings.end() ? Controller::Failure(MakeError(Physics::PhysicsErrors::HandleStale))
                                                            : Controller::Success(binding->handle);
        }

        /** @brief Admits the same previous native publication for ordinary and staged Character ticks. */
        Result<QueryContext> PrepareCharacterQuery(const Scope &scope, Character::CharacterPhysicsQueryAdapter &adapter,
                                                   std::uint64_t tick) {
            if (const auto processed = scope.physics.ProcessQueryBatch(); processed.HasError())
                return Result<QueryContext>::Failure(processed.ErrorValue());
            const auto current = scope.physics.PublishedTick();
            const auto &descriptor = scope.character.Descriptor();
            if (current.publicationRevision != 0) {
                if (const auto refreshed = scope.character.RefreshPhysicsSnapshot(scope.physics.Identity(), current.publicationRevision);
                    refreshed.HasError())
                    return Result<QueryContext>::Failure(refreshed.ErrorValue());
            } else if (current.completedTick != 0 || scope.character.PublishedTick().completedTick != 0 ||
                       descriptor.sceneGeneration != scope.scene.value || descriptor.physicsWorld != scope.physics.Identity()) {
                return Result<QueryContext>::Failure(
                    MakeError(Physics::PhysicsErrors::QuerySnapshotStale,
                              "Only the exact unticked Scene-owned pair may use its prepared Character query context."));
            }
            return Result<QueryContext>::Success(adapter.Context({descriptor.sceneGeneration, descriptor.identity, descriptor.physicsWorld,
                                                                  descriptor.collisionFilterGeneration, descriptor.originGeneration, tick,
                                                                  descriptor.physicsSnapshotRevision}));
        }

        /** @brief Checks the actor's declared policy before calling the Animation owner. */
        bool RequestsCharacterMotion(const Runtime::RuntimeEntityView &actor) noexcept {
            return actor.components && actor.components->animation &&
                   actor.components->animation->rootMotion == Animation::RootMotionPolicy::RequestCharacterMovement;
        }

        /** @brief Accumulates the captured authored ancestors from the immediate parent outward. */
        Result<Math::Mat4> CharacterParentMatrix(const Runtime::RuntimeSceneView &scene, std::optional<Runtime::EntityRef> parent) {
            using Matrix = Result<Math::Mat4>;
            auto world = Math::Mat4::Identity();
            auto remaining = scene.SlotCount();
            while (parent) {
                if (remaining-- == 0)
                    return Matrix::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
                const auto ancestor = scene.Get(*parent);
                if (ancestor.HasError())
                    return Matrix::Failure(ancestor.ErrorValue());
                const auto matrix = ancestor.Value().localTransform->TryToMatrix();
                if (matrix.HasError())
                    return Matrix::Failure(matrix.ErrorValue());
                world = Math::Multiply(matrix.Value(), world);
                parent = ancestor.Value().parent;
            }
            return Matrix::Success(world);
        }

        /** @brief Converts committed Character world coordinates without taking ownership of local scale. */
        Result<Math::Transform> CharacterLocalPose(const Runtime::RuntimeSceneView &scene, const Runtime::RuntimeEntityView &actor,
                                                   const Character::CharacterTransformPublication &collisionRoot) {
            using Pose = Result<Math::Transform>;
            if (!actor.parent)
                return Pose::Success({collisionRoot.position, collisionRoot.heading, actor.localTransform->scale});
            const auto matrix = CharacterParentMatrix(scene, actor.parent);
            if (matrix.HasError())
                return Pose::Failure(matrix.ErrorValue());
            const auto inverse = Math::TryInverseAffine(matrix.Value());
            if (inverse.HasError())
                return Pose::Failure(inverse.ErrorValue());
            const auto parent = Math::TryDecomposeAffineTRS(matrix.Value());
            if (parent.HasError())
                return Pose::Failure(parent.ErrorValue());
            const auto position = Math::TryTransformPoint(inverse.Value(), collisionRoot.position);
            if (position.HasError())
                return Pose::Failure(position.ErrorValue());
            return Pose::Success(
                {position.Value(), (parent.Value().rotation.Inverse() * collisionRoot.heading).Normalized(), actor.localTransform->scale});
        }
    }  // namespace

    /** @brief Bounded per-host input/receipt storage and the exact native attempt retained by the scheduler. */
    struct ScenePhysicsHost::AnimationFixedState final {
        enum class NativeStep {
            Pending,
            Attempted,
            Completed
        };

        struct Receipt final {
            Character::CharacterControllerHandle controller;
            Runtime::SceneRootMotionRequest request;
        };

        struct Pending final {
            Runtime::SceneRuntimeId scene;
            Attempt attempt;
            NativeStep step{NativeStep::Pending};
        };

        struct Committed final {
            Runtime::SceneRuntimeId scene;
            std::uint64_t localTick{};
            std::uint64_t schedulerTick{};
        };

        std::vector<Character::Detail::CharacterSceneRootMotion> roots;
        std::vector<Receipt> receipts;
        std::optional<Pending> pending;
        std::optional<Committed> committed;
        std::optional<Runtime::SceneRuntimeId> failedScene;

        explicit AnimationFixedState(std::size_t capacity) {
            roots.reserve(capacity);
            receipts.reserve(capacity);
        }

        /** @brief Excludes overlap with a pending tick or an irreversible failed native step. */
        bool MayStart(Runtime::SceneRuntimeId scene) const noexcept {
            return !pending && failedScene != scene;
        }

        /** @brief Matches the requested scheduler tick and both native publications to the same authority. */
        bool HasCommittedPose(const Scope &scope, const Runtime::RuntimeSceneView &scene, const std::uint64_t tick) const noexcept {
            return committed && scene.IsCurrent() && committed->scene == scope.scene && scene.RuntimeId() == scope.scene &&
                   committed->schedulerTick == tick && scope.character.PublishedTick().completedTick == committed->localTick &&
                   scope.physics.PublishedTick().completedTick == committed->localTick;
        }

        /** @brief Copies current root requests only from entities with a resident controller. */
        Result<void> Collect(const Scope &scope, const Runtime::RuntimeSceneView &scene, Runtime::SceneAnimationRuntime &animation,
                             const Runtime::FixedStepContext &context) {
            roots.clear();
            receipts.clear();
            if (scope.characterBindings.size() > std::min(roots.capacity(), receipts.capacity()))
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
            for (const auto &binding : scope.characterBindings) {
                const auto entity = scene.Find(binding.object);
                if (!entity)
                    continue;
                const auto actor = scene.Get(*entity);
                if (actor.HasError())
                    return Result<void>::Failure(actor.ErrorValue());
                if (!RequestsCharacterMotion(actor.Value()))
                    continue;
                const auto claimed = animation.ClaimRootMotion(*entity, context);
                if (claimed.HasError())
                    return Result<void>::Failure(claimed.ErrorValue());
                if (const auto &request = claimed.Value(); request) {
                    roots.push_back({binding.handle,
                                     {{request->id.tick.Value(), request->id.generation.Value()},
                                      {request->delta.translation, request->delta.rotation},
                                      Character::CharacterRootTranslation::Suggest,
                                      Character::CharacterRootRotation::Additive}});
                    receipts.emplace_back(binding.handle, *request);
                }
            }
            std::ranges::sort(roots, {}, &Character::Detail::CharacterSceneRootMotion::controller);
            return Result<void>::Success();
        }

        /** @brief Returns preflighted solver feedback to the original claimed animation instance. */
        Result<void> Resolve(const Scope &scope, Runtime::SceneAnimationRuntime &animation, const Runtime::FixedStepContext &context,
                             Attempt attempt) const {
            for (const auto &receipt : receipts) {
                const auto feedback = FixedAccess::ReadRootMotion(scope.character, attempt, receipt.controller);
                if (feedback.HasError())
                    return Result<void>::Failure(feedback.ErrorValue());
                if (!feedback.Value())
                    return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
                const auto &applied = feedback.Value()->applied;
                if (auto resolved =
                        animation.ResolveRootMotion(receipt.request.entity, receipt.request.id,
                                                    Animation::AnimationRootMotionDelta{applied.translation, applied.rotation}, context);
                    resolved.HasError())
                    return resolved;
            }
            return Result<void>::Success();
        }
    };

    /** @copydoc ParseScenePhysicsHostMode */
    std::optional<Physics::PhysicsRuntimeMode> ParseScenePhysicsHostMode(const std::string_view name) noexcept {
        if (name == "null")
            return Physics::PhysicsRuntimeMode::Null;
        if (name == "canonical")
            return Physics::PhysicsRuntimeMode::Canonical;
        return std::nullopt;
    }

    /** @copydoc ScenePhysicsHostModeName */
    std::string_view ScenePhysicsHostModeName(const Physics::PhysicsRuntimeMode mode) noexcept {
        switch (mode) {
            case Physics::PhysicsRuntimeMode::Null:
                return "null";
            case Physics::PhysicsRuntimeMode::Canonical:
                return "canonical";
        }
        return {};
    }

    /** @copydoc ParseSceneHostAbsolutePath */
    Result<std::filesystem::path> ParseSceneHostAbsolutePath(const std::string_view value) {
        const auto invalid = [] {
            return Result<std::filesystem::path>::Failure(
                MakeError(Physics::PhysicsErrors::DescriptorInvalid, "Supply a valid UTF-8 absolute Scene project/cook root."));
        };
        try {
            const std::filesystem::path path{std::u8string{value.begin(), value.end()}};
            if (!path.is_absolute())
                return invalid();
            return Result<std::filesystem::path>::Success(path.lexically_normal());
        } catch (const std::filesystem::filesystem_error &) {
            return invalid();
        }
    }

    /** @brief Forwards the real candidate's metadata while retaining its sole Scene ownership. */
    class ScenePhysicsHost::Candidate final : public Runtime::SceneActivationCandidate {
    public:
        Candidate(ScenePhysicsHost &host, std::unique_ptr<Runtime::SceneActivationCandidate> candidate,
                  Runtime::RuntimeSceneView scene) noexcept
            : host_(host), candidate_(std::move(candidate)),
              physics_(static_cast<Physics::PhysicsSceneActivationCandidate *>(candidate_.get())), scene_(std::move(scene)) {
            host_.pending_ = physics_;
        }

        ~Candidate() override {
            Shutdown();
        }

        Candidate(const Candidate &) = delete;
        Candidate &operator=(const Candidate &) = delete;
        Candidate(Candidate &&) = delete;
        Candidate &operator=(Candidate &&) = delete;

        Result<void> ValidatePublication() const override {
            return candidate_->ValidatePublication();
        }

        Runtime::SceneCanonicalDatasetProjection CanonicalDatasetProjection() const noexcept override {
            return candidate_->CanonicalDatasetProjection();
        }

        std::span<const Runtime::SceneInitialPlacement> InitialPlacements() const noexcept override {
            return candidate_->InitialPlacements();
        }

        void Publish() noexcept override {
            if (host_.pending_ == physics_)
                host_.pending_ = nullptr;
            candidate_->Publish();
            host_.published_ = physics_;
            host_.publishedScene_ = scene_;
        }

        void Shutdown() noexcept override {
            // Replacement publishes the new root before retiring the old one. Old teardown
            // must never withdraw the replacement's observer.
            if (host_.published_ == physics_) {
                host_.published_ = nullptr;
                host_.publishedScene_ = {};
            }
            if (host_.pending_ == physics_)
                host_.pending_ = nullptr;
            candidate_->Shutdown();
        }

    private:
        ScenePhysicsHost &host_;
        std::unique_ptr<Runtime::SceneActivationCandidate> candidate_;
        Physics::PhysicsSceneActivationCandidate *physics_;
        Runtime::RuntimeSceneView scene_;
    };

    /** @brief Wraps only the concrete explicit participant constructed here; it cannot accept a foreign candidate type. */
    class ScenePhysicsHost::Participant final : public Runtime::SceneActivationParticipant {
    public:
        Participant(ScenePhysicsHost &host, Physics::PhysicsRuntime &runtime, Physics::PhysicsSceneActivationAuthority &authority,
                    Physics::PhysicsSceneActivationSettings settings)
            : host_(host), owner_(runtime, authority, std::move(settings)) {}

        ~Participant() override {
            if (host_.participant_ == this)
                host_.participant_ = nullptr;
        }

        Participant(const Participant &) = delete;
        Participant &operator=(const Participant &) = delete;
        Participant(Participant &&) = delete;
        Participant &operator=(Participant &&) = delete;

        std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant() {
            return owner_.MakeStructuralParticipant();
        }

        Runtime::SceneActivationPhase PreparationPhase() const noexcept override {
            return owner_.PreparationPhase();
        }

        Result<void> PrepareOwnedScene(Runtime::RuntimeScene &scene) override {
            if (const auto identity = scene.View().RuntimeId().value;
                host_.identities_.first.value == 0 || host_.identities_.last.value < host_.identities_.first.value ||
                identity < host_.identities_.first.value || identity > host_.identities_.last.value)
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState,
                                                       "Scene identity is outside the application's reserved Physics identity range."));
            return owner_.PrepareOwnedScene(scene);
        }

        Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                           const Runtime::RuntimeSceneView &scene) override {
            auto candidate = owner_.Prepare(definition, scene);
            if (candidate.HasError())
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Failure(candidate.ErrorValue());
            try {
                return Result<std::unique_ptr<Runtime::SceneActivationCandidate>>::Success(
                    std::make_unique<Candidate>(host_, std::move(candidate).Value(), scene));
            } catch (const std::bad_alloc &) {
                // Construction is load-time; explicit retirement precedes propagation if wrapping
                // allocation fails. The real Physics candidate remains the unique native owner.
                if (candidate.Value())
                    candidate.Value()->Shutdown();
                throw;
            }
        }

    private:
        ScenePhysicsHost &host_;
        Physics::PhysicsSceneActivationParticipant owner_;
    };

    /** @copydoc ScenePhysicsHost::ScenePhysicsHost */
    ScenePhysicsHost::ScenePhysicsHost(const ScenePhysicsIdentityRange identities) : identities_(identities) {}

    ScenePhysicsHost::~ScenePhysicsHost() = default;

    /** @copydoc ScenePhysicsHost::MakeParticipant */
    Result<std::unique_ptr<Runtime::SceneActivationParticipant>> ScenePhysicsHost::MakeParticipant(
        Physics::PhysicsRuntime &runtime, Physics::PhysicsSceneActivationAuthority &authority,
        Physics::PhysicsSceneActivationSettings settings) {
        if (std::this_thread::get_id() != owner_)
            return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (participant_)
            return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::InvalidState, "This Scene host already has its Physics participant."));
        try {
            auto fixed = std::make_unique<AnimationFixedState>(settings.character.Values().capacities.maximumControllers);
            auto participant = std::make_unique<Participant>(*this, runtime, authority, std::move(settings));
            fixedAnimation_ = std::move(fixed);
            participant_ = participant.get();
            return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Success(std::move(participant));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<Runtime::SceneActivationParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc ScenePhysicsHost::MakeStructuralParticipant */
    Result<std::unique_ptr<Runtime::SceneStructuralParticipant>> ScenePhysicsHost::MakeStructuralParticipant() {
        if (std::this_thread::get_id() != owner_)
            return Result<std::unique_ptr<Runtime::SceneStructuralParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (!participant_)
            return Result<std::unique_ptr<Runtime::SceneStructuralParticipant>>::Failure(
                MakeError(Physics::PhysicsErrors::InvalidState,
                          "Register the Scene host Physics participant before its structural adapter."));
        return Result<std::unique_ptr<Runtime::SceneStructuralParticipant>>::Success(participant_->MakeStructuralParticipant());
    }

    /** @copydoc ScenePhysicsHost::AdvanceFixedTick */
    Result<void> ScenePhysicsHost::AdvanceFixedTick(const Duration delta) {
        auto borrowed = Borrow();
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        auto &scope = borrowed.Value();
        if (delta <= Duration{})
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid, "A host fixed quantum must be positive."));
        // Explicit Null composition omits both simulation stages; it must not publish synthetic native ticks.
        if (scope.physics.State() == Physics::PhysicsWorldState::ActiveNull)
            return Result<void>::Success();
        const auto publication = scope.physics.PublishedTick();
        if (publication.completedTick == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));
        const auto tick = publication.completedTick + 1;
        if (!fixedAnimation_->MayStart(scope.scene))
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
        Character::CharacterPhysicsQueryAdapter adapter{scope.physics};
        const auto query = PrepareCharacterQuery(scope, adapter, tick);
        if (query.HasError())
            return Result<void>::Failure(query.ErrorValue());
        if (const auto advanced = scope.character.AdvanceFixedTick(
                {.tick = tick, .sceneGeneration = scope.scene.value, .fixedDelta = delta, .query = query.Value()});
            advanced.HasError())
            return advanced;
        return scope.physics.AdvanceFixedTick({.simulationTick = tick, .sceneGeneration = scope.scene.value, .fixedDelta = delta});
    }

    /** @copydoc ScenePhysicsHost::StageAnimationFixedTick */
    Result<void> ScenePhysicsHost::StageAnimationFixedTick(const Runtime::RuntimeSceneView &scene,
                                                           Runtime::SceneAnimationRuntime &animation,
                                                           const Runtime::FixedStepContext &context) {
        auto borrowed = Borrow();
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        auto &scope = borrowed.Value();
        auto &fixed = *fixedAnimation_;
        if (!scene.IsCurrent() || scene.RuntimeId() != scope.scene || !fixed.MayStart(scope.scene))
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
        if (scope.physics.State() == Physics::PhysicsWorldState::ActiveNull)
            return Result<void>::Success();
        const auto previous = scope.physics.PublishedTick().completedTick;
        if (previous == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::GenerationExhausted));

        const Attempt attempt{previous + 1, context.attemptNumber};
        Character::CharacterPhysicsQueryAdapter adapter{scope.physics};
        const auto query = PrepareCharacterQuery(scope, adapter, attempt.tick);
        if (query.HasError())
            return Result<void>::Failure(query.ErrorValue());
        if (const auto collected = fixed.Collect(scope, scene, animation, context); collected.HasError())
            return collected;
        if (const auto staged = FixedAccess::Stage(scope.character, attempt,
                                                   {.delta = context.fixedDelta, .query = query.Value(), .rootMotion = fixed.roots});
            staged.HasError())
            return staged;
        fixed.pending = AnimationFixedState::Pending{scope.scene, attempt};
        return fixed.Resolve(scope, animation, context, attempt);
    }

    /** @copydoc ScenePhysicsHost::PrepareAnimationFixedCommit */
    Result<void> ScenePhysicsHost::PrepareAnimationFixedCommit(const Runtime::FixedStepContext &context) {
        if (!fixedAnimation_ || !fixedAnimation_->pending)
            return Result<void>::Success();
        auto &pending = *fixedAnimation_->pending;
        auto borrowed = Borrow();
        if (borrowed.HasError())
            return Result<void>::Failure(borrowed.ErrorValue());
        auto &scope = borrowed.Value();
        using Step = AnimationFixedState::NativeStep;
        if (pending.attempt.attempt != context.attemptNumber || pending.scene != scope.scene || pending.step != Step::Pending)
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState));
        if (context.cancellation.IsCancellationRequested())
            return JobCancelled();
        pending.step = Step::Attempted;
        auto stepped = scope.physics.AdvanceFixedTick(
            {.simulationTick = pending.attempt.tick, .sceneGeneration = scope.scene.value, .fixedDelta = context.fixedDelta});
        if (stepped.HasValue())
            pending.step = Step::Completed;
        return stepped;
    }

    /** @copydoc ScenePhysicsHost::CommitAnimationFixedTick */
    void ScenePhysicsHost::CommitAnimationFixedTick(const Runtime::FixedStepContext &context) noexcept {
        if (!fixedAnimation_ || !fixedAnimation_->pending)
            return;
        const auto pending = *fixedAnimation_->pending;
        auto scope = Borrow();
        if (pending.attempt.attempt != context.attemptNumber || pending.step != AnimationFixedState::NativeStep::Completed ||
            scope.HasError() || pending.scene != scope.Value().scene)
            std::terminate();
        FixedAccess::Commit(scope.Value().character, pending.attempt);
        fixedAnimation_->committed = AnimationFixedState::Committed{pending.scene, pending.attempt.tick, context.simulationTick};
        fixedAnimation_->pending.reset();
    }

    /** @copydoc ScenePhysicsHost::AbortAnimationFixedTick */
    void ScenePhysicsHost::AbortAnimationFixedTick(const Runtime::FixedStepContext &context) noexcept {
        if (!fixedAnimation_ || !fixedAnimation_->pending || fixedAnimation_->pending->attempt.attempt != context.attemptNumber)
            return;
        const auto pending = *fixedAnimation_->pending;
        fixedAnimation_->pending.reset();
        if (pending.step != AnimationFixedState::NativeStep::Pending)
            fixedAnimation_->failedScene = pending.scene;
        if (auto scope = Borrow(); scope.HasValue() && pending.scene == scope.Value().scene)
            FixedAccess::Abort(scope.Value().character, pending.attempt);
    }

    /** @copydoc ScenePhysicsHost::HasPublishedScene */
    bool ScenePhysicsHost::HasPublishedScene() const noexcept {
        return std::this_thread::get_id() == owner_ && published_ != nullptr;
    }

    /** @copydoc ScenePhysicsHost::Borrow */
    Result<Physics::Detail::PhysicsSceneHostScope> ScenePhysicsHost::Borrow() {
        if (std::this_thread::get_id() != owner_)
            return Result<Physics::Detail::PhysicsSceneHostScope>::Failure(MakeError(Physics::PhysicsErrors::ThreadAffinityViolation));
        if (!published_)
            return Result<Physics::Detail::PhysicsSceneHostScope>::Failure(
                MakeError(Physics::PhysicsErrors::InvalidState, "No Scene-owned Physics aggregate is published."));
        return Physics::Detail::PhysicsSceneHostAccess::Borrow(*published_);
    }

    /** @copydoc ScenePhysicsHost::ResolveCharacterController */
    Result<Character::CharacterControllerHandle> ScenePhysicsHost::ResolveCharacterController(const Runtime::RuntimeSceneView &scene,
                                                                                              const Runtime::EntityRef actor) {
        using Controller = Result<Character::CharacterControllerHandle>;
        const auto borrowed = Borrow();
        if (borrowed.HasError())
            return Controller::Failure(borrowed.ErrorValue());
        const auto &scope = borrowed.Value();
        if (!scene.IsCurrent() || !scene.SameIncarnation(publishedScene_) || actor.runtime != scope.scene)
            return Controller::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        const auto entity = scene.Get(actor);
        if (entity.HasError())
            return Controller::Failure(entity.ErrorValue());
        return FindCharacterController(scope, entity.Value());
    }

    /** @copydoc ScenePhysicsHost::ReadCharacterLocalTransform */
    Result<Math::Transform> ScenePhysicsHost::ReadCharacterLocalTransform(const Runtime::RuntimeSceneView &scene,
                                                                          const Runtime::RuntimeEntityView &entity,
                                                                          const std::uint64_t schedulerTick) {
        using Pose = Result<Math::Transform>;
        const auto borrowed = Borrow();
        if (borrowed.HasError())
            return Pose::Failure(borrowed.ErrorValue());
        const auto &scope = borrowed.Value();
        if (!scene.SameIncarnation(publishedScene_) || !fixedAnimation_->HasCommittedPose(scope, scene, schedulerTick))
            return Pose::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        const auto actor = scene.Get(entity.entity);
        if (actor.HasError())
            return Pose::Failure(actor.ErrorValue());
        const auto controller = FindCharacterController(scope, actor.Value());
        if (controller.HasError())
            return Pose::Failure(controller.ErrorValue());
        const auto collisionRoot = scope.character.ControllerTransform(controller.Value());
        if (collisionRoot.HasError())
            return Pose::Failure(collisionRoot.ErrorValue());
        return CharacterLocalPose(scene, actor.Value(), collisionRoot.Value());
    }

    /** @copydoc ScenePhysicsHost::ReadCharacterVelocity */
    Result<Math::Vec3> ScenePhysicsHost::ReadCharacterVelocity(Runtime::SceneRuntimeId scene, Runtime::SceneObjectId object,
                                                               Runtime::PhysicsComponentId component) {
        using Velocity = Result<Math::Vec3>;
        auto scope = Borrow();
        if (scope.HasError())
            return Velocity::Failure(scope.ErrorValue());
        if (scope.Value().scene != scene)
            return Velocity::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        const auto bindings = scope.Value().characterBindings;
        const auto binding = std::ranges::find_if(bindings, [object, component](const auto &row) {
            return row.object == object && row.component == component;
        });
        if (binding == bindings.end())
            return Velocity::Failure(MakeError(Physics::PhysicsErrors::HandleStale));
        const auto &character = scope.Value().character;
        const auto completed = character.PublishedTick().completedTick;
        if (completed == 0 || scope.Value().physics.PublishedTick().completedTick != completed)
            return Velocity::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        const auto transform = character.ControllerTransform(binding->handle);
        if (transform.HasError())
            return Velocity::Failure(transform.ErrorValue());
        // A resident controller with no movement publication for a completed tick did not advance in that tick.
        if (transform.Value().sourceTick < completed)
            return Velocity::Success({});
        const auto snapshot = character.ControllerLocomotionSnapshot(binding->handle);
        if (snapshot.HasError())
            return Velocity::Failure(snapshot.ErrorValue());
        if (snapshot.Value().tick != completed || transform.Value().sourceTick != completed)
            return Velocity::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        return Velocity::Success(snapshot.Value().movement.achievedVelocityMetersPerSecond);
    }
}  // namespace Horo::Application::Internal
