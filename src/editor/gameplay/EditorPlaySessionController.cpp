#include "editor/gameplay/EditorPlaySessionController.h"

#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>
#include <format>
#include <functional>
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
            explicit PlayGameplayParticipant(std::function<Gameplay::BehaviorRuntime *()> runtime) : runtime_(std::move(runtime)) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &scene,
                                                                               std::span<const Runtime::RuntimeEntityView> created,
                                                                               std::span<const Runtime::EntityRef> destroyed) override {
                auto *runtime = runtime_();
                if (!runtime)
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent, "Play has no active Gameplay structural owner."));
                return runtime->MakeStructuralParticipant()->Prepare(scene, created, destroyed);
            }

        private:
            // The Scene host is retired by the controller before its captured owner dies.
            // Resolve only during synchronous structural preparation so reload replacement stays visible.
            std::function<Gameplay::BehaviorRuntime *()> runtime_;
        };

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
            Error error = MakeError(Gameplay::GameplayErrors::GameplayPlayBlocked,
                                    std::format("Play Mode is blocked by {} unavailable or incompatible gameplay component(s).",
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
        if (viewSource > EditorPlayViewSource::SelectedXr ||
            (viewSource == EditorPlayViewSource::SelectedXr && (!factory || !selectedXrHost)) ||
            (viewSource == EditorPlayViewSource::AuthoredCamera && selectedXrHost)) {
            const auto error = InvalidTransition("Selected XR Play requires its actual application factory and host.");
            Fail(error);
            return Result<void>::Failure(error);
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

    /** @copydoc EditorPlaySessionController::BeginPreparation */
    Result<void> EditorPlaySessionController::BeginPreparation(Runtime::RuntimeSceneDefinition definition,
                                                               const Gameplay::BehaviorRegistry &registry,
                                                               const EditorPlaySceneFactory &factory) {
        const Runtime::SceneRuntimeId identity{nextRuntimeId_};
        auto prepared = factory ? factory(identity, definition)
                                : Result<std::unique_ptr<EditorPlaySceneHost>>::Success(std::make_unique<AssetlessPlaySceneHost>(identity));
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        if (!prepared.Value())
            return Result<void>::Failure(InvalidTransition("The Play Scene host is unavailable."));
        if (viewSource_ == EditorPlayViewSource::SelectedXr && prepared.Value()->SelectedXrHost() != selectedXrHost_)
            return Result<void>::Failure(InvalidTransition("Play factory does not own the selected XR participant."));
        sceneHost_ = std::move(prepared).Value();
        ++nextRuntimeId_;
        preparationCancellation_ = CancellationSource{};
        preparationFrame_ = 1;
        if (const auto registered = sceneHost_->Scenes().AddStructuralParticipant(std::make_unique<PlayGameplayParticipant>([this] {
            return behaviors_.get();
        }));
            registered.HasError())
            return registered;
        if (const auto started = sceneHost_->Scenes().Startup(preparationCancellation_.Token()); started.HasError())
            return started;
        if (const auto queued = sceneHost_->Scenes().QueuePreparation(std::move(definition)); queued.HasError())
            return queued;
        pendingRegistry_ = &registry;
        return Result<void>::Success();
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
        auto runtime =
            Gameplay::BehaviorRuntime::Create(scenes, *pendingRegistry_, {}, std::move(services).Value(), std::move(prefabs).Value());
        pendingRegistry_ = nullptr;
        if (runtime.HasError()) {
            Fail(runtime.ErrorValue());
            return Result<void>::Failure(runtime.ErrorValue());
        }
        behaviors_ = std::move(runtime).Value();
        state_ = EditorPlaySessionState::Playing;
        if (debugger_) {
            (void)debugger_->Begin(scenes.ActiveScene()->RuntimeId().value, ++diagnosticGeneration_, false);
            (void)debugger_->Publish(debugger_->Producer().Source());
        }
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
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Pause */
    Result<void> EditorPlaySessionController::Pause() {
        if (state_ != EditorPlaySessionState::Playing)
            return Result<void>::Failure(InvalidTransition("Only a playing session can be paused."));
        state_ = EditorPlaySessionState::Paused;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Resume */
    Result<void> EditorPlaySessionController::Resume() {
        if (state_ != EditorPlaySessionState::Paused)
            return Result<void>::Failure(InvalidTransition("Only a paused session can be resumed."));
        stepPending_ = false;
        state_ = EditorPlaySessionState::Playing;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Step */
    Result<void> EditorPlaySessionController::Step() {
        if (state_ != EditorPlaySessionState::Paused)
            return Result<void>::Failure(InvalidTransition("Step is available only while paused."));
        stepPending_ = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::Stop */
    void EditorPlaySessionController::Stop() noexcept {
        using enum Horo::Editor::EditorPlaySessionState;
        if (state_ != EditorPlaySessionState::Idle)
            state_ = EditorPlaySessionState::Stopping;
        if (debugger_)
            debugger_->Producer().Detach();
        if (sceneHost_)
            sceneHost_->QuiesceGameplay();
        if (behaviors_)
            behaviors_->Shutdown();
        behaviors_.reset();
        pendingRegistry_ = nullptr;
        preparationCancellation_.RequestCancellation();
        if (sceneHost_)
            sceneHost_->Shutdown();
        sceneHost_.reset();
        prefabComponents_ = Gameplay::ComponentRegistry{};
        stepPending_ = false;
        authoringRevision_ = {};
        state_ = EditorPlaySessionState::Idle;
    }

    /** @copydoc EditorPlaySessionController::FixedUpdate */
    Result<void> EditorPlaySessionController::FixedUpdate(std::span<const Gameplay::GameplayInputAction> input,
                                                          const Gameplay::FixedDeltaTime delta) {
        if (const auto prepared = AdvancePreparation(); prepared.HasError())
            return prepared;
        if (const bool shouldTick = state_ == EditorPlaySessionState::Playing || (state_ == EditorPlaySessionState::Paused && stepPending_);
            !shouldTick)
            return Result<void>::Success();
        stepPending_ = false;
        if (const auto admitted = sceneHost_->BeginFixedUpdate(delta); admitted.HasError()) {
            Fail(admitted.ErrorValue());
            return admitted;
        }
        if (Result<void> updated = behaviors_->FixedUpdate(input, delta); updated.HasError()) {
            Error error = updated.ErrorValue();
            Fail(error);
            return Result<void>::Failure(std::move(error));
        }
        if (const auto committed = CommitSceneChanges(); committed.HasError()) {
            Fail(committed.ErrorValue());
            return committed;
        }
        if (const auto native = sceneHost_->FixedUpdate(delta); native.HasError()) {
            Fail(native.ErrorValue());
            return native;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::PresentationUpdate */
    void EditorPlaySessionController::PresentationUpdate(const Gameplay::FrameDeltaTime delta) {
        if (const auto prepared = AdvancePreparation(); prepared.HasError())
            return;
        if (behaviors_ && (state_ == EditorPlaySessionState::Playing || state_ == EditorPlaySessionState::Paused)) {
            behaviors_->PresentationUpdate(delta);
            if (const auto committed = CommitSceneChanges(); committed.HasError())
                Fail(committed.ErrorValue());
        }
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
        if (!sceneHost_ || !behaviors_ || (state_ != Playing && state_ != Paused))
            return Result<EditorPlayReloadSnapshot>::Failure(InvalidTransition("Native reload requires an active play session."));
        auto captured = behaviors_->CaptureReloadSnapshot();
        if (captured.HasError())
            return Result<EditorPlayReloadSnapshot>::Failure(captured.ErrorValue());
        EditorPlayReloadSnapshot snapshot{std::move(captured).Value(), state_};
        state_ = Reloading;
        sceneHost_->QuiesceGameplay();
        if (debugger_)
            debugger_->Producer().Detach();
        behaviors_->Shutdown();
        behaviors_.reset();
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
        auto replacement =
            Gameplay::BehaviorRuntime::Create(sceneHost_->Scenes(), registry, {}, std::move(services).Value(), std::move(prefabs).Value());
        if (replacement.HasError()) {
            lastError_ = replacement.ErrorValue();
            return Result<void>::Failure(replacement.ErrorValue());
        }
        behaviors_ = std::move(replacement).Value();
        if (Result<void> restored = behaviors_->RestoreReloadSnapshot(snapshot.behaviors); restored.HasError()) {
            Error error = restored.ErrorValue();
            sceneHost_->QuiesceGameplay();
            behaviors_->Shutdown();
            behaviors_.reset();
            lastError_ = error;
            return Result<void>::Failure(std::move(error));
        }
        lastError_.reset();
        state_ = snapshot.priorState;
        if (debugger_) {
            (void)debugger_->Begin(sceneHost_->Scenes().ActiveScene()->RuntimeId().value, ++diagnosticGeneration_, false);
            (void)debugger_->Publish(debugger_->Producer().Source());
        }
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

    void EditorPlaySessionController::Fail(Error error) noexcept {
        if (debugger_)
            debugger_->Producer().Detach();
        if (sceneHost_)
            sceneHost_->QuiesceGameplay();
        if (behaviors_)
            behaviors_->Shutdown();
        behaviors_.reset();
        pendingRegistry_ = nullptr;
        preparationCancellation_.RequestCancellation();
        if (sceneHost_)
            sceneHost_->Shutdown();
        sceneHost_.reset();
        prefabComponents_ = Gameplay::ComponentRegistry{};
        stepPending_ = false;
        lastError_ = std::move(error);
        state_ = EditorPlaySessionState::Failed;
    }
}  // namespace Horo::Editor
