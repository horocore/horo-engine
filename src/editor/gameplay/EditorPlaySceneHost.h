#ifndef HORO_EDITOR_INTERNAL_PLAY_SCENE_HOST_H
#define HORO_EDITOR_INTERNAL_PLAY_SCENE_HOST_H

/** @file EditorPlaySceneHost.h
 * @brief Non-installed app-injected ownership boundary for isolated Editor Play Scene services.
 */
#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

namespace Horo::Gameplay {
    class ComponentRegistry;
}

namespace Horo::Editor {
    /** @brief Host-reserved namespace divider; edit preview admission stays strictly below this value. */
    inline constexpr std::uint64_t EditorPlaySceneNamespace = 0x8000000000000000ULL;
    /** @brief First Play identity; the divider itself is never issued. */
    inline constexpr Runtime::SceneRuntimeId EditorFirstPlayScene{EditorPlaySceneNamespace + 1};

    /** @brief Actual application-owned peer lifecycle, independent of editor draft/publication policy. */
    enum class EditorScenePhysicsState : std::uint8_t {
        NoPhysics,
        Inactive,
        Active
    };

    /** @brief Owner-lane synchronous receipt borrowing immutable capture identity from its actual host.
     * @details The projectId view expires at host shutdown/destruction. Never retain it in a job, snapshot or callback.
     * Active is reported only after the real aggregate participant publishes; capture fields remain available
     * while inactive solely to compare preview/Play authorities. No native handles or mutable authority escape.
     */
    struct EditorSceneCollisionReceipt final {
        EditorScenePhysicsState state{EditorScenePhysicsState::NoPhysics};
        std::string_view projectId;
        std::uint64_t documentRevision{};
        std::uint64_t generation{};
        std::uint64_t fingerprint{};
    };

    /** @brief Application-injected observer; callable only synchronously while its preview host remains alive. */
    struct EditorPreviewCollisionReceiptSource final {
        std::function<std::optional<EditorSceneCollisionReceipt>()> read;
    };

    /** @brief Owns one configured Scene service and every borrowed native/asset authority required by its candidates.
     * @details Scene remains the only candidate owner. Application implementations select their explicit backend and
     * capture project inputs before construction; Editor does not discover or instantiate native backends.
     */
    class EditorPlaySceneHost {
    public:
        virtual ~EditorPlaySceneHost() = default;
        /** @brief Borrows the actual single Scene service for preparation, behavior transactions and read-only presentation.
         * @return Service owned by this host until shutdown/destruction.
         */
        [[nodiscard]] virtual Runtime::RuntimeSceneService &Scenes() noexcept = 0;

        /** @brief Resolves explicitly granted gameplay services after aggregate readiness.
         * @return Captured typed services or a failure before behavior creation; default withholds all optional capabilities.
         */
        [[nodiscard]] virtual Result<Gameplay::BehaviorRuntimeServiceBindings> GameplayServices() {
            return Result<Gameplay::BehaviorRuntimeServiceBindings>::Success({});
        }

        /** @brief Acquires the explicitly granted prefab capability after aggregate readiness.
         * @param registry Frozen project-owned behavior snapshot, outliving its runtime.
         * @param components Frozen Scene-epoch project component metadata, outliving this host.
         * @return Host-owned context or an empty context for denied/unselected composition; no ambient provider discovery. */
        [[nodiscard]] virtual Result<std::shared_ptr<Gameplay::GameplayPrefabContext>> PrefabServices(const Gameplay::BehaviorRegistry &,
                                                                                                      const Gameplay::ComponentRegistry &) {
            return Result<std::shared_ptr<Gameplay::GameplayPrefabContext>>::Success(nullptr);
        }

        /** @brief Drives owned pending prefab preparation and the sole Scene safe-point transaction.
         * @param frame Actual owner-lane frame/cancellation boundary. @return Original preparation/commit failure. */
        [[nodiscard]] virtual Result<void> CommitSceneChanges(const Runtime::FrameContext &frame) {
            return Scenes().OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame);
        }

        /** @brief Advances capability tick admission before behavior fixed callbacks without simulating or blocking.
         * @param delta Actual host fixed quantum. @return Admission failure or success for a host without prefabs. */
        [[nodiscard]] virtual Result<void> BeginFixedUpdate(Gameplay::FixedDeltaTime) {
            return Result<void>::Success();
        }

        /** @brief Revokes module-owned pending prefab work before behavior reload; resident Scene groups keep their leases. */
        virtual void QuiesceGameplay() noexcept {}

        /** @brief Borrows actual capture/publication facts without allocating or inferring a backend.
         * @return Owner-lane receipt or unknown for hosts which supply no verified Physics evidence.
         */
        [[nodiscard]] virtual std::optional<EditorSceneCollisionReceipt> CollisionReceipt() const noexcept {
            return std::nullopt;
        }

        /** @brief Advances this already selected native composition after the ordinary gameplay fixed update.
         * @param delta Existing Editor host fixed quantum in seconds. @return Actual native failure or success.
         * @details Called only while playing or for one admitted paused step; never used for load progress.
         */
        [[nodiscard]] virtual Result<void> FixedUpdate(Gameplay::FixedDeltaTime delta) = 0;
        /** @brief Revokes borrowed native admission before cancelling/joining Scene work and releasing its candidates. */
        virtual void Shutdown() noexcept = 0;
    };

    /** @brief Load-time factory whose owned result pins all native/assets through Scene retirement.
     * @details The requested identity belongs to the host's reserved Play namespace. The result is configured but not
     * started; Play owns asynchronous Scene startup/preparation and publishes no gameplay before readiness.
     */
    using EditorPlaySceneFactory =
        std::function<Result<std::unique_ptr<EditorPlaySceneHost>>(Runtime::SceneRuntimeId, const Runtime::RuntimeSceneDefinition &)>;
    /** @brief Application policy bound to an exact project by the workspace before asynchronous Play admission. */
    using EditorProjectPlaySceneFactory =
        std::function<Result<std::unique_ptr<EditorPlaySceneHost>>(const std::filesystem::path &, Runtime::SceneRuntimeId,
                                                                   const Runtime::RuntimeSceneDefinition &)>;
}  // namespace Horo::Editor

#endif  // HORO_EDITOR_INTERNAL_PLAY_SCENE_HOST_H
