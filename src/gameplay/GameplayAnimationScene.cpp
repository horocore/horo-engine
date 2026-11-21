#include "Horo/Gameplay/GameplayAnimationScene.h"

#include "Horo/Animation/AnimationErrors.h"

#include <thread>
#include <type_traits>

namespace Horo::Gameplay {
    namespace A = Animation;
    namespace R = Runtime;

    namespace {
        /** @brief Projects a previously validated gameplay identity into Scene's exact entity address. */
        R::EntityRef ToRuntime(const GameplayEntityRef entity) noexcept {
            return {R::SceneRuntimeId{entity.scene}, R::EntityId{entity.index, entity.generation}};
        }

        /** @brief Preserves the Graph owner's typed failure at the gameplay boundary. */
        Result<void> AsResult(const A::GraphInstanceOutcome &outcome) {
            return outcome.HasValue() ? Result<void>::Success() : Result<void>::Failure(A::DescribeGraphInstanceFailure(outcome));
        }

        /** @brief Projects lightweight action failures into existing animation diagnostics. */
        const ErrorCodeDescriptor &ActionFailure(const A::GraphActionFailure failure) noexcept {
            using enum A::GraphActionFailure;
            using namespace A::AnimationErrors;
            switch (failure) {
                case Closed:
                case EvaluationPending:
                case NotTerminal:
                    return GraphAdmissionRejected;
                case ThreadViolation:
                    return PoseThreadViolation;
                case Malformed:
                    return HandleMalformed;
                case BindingMismatch:
                    return GraphBindingMismatch;
                case CapacityExceeded:
                    return GraphLimitExceeded;
                case A::GraphActionFailure::HandleStale:
                    return A::AnimationErrors::HandleStale;
                case None:
                    return GraphMalformed;
            }
            return GraphMalformed;
        }

        /** @brief Converts an action command outcome without creating diagnostics on success. */
        Result<void> AsResult(const A::GraphActionCommandOutcome &outcome) {
            return outcome.HasValue() ? Result<void>::Success() : Result<void>::Failure(MakeError(ActionFailure(outcome.failure)));
        }

        /** @brief Copies completed Scene facts into the module's view. */
        GameplayAnimationSnapshot Project(const GameplayEntityRef entity, const R::SceneAnimationObservation &observed) {
            GameplayAnimationSnapshot result;
            result.actor = {entity, observed.instance};
            result.state = observed.state;
            if (observed.component) {
                result.previous = observed.component->previousPose;
                result.current = observed.component->currentPose;
            }
            if (const auto &motion = observed.rootMotion) {
                result.rootMotion = GameplayRootMotionObservation{motion->request.id, motion->disposition, motion->request.delta,
                                                                  motion->applied, motion->remaining};
            }
            return result;
        }

        /** @brief Dispatches trusted module operations to the exact Scene owner. */
        class SceneGameplayAnimation final : public GameplayAnimationContext {
        public:
            SceneGameplayAnimation(std::weak_ptr<R::SceneAnimationRuntime> runtime, R::SceneRuntimeId scene, std::string module,
                                   const CancellationToken &cancellation)
                : runtime_(std::move(runtime)), scene_(scene), module_(std::move(module)), cancellation_(cancellation) {}

            std::uint64_t Scene() const noexcept override {
                return scene_.value;
            }

            std::string_view ModuleId() const noexcept override {
                return module_;
            }

            Result<GameplayAnimationSnapshot> Observe(const GameplayEntityRef entity) const override {
                const auto runtime = ResolveRuntime(entity);
                if (runtime.HasError())
                    return Result<GameplayAnimationSnapshot>::Failure(runtime.ErrorValue());
                const auto observed = runtime.Value()->Query(ToRuntime(entity));
                if (observed.HasError())
                    return Result<GameplayAnimationSnapshot>::Failure(observed.ErrorValue());
                return Result<GameplayAnimationSnapshot>::Success(Project(entity, observed.Value()));
            }

            Result<void> QueueParameter(const GameplayAnimationActor actor, const A::GraphParameterInput &parameter) const override {
                return WithActor(actor, [&parameter](auto &owner, const R::EntityRef entity) {
                    return AsResult(owner.QueueParameter(entity, parameter));
                });
            }

            Result<void> QueueTrigger(const GameplayAnimationActor actor, const A::GraphParameterId trigger) const override {
                return WithActor(actor, [trigger](auto &owner, const R::EntityRef entity) {
                    return AsResult(owner.QueueTrigger(entity, trigger));
                });
            }

            Result<A::GraphActionHandle> QueueAction(const GameplayAnimationActor actor,
                                                     const GameplayAnimationAction &request) const override {
                return WithActor(actor, [this, &request](auto &owner, const R::EntityRef entity) {
                    const auto outcome = owner.QueueAction(entity, {request.action, request.startSection, request.queue, cancellation_});
                    using Return = Result<A::GraphActionHandle>;
                    return outcome.HasValue() ? Return::Success(outcome.handle)
                                              : Return::Failure(MakeError(ActionFailure(outcome.failure)));
                });
            }

            Result<void> CancelAction(const GameplayAnimationActor actor, const A::GraphActionHandle &action,
                                      const A::GraphActionStopPolicy stop) const override {
                return WithActor(actor, [&action, stop](auto &owner, const R::EntityRef entity) {
                    return AsResult(owner.CancelAction(entity, action, stop));
                });
            }

            Result<A::GraphActionObservation> ObserveAction(const GameplayAnimationActor actor,
                                                            const A::GraphActionHandle &action) const override {
                return WithActor(actor, [&action](auto &owner, const R::EntityRef entity) {
                    const auto outcome = owner.QueryAction(entity, action);
                    using Return = Result<A::GraphActionObservation>;
                    return outcome.HasValue() ? Return::Success(outcome.observation)
                                              : Return::Failure(MakeError(ActionFailure(outcome.failure)));
                });
            }

            Result<void> ReleaseAction(const GameplayAnimationActor actor, const A::GraphActionHandle &action) const override {
                return WithActor(actor, [&action](auto &owner, const R::EntityRef entity) {
                    return AsResult(owner.ReleaseActionResult(entity, action));
                });
            }

            Result<void> CopyParameters(const GameplayAnimationActor actor, const A::PoseHandle &pose,
                                        const std::span<A::GraphParameterInput> output) const override {
                return WithActor(actor, [&pose, output](const auto &owner, const R::EntityRef entity) {
                    return AsResult(owner.CopyParameters(entity, pose, output));
                });
            }

            Result<std::uint32_t> CopyStateEvents(const GameplayAnimationActor actor, const A::PoseHandle &pose,
                                                  const std::span<A::GraphStateEvent> output) const override {
                return CopyEvents(actor, pose, output, &R::SceneAnimationRuntime::CopyStateEvents);
            }

            Result<std::uint32_t> CopyClipEvents(const GameplayAnimationActor actor, const A::PoseHandle &pose,
                                                 const std::span<A::AnimationEventOccurrence> output) const override {
                return CopyEvents(actor, pose, output, &R::SceneAnimationRuntime::CopyClipEvents);
            }

            Result<Math::Mat4> SocketModelTransform(const GameplayAnimationActor actor, const A::PoseHandle &pose,
                                                    const A::SkeletonSocketId socket) const override {
                return WithActor(actor, [&actor, &pose, socket](const auto &owner, const R::EntityRef entity) {
                    return owner.SocketModelTransform({entity, actor.instance, socket}, pose);
                });
            }

        private:
            /** @brief Verifies grant authority before pinning a live runtime for one call. */
            Result<std::shared_ptr<R::SceneAnimationRuntime>> ResolveRuntime(const GameplayEntityRef entity) const {
                using Return = Result<std::shared_ptr<R::SceneAnimationRuntime>>;
                using namespace A::AnimationErrors;
                if (owner_ != std::this_thread::get_id())
                    return Return::Failure(MakeError(PoseThreadViolation));
                if (cancellation_.IsCancellationRequested())
                    return Return::Failure(MakeError(GraphOperationCancelled));
                if (!entity.IsValid())
                    return Return::Failure(MakeError(HandleMalformed));
                if (entity.scene != Scene())
                    return Return::Failure(MakeError(HandleOwnerMismatch));
                auto runtime = runtime_.lock();
                if (!runtime)
                    return Return::Failure(MakeError(GraphAdmissionRejected));
                return Return::Success(std::move(runtime));
            }

            /** @brief Checks exact actor incarnation before synchronously invoking an owner operation. */
            template <typename Operation>
            auto WithActor(const GameplayAnimationActor actor, Operation operation) const
                -> std::invoke_result_t<Operation, R::SceneAnimationRuntime &, R::EntityRef> {
                using Return = std::invoke_result_t<Operation, R::SceneAnimationRuntime &, R::EntityRef>;
                const auto runtime = ResolveRuntime(actor.entity);
                if (runtime.HasError())
                    return Return::Failure(runtime.ErrorValue());
                if (!actor.instance.IsValid())
                    return Return::Failure(MakeError(A::AnimationErrors::HandleMalformed));
                const auto entity = ToRuntime(actor.entity);
                const auto observed = runtime.Value()->Query(entity);
                if (observed.HasError())
                    return Return::Failure(observed.ErrorValue());
                if (observed.Value().instance != actor.instance)
                    return Return::Failure(MakeError(A::AnimationErrors::HandleStale));
                return operation(*runtime.Value(), entity);
            }

            /** @brief Keeps event copy count and diagnostic handling identical for the committed producers. */
            template <typename Event, typename Copy>
            Result<std::uint32_t> CopyEvents(const GameplayAnimationActor actor, const A::PoseHandle &pose, std::span<Event> output,
                                             const Copy copy) const {
                return WithActor(actor, [&pose, output, copy](const auto &owner, const R::EntityRef entity) {
                    std::uint32_t written{};
                    const auto outcome = (owner.*copy)(entity, pose, output, written);
                    using Return = Result<std::uint32_t>;
                    return outcome.HasValue() ? Return::Success(written) : Return::Failure(A::DescribeGraphInstanceFailure(outcome));
                });
            }

            const std::thread::id owner_{std::this_thread::get_id()};
            std::weak_ptr<R::SceneAnimationRuntime> runtime_;
            R::SceneRuntimeId scene_;
            std::string module_;
            CancellationToken cancellation_;
        };
    }  // namespace

    /** @copydoc CreateGameplayAnimationContext */
    Result<std::shared_ptr<const GameplayAnimationContext>> CreateGameplayAnimationContext(std::weak_ptr<R::SceneAnimationRuntime> runtime,
                                                                                           const R::SceneRuntimeId scene,
                                                                                           std::string moduleId,
                                                                                           const CancellationToken &cancellation) {
        using Return = Result<std::shared_ptr<const GameplayAnimationContext>>;
        if (moduleId.empty() || !scene.IsValid())
            return Return::Failure(MakeError(A::AnimationErrors::HandleMalformed));
        if (cancellation.IsCancellationRequested())
            return Return::Failure(MakeError(A::AnimationErrors::GraphOperationCancelled));
        if (runtime.expired())
            return Return::Failure(MakeError(A::AnimationErrors::GraphAdmissionRejected));
        try {
            return Return::Success(std::make_shared<SceneGameplayAnimation>(std::move(runtime), scene, std::move(moduleId), cancellation));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(A::AnimationErrors::GraphLimitExceeded));
        }
    }
}  // namespace Horo::Gameplay
