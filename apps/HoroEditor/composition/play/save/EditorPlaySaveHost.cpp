#include "EditorPlaySaveHostInternal.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <limits>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Declares read scope without replacing the actual request's native preparation/capture. */
        Result<void> ValidatePhysicsSelection(const PlaySaveStorageSchema &schema,
                                              std::span<const Editor::PlaySaveGameplayBinding> gameplay,
                                              const std::shared_ptr<PlaySavePhysicsServices> &physics, Runtime::RuntimeSceneView scene) {
            if (!schema.physics || !physics)
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::ParticipantAdapterMissing));
            if (*schema.physics != Physics::Detail::PhysicsStateSaveDescriptor())
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::CaptureRegistryStale));
            return ValidatePlaySaveCurrentScope(scene, gameplay, PlaySavePhysicsReadPolicy::CanonicalRecordRequired);
        }

        /** @brief Admits only a real active Play epoch and its complete actual typed state inventory. */
        Result<void> ValidateOwnerInputs(const Runtime::RuntimeSceneService &scenes, const EditorPlaySaveEnvironment &environment,
                                         const std::shared_ptr<const PlaySaveStorageSchema> &schema,
                                         const std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> &liveBindings,
                                         const std::shared_ptr<PlaySavePhysicsServices> &physics) {
            const auto active = scenes.ActiveScene();
            if (!active || !schema || !environment.storage.entropy || active->DefinitionId() != schema->baseline.Id() ||
                active->DefinitionRevision() != schema->baseline.Revision())
                return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreContextInvalid));
            const auto gameplay = liveBindings ? std::span<const Editor::PlaySaveGameplayBinding>{*liveBindings}
                                               : std::span<const Editor::PlaySaveGameplayBinding>{};
            if (schema->physics || physics)
                return ValidatePhysicsSelection(*schema, gameplay, physics, *active);
            const auto captured = CapturePlaySaveScene(*active, schema->baseline, schema->identities, gameplay);
            return captured.HasError() ? Result<void>::Failure(captured.ErrorValue()) : Result<void>::Success();
        }
    }  // namespace

    /** @brief Captures only explicitly injected owners; no filesystem work or hidden registration occurs in construction. */
    EditorPlaySaveHost::State::State(Runtime::RuntimeSceneService &scenes, EditorPlaySaveEnvironment environment,
                                     std::shared_ptr<const PlaySaveStorageSchema> schema,
                                     std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings,
                                     std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner,
                                     std::shared_ptr<PlaySavePrefabServices> prefabs, std::shared_ptr<PlaySavePhysicsServices> physics)
        : scenes(scenes), environment(std::move(environment)), schema(std::move(schema)), liveBindings(std::move(liveBindings)),
          behaviorOwner(std::move(behaviorOwner)), prefabs(std::move(prefabs)), physics(std::move(physics)),
          started(this->environment.clock.MonotonicNow()) {}

    /** @copydoc EditorPlaySaveHost::Create */
    Result<std::unique_ptr<EditorPlaySaveHost>> EditorPlaySaveHost::Create(
        Runtime::RuntimeSceneService &scenes, EditorPlaySaveEnvironment environment, std::shared_ptr<const PlaySaveStorageSchema> schema,
        std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> liveBindings,
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner, std::shared_ptr<PlaySavePrefabServices> prefabs,
        std::shared_ptr<PlaySavePhysicsServices> physics) {
        using Outcome = Result<std::unique_ptr<EditorPlaySaveHost>>;
        try {
            if (const auto valid = ValidateOwnerInputs(scenes, environment, schema, liveBindings, physics); valid.HasError())
                return Outcome::Failure(valid.ErrorValue());
            auto state = std::make_unique<State>(scenes, std::move(environment), std::move(schema), std::move(liveBindings),
                                                 std::move(behaviorOwner), std::move(prefabs), std::move(physics));
            if (auto initialized = state->Initialize(); initialized.HasError())
                return Outcome::Failure(initialized.ErrorValue());
            return Outcome::Success(std::unique_ptr<EditorPlaySaveHost>(new EditorPlaySaveHost(std::move(state))));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(Runtime::SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc EditorPlaySaveHost::State::Initialize */
    Result<void> EditorPlaySaveHost::State::Initialize() {
        if (const auto checked = ValidateLiveBindings(liveBindings ? std::span<const Editor::PlaySaveGameplayBinding>{*liveBindings}
                                                                   : std::span<const Editor::PlaySaveGameplayBinding>{});
            checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        if (prefabs) {
            if (auto installed = prefabs->InstallPersistence(environment.storage.world, schema->identities, environment.storage.entropy);
                installed.HasError())
                return Result<void>::Failure(installed.ErrorValue());
        }
        if (const auto bound = BindActiveGameplay(); bound.HasError())
            return Result<void>::Failure(bound.ErrorValue());
        if (const auto composed = Compose(); composed.HasError())
            return Result<void>::Failure(composed.ErrorValue());
        if (const auto opened = StartStorage(PlaySaveStorageAction::Open); opened.HasError())
            return Result<void>::Failure(opened.ErrorValue());
        if (const auto published = Publish(); published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        return Result<void>::Success();
    }

    /** @brief Takes the one fully composed owner without replacing any operation, Scene or registry authority. */
    EditorPlaySaveHost::EditorPlaySaveHost(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @brief Lifetime composition must retain this Scene owner through real detached settlement. */
    EditorPlaySaveHost::~EditorPlaySaveHost() {
        BeginRetirement();
        if (!IsSettled())
            std::terminate();
    }

    /** @copydoc Editor::EditorPlaySaveSession::Snapshot */
    std::shared_ptr<const Editor::PlaySaveSnapshot> EditorPlaySaveHost::Snapshot() const noexcept {
        return state_->snapshot;
    }

    /** @copydoc Editor::EditorPlaySaveSession::Request */
    Result<void> EditorPlaySaveHost::Request(const Editor::PlaySaveCommand &command) {
        using enum Editor::PlaySaveCommandKind;
        if (const auto valid = state_->ValidateRequest(command); valid.HasError())
            return valid;
        if (command.kind == Cancel)
            return state_->CancelRequest();
        const auto admitted = state_->AdmitCommand(command);
        if (admitted.HasError())
            return admitted;
        return state_->Publish();
    }

    /** @brief Storage-only intents do not require executable bindings while a module is suspended. */
    bool RuntimeIntentAdmitted(Editor::PlaySaveCommandKind kind, bool runtime) noexcept {
        return runtime || kind == Editor::PlaySaveCommandKind::Refresh || kind == Editor::PlaySaveCommandKind::Delete;
    }

    /** @brief Checks owner affinity and finite single-operation admission without touching storage. */
    Result<void> EditorPlaySaveHost::State::ValidateRequest(const Editor::PlaySaveCommand &command) const {
        if (std::this_thread::get_id() != owner)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (command.kind == Editor::PlaySaveCommandKind::Cancel)
            return Result<void>::Success();
        if (retiring || (!storage && command.kind != Editor::PlaySaveCommandKind::Refresh))
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        if (!RuntimeIntentAdmitted(command.kind, static_cast<bool>(runtime)))
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        if (request || storageWork || !mutations.empty())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationInProgress));
        return Result<void>::Success();
    }

    /** @brief Requests original runtime and durable worker cancellation without revoking the activation's future admission. */
    Result<void> EditorPlaySaveHost::State::CancelRequest() {
        if (request)
            (void)request->RequestCancellation();
        if (storageWork)
            (void)storageJob.RequestCancel();
        return Result<void>::Success();
    }

    /** @brief Uses the last real immutable projection as the first stale-consent gate; storage rechecks exact generation CAS. */
    Result<void> EditorPlaySaveHost::State::AdmitCommand(const Editor::PlaySaveCommand &command) {
        using enum Editor::PlaySaveCommandKind;
        if (const auto valid = ValidateSelection(command); valid.HasError())
            return valid;
        if (command.kind == Refresh) {
            error.reset();
            return StartStorage(storage ? PlaySaveStorageAction::Refresh : PlaySaveStorageAction::Open);
        }
        if (suspending || suspended)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        if (command.kind == Delete)
            return StartStorage(PlaySaveStorageAction::Delete, command.selection);
        if (command.kind != Load && command.kind != SaveNew)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
        error.reset();
        return StartRuntime(command);
    }

    /** @brief Validates action-specific selection and actual projection consent before a worker obtains any storage lease. */
    Result<void> EditorPlaySaveHost::State::ValidateSelection(const Editor::PlaySaveCommand &command) const {
        using enum Editor::PlaySaveCommandKind;
        if (command.kind != Load && command.kind != Delete)
            return Result<void>::Success();
        if (!command.selection || !snapshot || !snapshot->manager)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
        const auto expected = command.kind == Load ? Runtime::SaveManagerCommandKind::Load : Runtime::SaveManagerCommandKind::Delete;
        if (command.selection->kind != expected)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CompositionUnsupported));
        return snapshot->manager->ValidateCommand(*command.selection);
    }

    /** @copydoc Editor::EditorPlaySaveSession::CommitAtSafePoint */
    Result<void> EditorPlaySaveHost::CommitAtSafePoint(const Runtime::FrameContext &) {
        if (std::this_thread::get_id() != state_->owner)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::ThreadAffinityViolation));
        if (!state_->retiring && !state_->mutations.empty())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::OperationInProgress));
        if (const auto pumped = state_->PumpStorage(); pumped.HasError()) {
            state_->error = pumped.ErrorValue();
            state_->dirty = true;
        }
        if (const auto pumped = state_->PumpRuntime(); pumped.HasError()) {
            state_->error = pumped.ErrorValue();
            state_->dirty = true;
        }
        if (const auto suspended = state_->AdvanceModuleSuspension(); suspended.HasError()) {
            state_->error = suspended.ErrorValue();
            state_->dirty = true;
        }
        state_->AdvanceRetirement();
        return state_->Publish();
    }

    /** @copydoc Editor::EditorPlaySaveSession::BeginRetirement */
    void EditorPlaySaveHost::BeginRetirement() noexcept {
        if (state_->retiring)
            return;
        state_->retiring = true;
        state_->dirty = true;
        state_->cancellation.RequestCancellation();
        state_->requestCancellation.RequestCancellation();
        try {
            if (state_->runtime)
                (void)state_->runtime->Retire();
            if (state_->storageWork)
                (void)state_->storageJob.RequestCancel();
        } catch (...) {
            // Exact workers/delegate remain owned; no exception permits their
            // destruction.
        }
    }

    /** @copydoc Editor::EditorPlaySaveSession::IsSettled */
    bool EditorPlaySaveHost::IsSettled() const noexcept {
        return state_->retiring && state_->cleanupAttempted && !state_->storageWork && (!state_->runtime || state_->runtime->IsSettled());
    }

    /** @copydoc EditorPlaySaveHost::IsRetiring */
    bool EditorPlaySaveHost::IsRetiring() const noexcept {
        return state_->retiring;
    }

    /** @copydoc EditorPlaySaveHost::State::AdvanceRetirement */
    void EditorPlaySaveHost::State::AdvanceRetirement() {
        if (!retiring || cleanupAttempted || storageWork || (runtime && !runtime->IsSettled()))
            return;
        cleanupAttempted = true;
        if (storage) {
            const auto submitted = StartStorage(PlaySaveStorageAction::Cleanup);
            if (submitted.HasValue())
                return;
            error = submitted.ErrorValue();
            LOG_WARN("editor.play.save", "Sandbox cleanup deferred after request admission closed: %s", error->message.c_str());
            storage->CloseAdmission();
        }
        dirty = true;
    }

    /** @copydoc EditorPlaySaveHost::BeginMutation */
    Result<void> EditorPlaySaveHost::BeginMutation() {
        if (std::this_thread::get_id() != state_->owner || !HasRuntimeBinding() || !state_->mutations.empty())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        for (const auto owner : state_->barrierOwners) {
            const auto begun = state_->barrier->BeginMutation(owner);
            if (begun.HasError())
                return Result<void>::Failure(begun.ErrorValue());
            state_->mutations.push_back(begun.Value());
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::EndMutation */
    Result<void> EditorPlaySaveHost::EndMutation() {
        if (std::this_thread::get_id() != state_->owner || state_->mutations.empty() ||
            state_->epoch == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::LifecycleUnavailable));
        ++state_->epoch;
        for (const auto mutation : state_->mutations)
            if (const auto ended = state_->barrier->EndMutation(mutation, {state_->epoch}); ended.HasError())
                return ended;
        state_->mutations.clear();
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::Provenance */
    Runtime::RuntimeSaveCaptureProvenance EditorPlaySaveHost::State::Provenance() const {
        const auto active = scenes.ActiveScene();
        return {captured,
                {epoch},
                active ? active->RuntimeId().value : 0,
                active ? active->StructuralRevision() : 0,
                participants.Generation()};
    }
}  // namespace Horo::Application::Internal
