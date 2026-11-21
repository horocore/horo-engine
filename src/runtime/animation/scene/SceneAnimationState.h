#pragma once

#include "SceneAnimationAssets.h"

#include <thread>

namespace Horo::Runtime {
    // Private shared ownership is acyclic: candidates pin State and Scope; neither retains its participants/candidates.
    struct SceneAnimationRuntime::State final {
        struct PendingRootMotion final {
            SceneRootMotionResult result;
            bool claimed{};
        };

        struct Actor final {
            AnimationSceneInternal::PreparedAssets assets;
            Animation::AnimationGraphInstance graph;
            EntityRef entity;
            Animation::AnimationAuthoringComponent authoring;
            Animation::AnimationInstanceHandle instance;
            std::vector<Animation::GraphParameterInput> inputStorage;
            SceneAnimationInputSnapshot capturedInput;
            std::optional<PendingRootMotion> pendingMotion;
            std::optional<SceneRootMotionResult> publishedMotion;
            bool playing{};
            bool staged{};
            Actor(AnimationSceneInternal::PreparedAssets prepared, Animation::AnimationGraphInstance instanceOwner, EntityRef owner,
                  Animation::AnimationAuthoringComponent component, Animation::AnimationInstanceHandle identity, std::size_t inputCapacity);
        };

        struct Set final {
            std::vector<std::shared_ptr<Actor>> actors;
        };

        struct Scope final {
            std::unique_ptr<Set> current;
            SceneRuntimeId scene;
            std::uint64_t revision{1};
            bool closed{};
            void Close() noexcept;
        };
        class Activation;
        class Structural;
        class ActivationCandidate;
        class StructuralCandidate;

        explicit State(SceneAnimationRuntimeConfig value);
        [[nodiscard]] bool OnOwner() const noexcept;
        [[nodiscard]] Result<void> Admit(bool controls = false) const;
        [[nodiscard]] std::shared_ptr<Actor> Find(EntityRef entity) const noexcept;
        /** @brief Checks owner admission before borrowing one exact active actor for a synchronous observation. */
        [[nodiscard]] std::pair<const Actor *, Animation::GraphInstanceFailure> ResolveObservationActor(EntityRef entity) const noexcept;
        [[nodiscard]] bool CurrentScene(const RuntimeSceneView &scene) const noexcept;
        [[nodiscard]] Result<std::shared_ptr<Actor>> PrepareActor(const RuntimeSceneView &scene, const RuntimeEntityView &entity,
                                                                  std::uint32_t slot);
        [[nodiscard]] Result<std::shared_ptr<Scope>> PrepareScope(const RuntimeSceneView &scene);
        [[nodiscard]] Result<std::unique_ptr<Set>> PrepareChange(const RuntimeSceneView &scene, std::span<const RuntimeEntityView> created,
                                                                 std::span<const EntityRef> destroyed);
        [[nodiscard]] Result<void> AddActor(Set &set, const RuntimeSceneView &scene, const RuntimeEntityView &entity);
        [[nodiscard]] bool ReadAttempt(const FixedStepContext &context, RuntimeDispatchFacts &facts) const noexcept;
        [[nodiscard]] Result<void> BeginAttempt(const FixedStepContext &context);
        /** @brief Copies every playing actor's bounded input on the owner lane before graph evaluation. */
        [[nodiscard]] Result<void> CaptureInputs(const FixedStepContext &context) const;
        /** @brief Stages one actor per assigned slot using inputs captured by the Scene owner. */
        [[nodiscard]] static Result<void> EvaluateRange(std::span<const std::shared_ptr<Actor>> actors,
                                                        const Animation::GraphEvaluationInput &batch, const CancellationToken &work);
        /** @brief Evaluates and joins all assigned slots before releasing the owner lane. */
        [[nodiscard]] Result<void> EvaluateActors(const FixedStepContext &context) const;
        /** @brief Captures movement requests only after the complete owner-lane candidate is evaluated. */
        [[nodiscard]] Result<void> StageRootMotion(const FixedStepContext &context) const;
        /** @brief Validates exact host callback authority before accessing a pending motion receipt. */
        [[nodiscard]] Result<void> AdmitMotionCallback(const FixedStepContext &context) const;
        [[nodiscard]] Result<void> PrepareAttempt(const FixedStepContext &context);
        [[nodiscard]] Result<void> ArmActors() const;
        void PublishActors() const noexcept;
        void AbortActors() const noexcept;
        void PublishAttempt(const FixedStepContext &context) noexcept;
        void AbortAttempt(const FixedStepContext &context) noexcept;
        void ClearAttempt() noexcept;
        void Close() noexcept;

        SceneAnimationRuntimeConfig config;
        const std::thread::id owner{std::this_thread::get_id()};
        AnimationSceneInternal::AssetGenerations assetGenerations;
        std::shared_ptr<Scope> active;
        std::uint32_t nextGeneration{1};
        RuntimeDispatchEvidence attemptEvidence;
        Animation::AnimationTickId attemptTick;
        bool started{};
        bool closed{};
        bool attempting{};
        bool armed{};
    };
}  // namespace Horo::Runtime
