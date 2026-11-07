#pragma once
/** @file EditorPlaySaveHost.h
 * @brief Explicit application composition of one isolated Play save session and its worker-owned sandbox.
 */
#include "EditorPlaySaveHostStorage.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Gameplay/BehaviorRuntimeRestore.h"
#include "Horo/Runtime/Save/SaveRootResolver.h"

#include <functional>

namespace Horo::Application::Internal {
    /** @brief Actual process services and immutable scope selected before activation; every service outlives settlement. */
    struct EditorPlaySaveEnvironment final {
        JobSystem &jobs;
        OperationStore &operations;
        Clock &clock;
        WallClock &wallClock;
        PlaySaveStorageConfig storage;
    };

    class PlaySavePrefabServices;
    class PlaySavePhysicsServices;
    class EditorPlaySaveHost;
    /** @brief Cold host factory; every invocation issues a new sandbox namespace and actual authored identity inventory. */
    using EditorPlaySaveFactory = std::function<
        Result<std::unique_ptr<EditorPlaySaveHost>>(Runtime::SaveProjectId, Runtime::RuntimeSceneService &,
                                                    const Runtime::RuntimeSceneDefinition &, std::vector<Editor::PlaySaveGameplayBinding>,
                                                    std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner>,
                                                    std::shared_ptr<PlaySavePrefabServices>, std::shared_ptr<PlaySavePhysicsServices>)>;

    /** @brief Single owner-lane runtime save composition. Workers never retain this object or a live Scene borrow. */
    class EditorPlaySaveHost final : public Editor::EditorPlaySaveSession {
    public:
        /** @brief Composes genuine registered sources and opens the sandbox asynchronously after real Scene activation.
         * @param scenes Existing isolated Play Scene owner. @param environment Actual lifetime-pinned process services.
         * @param schema Frozen baseline, durable identities and inert gameplay declarations.
         * @param liveBindings Current privately issued native installations, retained separately from worker policy.
         * @return Owned session or original typed composition/scheduler/coverage failure. No storage I/O runs here.
         */
        [[nodiscard]] static Result<std::unique_ptr<EditorPlaySaveHost>> Create(
            Runtime::RuntimeSceneService &scenes, EditorPlaySaveEnvironment environment,
            std::shared_ptr<const PlaySaveStorageSchema> schema,
            std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings = {},
            std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner = {}, std::shared_ptr<PlaySavePrefabServices> prefabs = {},
            std::shared_ptr<PlaySavePhysicsServices> physics = {});
        ~EditorPlaySaveHost() override;
        [[nodiscard]] std::shared_ptr<const Editor::PlaySaveSnapshot> Snapshot() const noexcept override;
        [[nodiscard]] Result<void> Request(const Editor::PlaySaveCommand &command) override;
        [[nodiscard]] Result<void> CommitAtSafePoint(const Runtime::FrameContext &frame) override;
        void BeginRetirement() noexcept override;
        [[nodiscard]] bool IsSettled() const noexcept override;
        /** @brief Reports canonical save-owner retirement; synchronous Play mutations may continue without reopening capture. */
        [[nodiscard]] bool IsRetiring() const noexcept;
        /** @brief Cancels and drains only module-bound runtime work, retaining this Play namespace and its slots. */
        [[nodiscard]] bool SuspendForModuleReload() noexcept;
        /** @brief Rebinds real same-schema installed sources after suspension; existing slots and scope remain unchanged. */
        [[nodiscard]] Result<void> RebindGameplay(std::vector<Editor::PlaySaveGameplayBinding> bindings);
        /** @brief Reports actual current runtime-binding admission, independently of value-only storage lifetime. */
        [[nodiscard]] bool HasRuntimeBinding() const noexcept;
        /** @brief Invalidates readiness before synchronous Play state producers run; no callbacks may capture during mutation. */
        [[nodiscard]] Result<void> BeginMutation();
        /** @brief Closes actual synchronous producer writes and publishes their common semantic epoch. */
        [[nodiscard]] Result<void> EndMutation();

    private:
        struct State;
        explicit EditorPlaySaveHost(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    /** @brief Creates the explicitly scoped application factory using the existing root resolver and native entropy.
     * @param jobs Actual scheduler. @param operations Process operation authority. @param clock Monotonic host clock.
     * @param wallClock Host UTC clock. @param processes Real host environment capability, borrowed only during cold root resolution.
     * @param platform Exact compile-target OS convention. @param entropy Genuine supplied random owner.
     * @return Factory or original root/configuration error. No production namespace or authoring file is written.
     */
    [[nodiscard]] Result<EditorPlaySaveFactory> MakeEditorPlaySaveFactory(JobSystem &jobs, OperationStore &operations, Clock &clock,
                                                                          WallClock &wallClock, const ProcessService &processes,
                                                                          Runtime::SaveRootPlatform platform,
                                                                          std::shared_ptr<Security::SecureRandomSource> entropy);
    /** @brief Selects real native entropy and the compiled OS convention at the explicit application composition boundary.
     * @param jobs Actual application scheduler. @param operations Actual process diagnostics authority.
     * @param clock Host monotonic clock. @param wallClock Host UTC clock.
     * @return Native sandbox factory or original typed root/configuration failure; no production namespace is selected.
     */
    [[nodiscard]] Result<EditorPlaySaveFactory> MakeNativeEditorPlaySaveFactory(JobSystem &jobs, OperationStore &operations, Clock &clock,
                                                                                WallClock &wallClock);
}  // namespace Horo::Application::Internal
