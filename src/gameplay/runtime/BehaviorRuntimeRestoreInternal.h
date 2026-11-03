#pragma once
#include "BehaviorRuntimeState.h"
#include "Horo/Gameplay/BehaviorRuntimeRestore.h"

#include <thread>

namespace Horo::Gameplay {
    [[nodiscard]] inline bool ValidLifecycleFlags(BehaviorLifecycleFlags flags) noexcept {
        if (!flags.created && (flags.started || flags.enabledCallbackActive))
            return false;
        return !flags.enabledCallbackActive || flags.enabled;
    }

    struct BehaviorReloadLifecycle final {
        Runtime::EntityRef entity;
        BehaviorInstanceId instance;
        BehaviorTypeId type;
        BehaviorLifecycleFlags flags;
    };

    struct BehaviorAttachmentProjection final {
        Runtime::EntityRef runtime;
        BehaviorRestoreAttachment identity;
        BehaviorTypeId type;
    };

    struct BehaviorRestoreBindingProgress {
        bool materialized{}, fixed{};
    };

    struct BehaviorRuntimeRestoreCandidate::State {
        std::unique_ptr<BehaviorRuntime> runtime;
        Runtime::RuntimeScene *scene{};
        const BehaviorRegistry *registry{};
        std::shared_ptr<void> generation;
        std::vector<BehaviorLifecycleFlags> lifecycle;
        std::vector<BehaviorReloadLifecycle> reloadFlags;
        std::vector<std::shared_ptr<BehaviorRestoreBindingProgress>> bindings;
        std::thread::id owner{std::this_thread::get_id()};
        std::uint64_t bindingEpoch{1};
        bool prehookReady{}, restoring{}, staged{}, lifecyclePublished{}, published{}, closed{}, ordinaryActivated{};
    };

    struct BehaviorRuntimeSceneOwner::State {
        Runtime::RuntimeSceneService *scenes{};
        const BehaviorRegistry *registry{};
        BehaviorRuntimeLimits limits;
        std::shared_ptr<BehaviorRuntimeRestoreCandidate> pending;
        std::weak_ptr<BehaviorRuntimeRestoreCandidate> active;
        std::thread::id owner{std::this_thread::get_id()};
    };

    class BehaviorRuntimeRestoreAccess {
    public:
        [[nodiscard]] static Result<std::unique_ptr<BehaviorRuntime>> Construct(Runtime::RuntimeScene &, const BehaviorRegistry &,
                                                                                BehaviorRuntimeLimits);
        [[nodiscard]] static Result<void> ConfigureState(BehaviorRuntime::Impl &, BehaviorRuntimeServiceBindings,
                                                         std::shared_ptr<const GameplayPrefabContext>);
        [[nodiscard]] static Result<void> Configure(BehaviorRuntime &, BehaviorRuntimeServiceBindings,
                                                    std::shared_ptr<const GameplayPrefabContext>, Runtime::RuntimeSceneService *);
        [[nodiscard]] static Result<void> Prehook(BehaviorRuntime &);
        [[nodiscard]] static Result<void> Activate(BehaviorRuntime &);
        [[nodiscard]] static std::vector<BehaviorReloadLifecycle> LiveFlags(const BehaviorRuntime &);
        [[nodiscard]] static Result<std::vector<BehaviorLifecycleFlags>> MatchReload(const BehaviorRuntime &,
                                                                                     std::span<const BehaviorReloadLifecycle>);
        [[nodiscard]] static Result<void> RestoreReloadRunner(BehaviorRuntime &, const BehaviorRuntimeReloadSnapshot &,
                                                              std::span<const BehaviorLifecycleFlags>, BehaviorRuntimeServiceBindings,
                                                              std::shared_ptr<const GameplayPrefabContext>, Runtime::RuntimeSceneService *);
        [[nodiscard]] static Result<std::unique_ptr<BehaviorRuntime>> PrepareReloadRunner(
            Runtime::RuntimeScene &, const BehaviorRegistry &, BehaviorRuntimeLimits, const BehaviorRuntimeReloadSnapshot &,
            std::span<const BehaviorReloadLifecycle>, BehaviorRuntimeServiceBindings, std::shared_ptr<const GameplayPrefabContext>,
            Runtime::RuntimeSceneService *);
        [[nodiscard]] static Result<void> ReloadEnabled(BehaviorRuntime &, std::span<const BehaviorLifecycleFlags>);
        [[nodiscard]] static Result<void> PrepareHooks(BehaviorRuntimeRestoreCandidate &);
        [[nodiscard]] static Result<void> ValidateKnownNoServiceRequirements(const BehaviorRuntime &);
        [[nodiscard]] static bool BindingAdmitted(const BehaviorRuntime &) noexcept;
        [[nodiscard]] static Result<void> ValidateGeneration(const BehaviorRuntime &);
        [[nodiscard]] static bool Matches(const BehaviorRuntimeRestoreCandidate &, Runtime::SceneRuntimeId) noexcept;
        [[nodiscard]] static std::shared_ptr<void> Generation(const BehaviorRuntime &);
        [[nodiscard]] static Result<std::vector<BehaviorAttachmentProjection>> AttachmentIdentities(
            const BehaviorRuntime &, const BehaviorTypeId &, const Runtime::PersistentEntityIdentityMap &);
        [[nodiscard]] static Result<std::vector<BehaviorRestoreInstance>> Project(BehaviorRuntime &);
        [[nodiscard]] static Result<BehaviorLifecycleSnapshot> Capture(const BehaviorRuntime &,
                                                                       const Runtime::PersistentEntityIdentityMap &);
        [[nodiscard]] static Result<std::vector<BehaviorLifecycleFlags>> Match(const BehaviorRuntime &, const BehaviorLifecycleSnapshot &,
                                                                               const Runtime::PersistentEntityIdentityMap &);
        static void Publish(BehaviorRuntime &, std::span<const BehaviorLifecycleFlags>) noexcept;
        [[nodiscard]] static Result<std::shared_ptr<BehaviorRuntimeRestoreCandidate>> Prepare(
            std::shared_ptr<BehaviorRuntimeSceneOwner::State>, Runtime::RuntimeScene &, bool restoring,
            BehaviorRuntimeServiceBindings services = {}, std::shared_ptr<const GameplayPrefabContext> prefabs = {});
        [[nodiscard]] static Result<std::unique_ptr<Runtime::SceneActivationCandidate>> SceneCandidate(
            std::shared_ptr<BehaviorRuntimeSceneOwner::State>, std::shared_ptr<BehaviorRuntimeRestoreCandidate>);
        [[nodiscard]] static std::unique_ptr<Runtime::SceneActivationParticipant> Participant(
            std::shared_ptr<BehaviorRuntimeSceneOwner::State>);
        [[nodiscard]] static Result<void> StageFlags(BehaviorRuntimeRestoreCandidate &, std::vector<BehaviorLifecycleFlags>);
        static void PublishLifecycle(BehaviorRuntimeRestoreCandidate &) noexcept;
        static void PublishCandidate(BehaviorRuntimeRestoreCandidate &candidate, BehaviorRuntimeSceneOwner::State &owner) noexcept;
        static void ShutdownCandidate(BehaviorRuntimeRestoreCandidate &candidate) noexcept;
    };

    [[nodiscard]] inline Error BehaviorRestoreError(const char *message) {
        return MakeError(Runtime::SaveErrors::RestoreAdapterContractInvalid, message);
    }
}  // namespace Horo::Gameplay
