#pragma once
/** @file EditorPlaySaveHostInternal.h
 * @brief Private owner/worker partition; only immutable storage inputs cross the actual scheduler boundary.
 */
#include "EditorPlaySaveHost.h"
#include "EditorPlaySaveHostRestore.h"
#include "EditorPlaySavePhysicsServices.h"
#include "EditorPlaySavePrefabServices.h"

#include <mutex>
#include <thread>

namespace Horo::Application::Internal {
    /** @brief Closed set of worker-only storage actions, each admitted at most once per owner. */
    enum class PlaySaveStorageAction {
        Open,
        Refresh,
        Delete,
        Cleanup
    };

    /** @brief Actual detached storage result published only after all action work completed. */
    struct PlaySaveStorageUpdate final {
        std::shared_ptr<PlaySaveStorage> storage;
        Runtime::SaveSlotIndex catalog;
    };

    /** @brief Serializes publication of one owned result; JobHandle terminal state remains the settlement authority. */
    struct PlaySaveStorageWork final {
        std::mutex publication;
        std::optional<Result<PlaySaveStorageUpdate>> result;
    };

    /** @brief Actual live-generation authority; only owner safe points call Current while the containing Scene is retained. */
    class PlaySaveGeneration final : public Runtime::ISceneRestoreGenerationAuthority {
    public:
        PlaySaveGeneration(Runtime::RuntimeSceneService &scenes, std::uint64_t session, std::uint64_t registry) noexcept;
        [[nodiscard]] Runtime::StagedRestoreActivationEvidence Current() const noexcept override;
        [[nodiscard]] Runtime::SaveRuntimeGeneration Generation() const noexcept;

    private:
        Runtime::RuntimeSceneService &scenes_;
        std::uint64_t session_{};
        std::uint64_t registry_{};
    };

    /** @brief Runs one real lifecycle action on owned detached inputs without a controller, Scene or authoring borrow. */
    [[nodiscard]] Result<PlaySaveStorageUpdate> RunPlaySaveStorageAction(PlaySaveStorageAction action, PlaySaveStorageConfig config,
                                                                         std::shared_ptr<const PlaySaveStorageSchema> schema,
                                                                         std::shared_ptr<PlaySaveStorage> storage,
                                                                         std::optional<Runtime::SaveManagerCommand> selection,
                                                                         const CancellationToken &cancellation);
    /** @brief Returns the same complete current inventory to every actual canonical Gameplay/lifecycle capture owner. */
    [[nodiscard]] Result<Runtime::PersistentEntityIdentityMap> CurrentPlaySaveIdentities(Runtime::RuntimeSceneService &scenes,
                                                                                         const PlaySaveStorageSchema &schema,
                                                                                         const PlaySavePrefabServices *prefabs);

    /** @brief Polls genuine durable job state without joining or relying on bounded global history. */
    [[nodiscard]] bool PlaySaveJobSettled(const JobHandle &job);

    struct EditorPlaySaveHost::State final {
        State(Runtime::RuntimeSceneService &scenes, EditorPlaySaveEnvironment environment,
              std::shared_ptr<const PlaySaveStorageSchema> schema,
              std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings,
              std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner, std::shared_ptr<PlaySavePrefabServices> prefabs,
              std::shared_ptr<PlaySavePhysicsServices> physics);
        /** @brief Composes the actual initial owners before asynchronous storage opening. */
        [[nodiscard]] Result<void> Initialize();
        /** @brief Registers the disjoint Scene and prefab canonical capture owners once. */
        [[nodiscard]] Result<void> RegisterCoreCapture();
        /** @brief Registers only the selected actual canonical Physics source. */
        [[nodiscard]] Result<void> RegisterPhysicsCapture();
        [[nodiscard]] Result<void> ReleasePhysicsCapture();
        /** @brief Builds same-request preparation without retaining this state in detached workers. */
        [[nodiscard]] std::shared_ptr<Runtime::IRuntimeSaveCapturePreparation> CapturePreparation();
        /** @brief Reads the same exact complete current identity map for all canonical Gameplay owners. */
        [[nodiscard]] Result<Runtime::PersistentEntityIdentityMap> CurrentIdentities() const;
        /** @brief Binds actual installed behavior sources through the real current
         * identity map, never metadata-only coverage. */
        [[nodiscard]] Result<void> BindActiveGameplay();
        /** @brief Rebinds actual current sources before registering the one canonical
         * capture inventory. */
        [[nodiscard]] Result<void> RecomposeBindings();
        /** @brief Registers the Gameplay-owned lifecycle schema and actual owner
         * capture adapter. */
        [[nodiscard]] Result<void> RegisterBehaviorLifecycle();
        /** @brief Registers the complete actual core/native participant inventory before any session request. */
        [[nodiscard]] Result<void> Compose();
        /** @brief Admits all real capture owners under the same barrier and bounded snapshot policy. */
        [[nodiscard]] Result<void> ComposeCaptureOwners();
        /** @brief Recreates only the session fence after an actual Scene aggregate replacement has settled. */
        [[nodiscard]] Result<void> ComposeRuntime();
        /** @brief Builds current source evidence from the actual committed Play Scene. */
        [[nodiscard]] Runtime::RuntimeSaveCaptureProvenance Provenance() const;
        /** @brief Admits one already-issued operation into the existing runtime session. */
        /** @brief Contains pre-admission allocation failure while preserving the existing process operation record. */
        [[nodiscard]] Result<void> TryAdmitRuntime(const Editor::PlaySaveCommand &command, OperationId id);
        [[nodiscard]] Result<void> AdmitRuntime(const Editor::PlaySaveCommand &command, OperationId id);
        /** @brief Starts detached slot work with no live owner capture. */
        [[nodiscard]] Result<void> StartStorage(PlaySaveStorageAction action, std::optional<Runtime::SaveManagerCommand> selection = {});
        /** @brief Consumes storage publication only after the exact accepted job really settled. */
        [[nodiscard]] Result<void> PumpStorage();
        /** @brief Consumes only a genuinely settled worker's detached publication under its short publication lock. */
        [[nodiscard]] Result<void> ConsumeStorage(PlaySaveStorageWork &work, const std::optional<JobSnapshot> &job);
        /** @brief Publishes changed immutable presentation from actual detached catalog and operation values. */
        [[nodiscard]] Result<void> Publish();
        /** @brief Computes presentation state from actual owner admission and detached work. */
        [[nodiscard]] Editor::PlaySaveAvailability Availability() const noexcept;
        /** @brief Creates the bounded shared Manager projection from the real worker-returned catalog. */
        [[nodiscard]] Result<Runtime::SaveManagerProjection> ProjectManager() const;
        /** @brief Admits a genuine runtime request using a fresh OperationStore identity. */
        /** @brief Routes validated owner intent without storage I/O or an independent publication gate. */
        /** @brief Revalidates actual slot consent and action kind before detached execution. */
        /** @brief Checks actual owner and single-operation capacity before intent dispatch. */
        [[nodiscard]] Result<void> ValidateRequest(const Editor::PlaySaveCommand &command) const;
        /** @brief Requests canonical operation/job cancellation without waiting. */
        [[nodiscard]] Result<void> CancelRequest();
        [[nodiscard]] Result<void> ValidateSelection(const Editor::PlaySaveCommand &command) const;
        [[nodiscard]] Result<void> AdmitCommand(const Editor::PlaySaveCommand &command);
        [[nodiscard]] Result<void> StartRuntime(const Editor::PlaySaveCommand &command);
        /** @brief Processes actual terminal settlement before recomposing a replaced Scene incarnation. */
        [[nodiscard]] Result<void> PumpRuntime();
        /** @brief Publishes actual canonical progress and the original terminal failure into Manager/process diagnostics. */
        void ObserveRuntime(const Runtime::SaveOperationSnapshot &current);
        /** @brief Reopens only runtime admission after settled work; retirement never recreates module bindings. */
        [[nodiscard]] Result<void> ResumeAfterRequest();
        /** @brief Releases actual runtime/source bindings only after their original request and workers settled. */
        [[nodiscard]] Result<void> AdvanceModuleSuspension();
        /** @brief Unregisters exact current bindings on the same registry, preserving monotonic generations. */
        [[nodiscard]] Result<void> ReleaseRuntimeBindings();
        /** @brief Checks real installed inventory against this activation's frozen value schema. */
        [[nodiscard]] Result<void> ValidateLiveBindings(std::span<const Editor::PlaySaveGameplayBinding> bindings) const;
        /** @brief Starts sandbox-only cleanup after actual session/delegate/worker settlement. */
        void AdvanceRetirement();

        Runtime::RuntimeSceneService &scenes;
        EditorPlaySaveEnvironment environment;
        std::shared_ptr<const PlaySaveStorageSchema> schema;
        std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings;
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner;
        std::shared_ptr<Gameplay::BehaviorLifecyclePersistenceAdapter> lifecycle;
        std::shared_ptr<PlaySavePrefabServices> prefabs;
        std::shared_ptr<PlaySavePhysicsServices> physics;
        std::shared_ptr<PlaySaveSceneHandoff> handoff;
        Runtime::CanonicalStateParticipantRegistry registry;
        Runtime::SaveParticipantRegistrySnapshot participants;
        std::shared_ptr<PlaySaveGeneration> authority;
        std::unique_ptr<Runtime::SaveCaptureBarrier> barrier;
        std::vector<std::size_t> barrierOwners;
        std::vector<Runtime::SaveBarrierMutation> mutations;
        std::unique_ptr<Runtime::RuntimeSaveSession> runtime;
        std::optional<Runtime::RuntimeSaveRequest> request;
        std::optional<Runtime::SaveOperationSnapshot> operation;
        std::shared_ptr<PlaySaveStorage> storage;
        std::optional<Runtime::SaveSlotIndex> catalog;
        std::shared_ptr<PlaySaveStorageWork> storageWork;
        JobHandle storageJob;
        PlaySaveStorageAction storageAction{PlaySaveStorageAction::Open};
        CancellationSource cancellation;
        CancellationSource requestCancellation;
        std::shared_ptr<const Editor::PlaySaveSnapshot> snapshot;
        std::optional<Error> error;
        std::thread::id owner{std::this_thread::get_id()};
        Duration started;
        Runtime::CapturedStateId captured;
        std::uint64_t epoch{1};
        std::uint64_t revision{};
        std::uint64_t operationRevision{};
        bool suspending{};
        bool suspended{};
        bool retiring{};
        bool cleanupAttempted{};
        bool dirty{true};
        bool loading{};
    };
}  // namespace Horo::Application::Internal
