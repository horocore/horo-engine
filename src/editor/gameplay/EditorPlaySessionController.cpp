#include "editor/gameplay/EditorPlaySessionController.h"

#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>
#include <format>
#include <ranges>
#include <string_view>

namespace Horo::Editor {
    namespace {
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
                                                                     const Gameplay::ComponentRegistry &components) {
            if (std::ranges::none_of(authoring.objects, [](const SceneObjectSnapshot &object) {
                return object.components.camera.has_value() && object.components.camera->enabled;
            }))
                return InvalidTransition("Play Mode requires an authored camera component.");
            const SceneGameplayInspection inspection = InspectSceneGameplayComponents(authoring.objects, components);
            if (inspection.HasBlockingIssues())
                return GameplayComponentPlayError(inspection);
            return std::nullopt;
        }
    }  // namespace

    /** @copydoc EditorPlaySessionController::Start */
    Result<void> EditorPlaySessionController::Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                                    std::unique_ptr<Runtime::RuntimeScene> preparedScene) {
        static const Gameplay::ComponentRegistry missingComponents = [] {
            Gameplay::ComponentRegistry registry;
            static_cast<void>(registry.Freeze());
            return registry;
        }();
        return Start(authoring, registry, missingComponents, std::move(preparedScene));
    }

    /** @copydoc EditorPlaySessionController::Start */
    Result<void> EditorPlaySessionController::Start(const SceneDocumentSnapshot &authoring, const Gameplay::BehaviorRegistry &registry,
                                                    const Gameplay::ComponentRegistry &components,
                                                    std::unique_ptr<Runtime::RuntimeScene> preparedScene) {
        if (state_ != EditorPlaySessionState::Idle && state_ != EditorPlaySessionState::Failed)
            return Result<void>::Failure(InvalidTransition("A play session is already active."));
        Stop();
        state_ = EditorPlaySessionState::Starting;
        lastError_.reset();
        authoringRevision_ = authoring.revision;

        if (std::optional<Error> prerequisiteError = ValidatePlayPrerequisites(authoring, components); prerequisiteError.has_value()) {
            Error error = std::move(*prerequisiteError);
            Fail(error);
            return Result<void>::Failure(std::move(error));
        }

        if (preparedScene) {
            if (preparedScene->View().DefinitionRevision().value != authoring.state.value) {
                Error error = InvalidTransition("The prepared authoring preview is stale; wait for scene synchronization and retry.");
                Fail(error);
                return Result<void>::Failure(std::move(error));
            }
            scene_ = std::move(preparedScene);
        } else {
            Result<Runtime::RuntimeSceneDefinition> converted = ConvertSceneDocumentToRuntime(authoring, Runtime::SceneDefinitionId{2});
            if (converted.HasError()) {
                Error error = converted.ErrorValue();
                Fail(error);
                return Result<void>::Failure(std::move(error));
            }
            Result<std::unique_ptr<Runtime::RuntimeScene>> created =
                Runtime::RuntimeScene::Create(converted.Value(), Runtime::SceneRuntimeId{nextRuntimeId_++});
            if (created.HasError()) {
                Error error = created.ErrorValue();
                Fail(error);
                return Result<void>::Failure(std::move(error));
            }
            scene_ = std::move(created).Value();
        }

        Result<std::unique_ptr<Gameplay::BehaviorRuntime>> runtime = Gameplay::BehaviorRuntime::Create(*scene_, registry);
        if (runtime.HasError()) {
            Error error = runtime.ErrorValue();
            Fail(error);
            return Result<void>::Failure(std::move(error));
        }
        behaviors_ = std::move(runtime).Value();
        state_ = EditorPlaySessionState::Playing;
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
        if (behaviors_)
            behaviors_->Shutdown();
        behaviors_.reset();
        scene_.reset();
        stepPending_ = false;
        authoringRevision_ = {};
        state_ = EditorPlaySessionState::Idle;
    }

    /** @copydoc EditorPlaySessionController::FixedUpdate */
    Result<void> EditorPlaySessionController::FixedUpdate(std::span<const Gameplay::GameplayInputAction> input,
                                                          const Gameplay::FixedDeltaTime delta) {
        if (const bool shouldTick = state_ == EditorPlaySessionState::Playing || (state_ == EditorPlaySessionState::Paused && stepPending_);
            !shouldTick)
            return Result<void>::Success();
        stepPending_ = false;
        if (Result<void> updated = behaviors_->FixedUpdate(input, delta); updated.HasError()) {
            Error error = updated.ErrorValue();
            Fail(error);
            return Result<void>::Failure(std::move(error));
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySessionController::PresentationUpdate */
    void EditorPlaySessionController::PresentationUpdate(const Gameplay::FrameDeltaTime delta) {
        if (behaviors_ && (state_ == EditorPlaySessionState::Playing || state_ == EditorPlaySessionState::Paused))
            behaviors_->PresentationUpdate(delta);
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
        if (!scene_ || !behaviors_ || (state_ != Playing && state_ != Paused))
            return Result<EditorPlayReloadSnapshot>::Failure(InvalidTransition("Native reload requires an active play session."));
        auto captured = behaviors_->CaptureReloadSnapshot();
        if (captured.HasError())
            return Result<EditorPlayReloadSnapshot>::Failure(captured.ErrorValue());
        EditorPlayReloadSnapshot snapshot{std::move(captured).Value(), state_};
        state_ = Reloading;
        behaviors_->Shutdown();
        behaviors_.reset();
        return Result<EditorPlayReloadSnapshot>::Success(std::move(snapshot));
    }

    /** @copydoc EditorPlaySessionController::RestoreAfterReload */
    Result<void> EditorPlaySessionController::RestoreAfterReload(const Gameplay::BehaviorRegistry &registry,
                                                                 const EditorPlayReloadSnapshot &snapshot) {
        if (!scene_ || state_ != EditorPlaySessionState::Reloading || behaviors_)
            return Result<void>::Failure(InvalidTransition("No quiesced native reload transaction is active."));
        auto replacement = Gameplay::BehaviorRuntime::Create(*scene_, registry);
        if (replacement.HasError()) {
            lastError_ = replacement.ErrorValue();
            return Result<void>::Failure(replacement.ErrorValue());
        }
        behaviors_ = std::move(replacement).Value();
        if (Result<void> restored = behaviors_->RestoreReloadSnapshot(snapshot.behaviors); restored.HasError()) {
            Error error = restored.ErrorValue();
            behaviors_->Shutdown();
            behaviors_.reset();
            lastError_ = error;
            return Result<void>::Failure(std::move(error));
        }
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

    Runtime::RuntimeScene *EditorPlaySessionController::Scene() noexcept {
        return scene_.get();
    }

    const Runtime::RuntimeScene *EditorPlaySessionController::Scene() const noexcept {
        return scene_.get();
    }

    const std::optional<Error> &EditorPlaySessionController::LastError() const noexcept {
        return lastError_;
    }

    DocumentRevision EditorPlaySessionController::AuthoringRevision() const noexcept {
        return authoringRevision_;
    }

    void EditorPlaySessionController::Fail(Error error) noexcept {
        if (behaviors_)
            behaviors_->Shutdown();
        behaviors_.reset();
        scene_.reset();
        stepPending_ = false;
        lastError_ = std::move(error);
        state_ = EditorPlaySessionState::Failed;
    }
}  // namespace Horo::Editor
