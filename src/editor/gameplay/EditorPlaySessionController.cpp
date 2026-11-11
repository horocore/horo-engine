#include "editor/gameplay/EditorPlaySessionController.h"

#include "EditorPlayXrHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "ProjectGameplayRegistry.h"

#include <algorithm>
#include <format>
#include <new>
#include <ranges>
#include <string_view>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Explicit assetless composition for private callers that select no native participants or assets. */
        class AssetlessPlaySceneHost final : public EditorPlaySceneHost {
        public:
            explicit AssetlessPlaySceneHost(Runtime::SceneRuntimeId scene) : scenes_(scene) {}

            Runtime::RuntimeSceneService &Scenes() noexcept override {
                return scenes_;
            }

            Result<void> FixedUpdate(Gameplay::FixedDeltaTime) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                scenes_.Shutdown();
            }

        private:
            Runtime::RuntimeSceneService scenes_;
        };

        /** @brief Resolves the actual Play runner only during one synchronous Scene structural transaction. */
        class PlayGameplayParticipant final : public Runtime::SceneStructuralParticipant {
        public:
            explicit PlayGameplayParticipant(std::shared_ptr<EditorPlayRuntimeBinding> binding) : binding_(std::move(binding)) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Runtime::RuntimeEntityView> created,
                                                                               std::span<const Runtime::EntityRef> destroyed) override {
                const auto owner = binding_->owner.lock();
                auto *runtime = binding_->revoked || !owner ? nullptr : owner->Active();
                if (!runtime)
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent, "Play has no active Gameplay structural owner."));
                return runtime->MakeStructuralParticipant()->Prepare(scene, created, destroyed);
            }

        private:
            // The moved Scene pins this owner-lane lookup, never the controller or
            // workspace address.
            std::shared_ptr<EditorPlayRuntimeBinding> binding_;
        };

        /** @brief Contains allocation failure while the original runtime and all live gameplay scopes remain intact. */
        Result<Gameplay::BehaviorRuntimeReloadSnapshot> CaptureReloadSafely(Gameplay::BehaviorRuntime &runtime) {
            try {
                return runtime.CaptureReloadSnapshot();
            } catch (const std::bad_alloc &) {
                return Result<Gameplay::BehaviorRuntimeReloadSnapshot>::Failure(MakeError(Runtime::SaveErrors::RestoreAllocationFailed));
            }
        }

        [[nodiscard]] Error InvalidTransition(const char *message) {
            return MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent, message);
        }

        [[nodiscard]] std::string_view ComponentStatusText(const Gameplay::ComponentInspectionStatus status) noexcept {
            using enum Gameplay::ComponentInspectionStatus;
            switch (status) {
                case MissingDescriptor:
                    return "descriptor missing";
                case MigrationRequired:
                    return "migration required";
                case UnsupportedOlderSchema:
                    return "older schema unsupported";
                case NewerSchema:
                    return "newer schema";
                case InvalidEnvelope:
                    return "invalid envelope";
                case Current:
                    return "current";
            }
            return "unknown compatibility state";
        }

        [[nodiscard]] const ErrorCodeDescriptor &ComponentIssueCode(const Gameplay::ComponentInspectionStatus status) noexcept {
            using enum Gameplay::ComponentInspectionStatus;
            if (status == MissingDescriptor)
                return Gameplay::GameplayErrors::ComponentDescriptorMissing;
            if (status == InvalidEnvelope)
                return Gameplay::GameplayErrors::InvalidSerializedComponent;
            return Gameplay::GameplayErrors::ComponentSchemaIncompatible;
        }

        [[nodiscard]] Error GameplayComponentPlayError(const SceneGameplayInspection &inspection) {
            Error error = MakeError(Gameplay::GameplayErrors::GameplayPlayBlocked, std::format("Play Mode is blocked by {} unavailable "
                                                                                               "or incompatible gameplay component(s).",
                                                                                               inspection.issues.size()));
            error.diagnostics.reserve(inspection.issues.size());
            for (const SceneGameplayComponentIssue &issue : inspection.issues) {
                const ErrorCodeDescriptor &issueCode = ComponentIssueCode(issue.status);
                const std::string typeId = issue.typeId.IsValid() ? issue.typeId.Value() : "<invalid>";
                error.diagnostics.push_back(Diagnostic{
                    .code = DiagnosticCode{issueCode.code.Value()},
                    .severity = DiagnosticSeverity::Error,
                    .message = std::format("Object {} references gameplay component '{}' ({}).", issue.object.value, typeId,
                                           ComponentStatusText(issue.status)),
                    .location = SourceLocation{"scene", 0, 0},
                    .path = std::format("objects[{}].components.gameplayComponents[{}]", issue.object.value, issue.componentIndex),
                });
            }
            return error;
        }

        std::optional<Error> ValidateViewSelection(const EditorPlayViewSource source, const EditorPlaySceneFactory &factory,
                                                   const IEditorPlayXrHost *host) {
            using enum Horo::Editor::EditorPlayViewSource;
            if (source > Simulator || (source != AuthoredCamera && (!factory || !host)) || (source == AuthoredCamera && host))
                return InvalidTransition("Selected XR Play requires its actual application factory and host.");
            if (host && host->ViewSource() != source)
                return InvalidTransition("Selected Play view source must match its actual application host.");
            return {};
        }

        [[nodiscard]] std::optional<Error> ValidatePlayPrerequisites(const SceneDocumentSnapshot &authoring,
                                                                     const Gameplay::ComponentRegistry &components,
                                                                     const EditorPlayViewSource viewSource) {
            if (viewSource == EditorPlayViewSource::AuthoredCamera &&
                std::ranges::none_of(authoring.objects, [](const SceneObjectSnapshot &object) {
                return object.components.camera.has_value() && object.components.camera->enabled;
            }))
                return InvalidTransition("Play Mode requires an authored camera component.");
            if (const SceneGameplayInspection inspection = InspectSceneGameplayComponents(authoring.objects, components);
                inspection.HasBlockingIssues())
                return GameplayComponentPlayError(inspection);
            return std::nullopt;
        }
    }  // namespace

    /** @brief Requires the containing workspace to transfer genuine pending ownership before destroying borrowed registry state. */
    EditorPlaySessionController::~EditorPlaySessionController() {
        Stop();
        if (sceneHost_)
            std::terminate();
    }

    /** @copydoc EditorPlaySessionController::Start */
    Result<void> EditorPlaySessionController::Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                                    const EditorPlaySceneFactory &factory,
                                                    const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding,
                                                    const EditorPlayViewSource viewSource, const IEditorPlayXrHost *selectedXrHost) {
        static const Gameplay::ComponentRegistry missingComponents = [] {
            Gameplay::ComponentRegistry registry;
            static_cast<void>(registry.Freeze());
            return registry;
        }();
        return Start(authoring, registry, missingComponents, factory, triggerBinding, viewSource, selectedXrHost);
    }

    /** @copydoc EditorPlaySessionController::Start */
    Result<void> EditorPlaySessionController::Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                                    const Gameplay::ComponentRegistry &components, const EditorPlaySceneFactory &factory,
                                                    const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding,
                                                    const EditorPlayViewSource viewSource, const IEditorPlayXrHost *selectedXrHost) {
        if (state_ != EditorPlaySessionState::Idle && state_ != EditorPlaySessionState::Failed)
            return Result<void>::Failure(InvalidTransition("A play session is already active."));
        Stop();
        state_ = EditorPlaySessionState::Starting;
        lastError_.reset();
        authoringRevision_ = authoring.revision;
        if (const auto error = ValidateViewSelection(viewSource, factory, selectedXrHost)) {
            Fail(*error);
            return Result<void>::Failure(*error);
        }
        viewSource_ = viewSource;
        selectedXrHost_ = selectedXrHost;

        if (std::optional<Error> prerequisiteError = ValidatePlayPrerequisites(authoring, components, viewSource_);
            prerequisiteError.has_value()) {
            Error error = std::move(*prerequisiteError);
            Fail(error);
            return Result<void>::Failure(std::move(error));
        }

        if (nextRuntimeId_ == 0) {
            auto error = InvalidTransition("The reserved Play Scene identity space is exhausted.");
            Fail(error);
            return Result<void>::Failure(std::move(error));
        }
        if (!components.IsFrozen()) {
            const auto error = InvalidTransition("Play requires frozen project component metadata.");
            Fail(error);
            return Result<void>::Failure(error);
        }
        prefabComponents_ = components;
        auto converted = ConvertSceneDocumentToRuntime(authoring, Runtime::SceneDefinitionId{2}, triggerBinding);
        if (converted.HasError()) {
            Fail(converted.ErrorValue());
            return Result<void>::Failure(converted.ErrorValue());
        }
        if (auto prepared = BeginPreparation(std::move(converted).Value(), registry, factory); prepared.HasError()) {
            Fail(prepared.ErrorValue());
            return prepared;
        }
        return AdvancePreparation();
    }

    /** @copydoc EditorPlaySessionController::ReservePlayOwnership */
    Result<void> EditorPlaySessionController::ReservePlayOwnership() {
        if (saveRetirement_) {
            auto reserved = saveRetirement_->Reserve();
            if (reserved.HasError())
                return Result<void>::Failure(reserved.ErrorValue());
            retirementReservation_ = std::move(reserved).Value();
        }
        try {
            runtimeBinding_ = std::make_shared<EditorPlayRuntimeBinding>();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            retirementReservation_ = {};
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc EditorPlaySessionController::BeginPreparation */
    Result<void> EditorPlaySessionController::BeginPreparation(Runtime::RuntimeSceneDefinition definition,
                                                               const Gameplay::BehaviorRegistry &registry,
                                                               const EditorPlaySceneFactory &factory) {
        if (const auto reserved = ReservePlayOwnership(); reserved.HasError())
            return reserved;
        const Runtime::SceneRuntimeId identity{nextRuntimeId_};
        auto prepared = factory ? factory(identity, definition)
                                : Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::make_unique<AssetlessPlaySceneHost>(identity));
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (!prepared.Value())
            return Result<void>::Failure(InvalidTransition("The Play Scene host is unavailable."));
        if (viewSource_ != EditorPlayViewSource::AuthoredCamera && prepared.Value()->SelectedXrHost() != selectedXrHost_)
            return Result<void>::Failure(InvalidTransition("Play factory does not own the selected XR participant."));
        sceneHost_ = std::move(prepared).Value();
        ++nextRuntimeId_;
        preparationCancellation_ = CancellationSource{};
        preparationFrame_ = 1;
        if (const auto prepared = PrepareBehaviorOwner(registry); prepared.HasError())
            return prepared;
        if (const auto registered =
                sceneHost_->Scenes().AddStructuralParticipant(std::make_unique<PlayGameplayParticipant>(runtimeBinding_));
            registered.HasError())
            return registered;
        return StartPreparedScene(std::move(definition), registry);
    }

    /** @copydoc EditorPlaySessionController::StartPreparedScene */
    Result<void> EditorPlaySessionController::StartPreparedScene(Runtime::RuntimeSceneDefinition definition,
                                                                 const Gameplay::BehaviorRegistry &registry) {
        if (const auto started = sceneHost_->Scenes().Startup(preparationCancellation_.Token()); started.HasError())
            return started;
        if (const auto queued = sceneHost_->Scenes().QueuePreparation(std::move(definition)); queued.HasError())
            return queued;
        pendingRegistry_ = &registry;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::PrepareBehaviorOwner */
    Result<void> EditorPlaySessionController::PrepareBehaviorOwner(const Gameplay::BehaviorRegistry &registry) {
        auto created = Gameplay::BehaviorRuntimeSceneOwner::Create(sceneHost_->Scenes(), registry);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        behaviorOwner_ = std::move(created).Value();
        runtimeBinding_->owner = behaviorOwner_;
        return sceneHost_->Scenes().AddActivationParticipant(behaviorOwner_->MakeActivationParticipant());
    }

    /** @copydoc EditorPlaySessionController::RefreshBehaviorBinding */
    void EditorPlaySessionController::RefreshBehaviorBinding() {
        auto *current = behaviorOwner_ ? behaviorOwner_->Active() : nullptr;
        RefreshModuleScriptEpoch();
        if (current == behaviors_)
            return;
        behaviors_ = current;
        if (runtimeBinding_)
            runtimeBinding_->runtime = current;
        if (debugger_) {
            debugger_->Producer().Detach();
            if (current) {
                (void)debugger_->Begin(sceneHost_->Scenes().ActiveScene()->RuntimeId().value, ++diagnosticGeneration_, false);
                (void)debugger_->Publish(debugger_->Producer().Source());
            }
        }
    }

    /** @copydoc EditorPlaySessionController::AdvancePreparation */
    Result<void> EditorPlaySessionController::AdvancePreparation() {
        if (state_ != EditorPlaySessionState::Starting)
            return Result<void>::Success();
        const auto token = preparationCancellation_.Token();
        const Runtime::FrameContext frame{.frameNumber = preparationFrame_++, .cancellation = token};
        auto &scenes = sceneHost_->Scenes();
        if (const auto advanced = sceneHost_->CommitSceneChanges(frame); advanced.HasError()) {
            Fail(advanced.ErrorValue());
            return advanced;
        }
        if (auto error = scenes.TakeOperationError()) {
            Fail(*error);
            return Result<void>::Failure(std::move(*error));
        }
        if (!scenes.ActiveScene())
            return Result<void>::Success();
        auto services = sceneHost_->GameplayServices();
        if (services.HasError()) {
            Fail(services.ErrorValue());
            return Result<void>::Failure(services.ErrorValue());
        }
        auto prefabs = sceneHost_->PrefabServices(*pendingRegistry_, prefabComponents_);
        if (prefabs.HasError()) {
            Fail(prefabs.ErrorValue());
            return Result<void>::Failure(prefabs.ErrorValue());
        }
        const auto activated = behaviorOwner_->ActivatePublished(std::move(services).Value(), std::move(prefabs).Value());
        pendingRegistry_ = nullptr;
        if (activated.HasError()) {
            Fail(activated.ErrorValue());
            return activated;
        }
        RefreshBehaviorBinding();
        ConfigureReadySave();
        state_ = EditorPlaySessionState::Playing;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::CommitSceneChanges */
    Result<void> EditorPlaySessionController::CommitSceneChanges() {
        const auto token = preparationCancellation_.Token();
        const Runtime::FrameContext frame{.frameNumber = preparationFrame_++, .cancellation = token};
        auto &scenes = sceneHost_->Scenes();
        if (const auto committed = sceneHost_->CommitSceneChanges(frame); committed.HasError())
            return committed;
        if (auto failure = scenes.TakeOperationError())
            return Result<void>::Failure(std::move(*failure));
        RefreshBehaviorBinding();
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Pause */
    Result<void> EditorPlaySessionController::Pause() {
        if (fixedAttempt_)
            return Result<void>::Failure(InvalidTransition("Play control waits for fixed commit or abort."));
        if (state_ != EditorPlaySessionState::Playing)
            return Result<void>::Failure(InvalidTransition("Only a playing session can be paused."));
        state_ = EditorPlaySessionState::Paused;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Resume */
    Result<void> EditorPlaySessionController::Resume() {
        if (fixedAttempt_)
            return Result<void>::Failure(InvalidTransition("Play control waits for fixed commit or abort."));
        if (state_ != EditorPlaySessionState::Paused)
            return Result<void>::Failure(InvalidTransition("Only a paused session can be resumed."));
        stepPending_ = false;
        state_ = EditorPlaySessionState::Playing;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Step */
    Result<void> EditorPlaySessionController::Step() {
        if (fixedAttempt_)
            return Result<void>::Failure(InvalidTransition("Play control waits for fixed commit or abort."));
        if (state_ != EditorPlaySessionState::Paused)
            return Result<void>::Failure(InvalidTransition("Step is available only while paused."));
        stepPending_ = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::ConfigureSaveRetirement */
    void EditorPlaySessionController::ConfigureSaveRetirement(std::shared_ptr<EditorPlaySaveRetirement> owner) noexcept {
        if (!sceneHost_)
            saveRetirement_ = std::move(owner);
    }

    /** @copydoc EditorPlaySessionController::SaveSession */
    EditorPlaySaveSession *EditorPlaySessionController::SaveSession() noexcept {
        return sceneHost_ ? sceneHost_->SaveSession() : nullptr;
    }

    /** @copydoc EditorPlaySessionController::SaveBinding */
    std::weak_ptr<EditorPlayRuntimeBinding> EditorPlaySessionController::SaveBinding() const noexcept {
        return runtimeBinding_;
    }

    /** @copydoc EditorPlaySessionController::ConfigureSaveBindings */
    void EditorPlaySessionController::ConfigureSaveBindings(std::vector<PlaySaveGameplayBinding> bindings) {
        saveSetupError_.reset();
        saveBindings_ = std::move(bindings);
        if (behaviors_)
            ConfigureReadySave();
    }

    /** @copydoc EditorPlaySessionController::PrepareSaveRetirement */
    bool EditorPlaySessionController::PrepareSaveRetirement() noexcept {
        if (!sceneHost_)
            return true;
        if (runtimeBinding_)
            runtimeBinding_->save = nullptr;
        return sceneHost_->RetireSaveSession();
    }

    /** @copydoc EditorPlaySessionController::ConfigureSaveError */
    void EditorPlaySessionController::ConfigureSaveError(Error error) {
        saveBindings_.clear();
        saveSetupError_ = std::move(error);
        if (runtimeBinding_)
            runtimeBinding_->saveError = saveSetupError_;
    }

    /** @copydoc EditorPlaySessionController::ConfigureReadySave */
    void EditorPlaySessionController::ConfigureReadySave() {
        runtimeBinding_->save = sceneHost_->SaveSession();
        if (saveSetupError_) {
            runtimeBinding_->saveError = saveSetupError_;
            return;
        }
        const auto configured = sceneHost_->ConfigureSaveSession(std::move(saveBindings_), behaviorOwner_);
        runtimeBinding_->saveError = configured.HasError() ? std::optional<Error>{configured.ErrorValue()} : std::nullopt;
        runtimeBinding_->save = sceneHost_->SaveSession();
    }

    /** @copydoc EditorPlaySessionController::DispatchSaveCommand */
    void EditorPlaySessionController::DispatchSaveCommand() {
        if (!runtimeBinding_ || runtimeBinding_->revoked || !runtimeBinding_->saveCommand)
            return;
        auto command = std::move(*runtimeBinding_->saveCommand);
        runtimeBinding_->saveCommand.reset();
        if (!runtimeBinding_->save)
            return;
        if (command.kind == PlaySaveCommandKind::Load && state_ == EditorPlaySessionState::Playing)
            static_cast<void>(Pause());
        const auto admitted = runtimeBinding_->save->Request(command);
        runtimeBinding_->saveError = admitted.HasError() ? std::optional<Error>{admitted.ErrorValue()} : std::nullopt;
    }

    /** @copydoc EditorPlaySessionController::Stop */
    void EditorPlaySessionController::Stop() noexcept {
        if (fixedAttempt_) {
            stopPending_ = true;
            return;
        }
        stopPending_ = false;
        CancelModuleScripts();
        if (debugger_)
            debugger_->Producer().Detach();
        if (runtimeBinding_) {
            runtimeBinding_->revoked = true;
            runtimeBinding_->save = nullptr;
            runtimeBinding_->saveCommand.reset();
        }
        pendingRegistry_ = nullptr;
        stepPending_ = false;
        preparationCancellation_.RequestCancellation();
        if (auto *save = SaveSession())
            save->BeginRetirement();
        if (sceneHost_)
            sceneHost_->QuiesceGameplay();
        state_ = EditorPlaySessionState::Stopping;
        PollStopping();
    }

    /** @copydoc EditorPlaySessionController::PollStopping */
    void EditorPlaySessionController::PollStopping() noexcept {
        if (state_ != EditorPlaySessionState::Stopping)
            return;
        try {
            if (auto *save = SaveSession(); save && !save->IsSettled()) {
                const Runtime::FrameContext frame{.frameNumber = preparationFrame_++, .cancellation = preparationCancellation_.Token()};
                const auto advanced = sceneHost_->CommitSceneChanges(frame);
                const auto pumped = save->CommitAtSafePoint(frame);
                if (advanced.HasError())
                    lastError_ = advanced.ErrorValue();
                if (pumped.HasError())
                    lastError_ = pumped.ErrorValue();
                if (!save->IsSettled())
                    return;
            }
            FinishStop();
        } catch (...) {
            // Preserve all owners; the reserved application slot is the leave/shutdown
            // fallback.
        }
    }

    /** @copydoc EditorPlaySessionController::FinishStop */
    void EditorPlaySessionController::FinishStop() noexcept {
        if (runtimeBinding_)
            runtimeBinding_->runtime = nullptr;
        behaviors_ = nullptr;
        if (sceneHost_) {
            nextRuntimeId_ = sceneHost_->Scenes().NextRuntimeId().value;
            sceneHost_->Shutdown();
        }
        behaviorOwner_.reset();
        sceneHost_.reset();
        runtimeBinding_.reset();
        retirementReservation_ = {};
        prefabComponents_ = Gameplay::ComponentRegistry{};
        authoringRevision_ = {};
        state_ = EditorPlaySessionState::Idle;
    }

    /** @copydoc EditorPlaySessionController::RetireOnLeave */
    void EditorPlaySessionController::RetireOnLeave(std::unique_ptr<ProjectGameplayRegistry> &registry) noexcept {
        Stop();
        if (!sceneHost_)
            return;
        if (!retirementReservation_.IsValid())
            std::terminate();
        EditorPlayRetiredUnit unit;
        unit.registry = std::move(registry);
        unit.scene = std::move(sceneHost_);
        unit.behaviorOwner = std::move(behaviorOwner_);
        behaviors_ = nullptr;
        unit.binding = std::move(runtimeBinding_);
        unit.cancellation = preparationCancellation_.Token();
        unit.frame = preparationFrame_;
        retirementReservation_.Retain(std::move(unit));
        state_ = EditorPlaySessionState::Idle;
    }

    /** @copydoc EditorPlaySessionController::FixedUpdate */
    Result<void> EditorPlaySessionController::FixedUpdate(std::span<const Gameplay::GameplayInputAction> input,
                                                          const Gameplay::FixedDeltaTime delta) {
        if (fixedAttempt_ || dispatchSource_.IsValid())
            return Result<void>::Failure(InvalidTransition("Legacy fixed update cannot bypass a bound runtime dispatch."));
        FlushPendingStop();
        if (const auto prepared = AdvancePreparation(); prepared.HasError())
            return prepared;
        if (!ShouldTick())
            return Result<void>::Success();
        stepPending_ = false;
        if (auto gameplay = RunGameplayFixed(input, delta); gameplay.HasError()) {
            Fail(gameplay.ErrorValue());
            return gameplay;
        }
        if (const auto native = sceneHost_->FixedUpdate(delta); native.HasError()) {
            Fail(native.ErrorValue());
            return native;
        }
        return Result<void>::Success();
    }

    bool EditorPlaySessionController::ShouldTick() const noexcept {
        return state_ == EditorPlaySessionState::Playing || (state_ == EditorPlaySessionState::Paused && stepPending_);
    }

    Result<void> EditorPlaySessionController::RunGameplayFixed(std::span<const Gameplay::GameplayInputAction> input,
                                                               const Gameplay::FixedDeltaTime delta) {
        if (auto admitted = sceneHost_->BeginFixedUpdate(delta); admitted.HasError())
            return admitted;
        if (auto updated = behaviors_->FixedUpdate(input, delta); updated.HasError())
            return updated;
        return CommitSceneChanges();
    }

    /** @copydoc EditorPlaySessionController::PresentationUpdate */
    void EditorPlaySessionController::PresentationUpdate(const Gameplay::FrameDeltaTime delta) {
        if (fixedAttempt_)
            return;
        FlushPendingStop();
        PollStopping();
        DispatchSaveCommand();
        if (const auto prepared = AdvancePreparation(); prepared.HasError())
            return;
        if (behaviors_ && (state_ == EditorPlaySessionState::Playing || state_ == EditorPlaySessionState::Paused)) {
            if (const auto begun = sceneHost_->BeginPresentationUpdate(); begun.HasError()) {
                Fail(begun.ErrorValue());
                return;
            }
            if (!SaveLoading())
                behaviors_->PresentationUpdate(delta);
            if (const auto committed = CommitSceneChanges(); committed.HasError())
                Fail(committed.ErrorValue());
        }
        PollModuleScripts();
    }

    /** @brief Reads immutable actual load state; pending restore advances only the existing Scene lifecycle, not project callbacks. */
    bool EditorPlaySessionController::SaveLoading() noexcept {
        const auto *save = SaveSession();
        const auto snapshot = save ? save->Snapshot() : nullptr;
        return snapshot && snapshot->loading;
    }

    /** @copydoc EditorPlaySessionController::ReloadBehaviors */
    Result<void> EditorPlaySessionController::ReloadBehaviors(const Gameplay::BehaviorRegistry &candidate,
                                                              const Gameplay::BehaviorRegistry &rollback) {
        auto snapshot = QuiesceForReload();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (Result<void> replacement = RestoreAfterReload(candidate, snapshot.Value()); replacement.HasValue())
            return Result<void>::Success();
        Error candidateError = lastError_.value_or(MakeError(Gameplay::GameplayErrors::GameplayReloadRestoreFailed));
        if (Result<void> restored = RestoreAfterReload(rollback, snapshot.Value()); restored.HasValue())
            return Result<void>::Failure(std::move(candidateError));
        Fail(lastError_.value_or(MakeError(Gameplay::GameplayErrors::GameplayReloadRestoreFailed)));
        return Result<void>::Failure(std::move(candidateError));
    }

    /** @copydoc EditorPlaySessionController::QuiesceForReload */
    Result<EditorPlayReloadSnapshot> EditorPlaySessionController::QuiesceForReload() {
        using enum EditorPlaySessionState;
        if (fixedAttempt_ || !sceneHost_ || !behaviors_ || (state_ != Playing && state_ != Paused))
            return Result<EditorPlayReloadSnapshot>::Failure(InvalidTransition("Native reload requires an active play session."));
        if (!PrepareSaveRetirement())
            return Result<EditorPlayReloadSnapshot>::Failure(MakeError(Runtime::SaveErrors::OperationInProgress));
        auto captured = CaptureReloadSafely(*behaviors_);
        if (captured.HasError())
            return Result<EditorPlayReloadSnapshot>::Failure(captured.ErrorValue());
        EditorPlayReloadSnapshot snapshot{std::move(captured).Value(), state_};
        const auto quiesced = behaviorOwner_->QuiesceForReload();
        if (quiesced.HasError())
            return Result<EditorPlayReloadSnapshot>::Failure(quiesced.ErrorValue());
        CancelModuleScripts();
        // The owner completed fallible preparation and revoked its real child scopes before destroying callbacks.
        state_ = Reloading;
        sceneHost_->QuiesceGameplay();
        if (debugger_)
            debugger_->Producer().Detach();
        runtimeBinding_->runtime = nullptr;
        behaviors_ = nullptr;
        return Result<EditorPlayReloadSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc EditorPlaySessionController::RestoreAfterReload */
    Result<void> EditorPlaySessionController::RestoreAfterReload(const Gameplay::BehaviorRegistry &registry,
                                                                 const EditorPlayReloadSnapshot &snapshot) {
        if (!sceneHost_ || state_ != EditorPlaySessionState::Reloading || behaviors_)
            return Result<void>::Failure(InvalidTransition("No quiesced native reload transaction is active."));
        auto services = sceneHost_->GameplayServices();
        if (services.HasError()) {
            lastError_ = services.ErrorValue();
            return Result<void>::Failure(services.ErrorValue());
        }
        auto prefabs = sceneHost_->PrefabServices(registry, prefabComponents_);
        if (prefabs.HasError()) {
            lastError_ = prefabs.ErrorValue();
            return Result<void>::Failure(prefabs.ErrorValue());
        }
        const auto restored =
            behaviorOwner_->ResumeAfterReload(registry, snapshot.behaviors, std::move(services).Value(), std::move(prefabs).Value());
        if (restored.HasError()) {
            lastError_ = restored.ErrorValue();
            return restored;
        }
        RefreshBehaviorBinding();
        lastError_.reset();
        state_ = snapshot.priorState;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::DegradeAfterReload */
    void EditorPlaySessionController::DegradeAfterReload(Error error) noexcept {
        Fail(std::move(error));
    }

    EditorPlaySessionState EditorPlaySessionController::State() const noexcept {
        return state_;
    }

    bool EditorPlaySessionController::IsActive() const noexcept {
        using enum EditorPlaySessionState;
        return state_ == Starting || state_ == Playing || state_ == Paused || state_ == Reloading || state_ == Stopping;
    }

    std::optional<Runtime::RuntimeSceneView> EditorPlaySessionController::SceneView() const noexcept {
        return sceneHost_ ? sceneHost_->Scenes().ActiveScene() : std::nullopt;
    }

    std::optional<EditorSceneCollisionReceipt> EditorPlaySessionController::CollisionReceipt() const noexcept {
        return sceneHost_ ? sceneHost_->CollisionReceipt() : std::nullopt;
    }

    const std::optional<Error> &EditorPlaySessionController::LastError() const noexcept {
        return lastError_;
    }

    DocumentRevision EditorPlaySessionController::AuthoringRevision() const noexcept {
        return authoringRevision_;
    }

    /** @brief Preserves actual pending save ownership when a Play callback fails. */
    void EditorPlaySessionController::Fail(Error error) noexcept {
        Stop();
        lastError_ = std::move(error);
        if (!sceneHost_)
            state_ = EditorPlaySessionState::Failed;
    }
}  // namespace Horo::Editor
