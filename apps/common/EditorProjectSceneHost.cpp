#include "EditorProjectSceneHost.h"

#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "ScenePhysicsHost.h"

#include <cmath>
#include <limits>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        const ErrorCodeDescriptor
            InvalidProjectScene{ErrorDomainId{"application.scene"},
                                ErrorCode{"application.scene.project.invalid"},
                                ErrorSeverity::Error,
                                "The selected project cannot prepare this Scene",
                                "Provide complete project identity/collision settings and select committed cooked assets when required.",
                                false,
                                false,
                                {}};

        /** @brief Pins every borrowed provider, request group and native authority behind one Scene service. */
        class ProjectPlaySceneHost final : public Editor::EditorPlaySceneHost {
        public:
            ProjectPlaySceneHost(const Runtime::SceneRuntimeId identity, const ScenePhysicsIdentityRange range)
                : native_(range), identity_(identity) {}

            ~ProjectPlaySceneHost() override {
                Shutdown();
            }

            ProjectPlaySceneHost(const ProjectPlaySceneHost &) = delete;
            ProjectPlaySceneHost &operator=(const ProjectPlaySceneHost &) = delete;
            ProjectPlaySceneHost(ProjectPlaySceneHost &&) = delete;
            ProjectPlaySceneHost &operator=(ProjectPlaySceneHost &&) = delete;

            Result<void> Prepare(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                                 const Runtime::RuntimeSceneDefinition *definition) {
                if (!identity_.IsValid() || (root.empty() && policy.physicsMode != Physics::PhysicsRuntimeMode::Null))
                    return Result<void>::Failure(MakeError(InvalidProjectScene));
                if (auto captured = CaptureAssets(root, jobs, policy, definition); captured.HasError())
                    return captured;
                auto runtime = Physics::PhysicsRuntime::Create(policy.physicsMode);
                if (runtime.HasError())
                    return Result<void>::Failure(runtime.ErrorValue());
                physics_ = std::move(runtime).Value();
                auto settings = CaptureSettings(root, policy.physicsMode);
                if (settings.HasError())
                    return Result<void>::Failure(settings.ErrorValue());
                scenes_ = std::make_unique<Runtime::RuntimeSceneService>(registry_, *loads_, Runtime::RuntimeSceneAssetLimits{}, identity_);
                auto participant = native_.MakeParticipant(*physics_, authority_, std::move(settings).Value());
                if (participant.HasError())
                    return Result<void>::Failure(participant.ErrorValue());
                if (auto added = scenes_->AddActivationParticipant(std::move(participant).Value()); added.HasError())
                    return added;
                auto structural = native_.MakeStructuralParticipant();
                if (structural.HasError())
                    return Result<void>::Failure(structural.ErrorValue());
                return scenes_->AddStructuralParticipant(std::move(structural).Value());
            }

            /** @brief Transfers the preview service to its actual RuntimeHost scheduler; resources remain pinned here. */
            std::unique_ptr<Runtime::RuntimeSceneService> ReleaseScenes() noexcept {
                return std::move(scenes_);
            }

            Physics::PhysicsRuntimeMode PhysicsMode() const noexcept {
                return physics_->Mode();
            }

            Runtime::RuntimeSceneService &Scenes() noexcept override {
                return *scenes_;
            }

            std::optional<Editor::EditorSceneCollisionReceipt> CollisionReceipt() const noexcept override {
                if (std::this_thread::get_id() != ownerLane_ || !physics_)
                    return std::nullopt;
                if (physics_->Mode() == Physics::PhysicsRuntimeMode::Null)
                    return Editor::EditorSceneCollisionReceipt{};
                if (!collision_)
                    return std::nullopt;
                const auto &input = collision_->Input();
                return Editor::EditorSceneCollisionReceipt{native_.HasPublishedScene() ? Editor::EditorScenePhysicsState::Active
                                                                                       : Editor::EditorScenePhysicsState::Inactive,
                                                           input.projectId, input.revision, capturedFilterGeneration_,
                                                           collision_->Schema()->SemanticFingerprint()};
            }

            Result<void> FixedUpdate(const Gameplay::FixedDeltaTime delta) override {
                // The existing Editor screen contract supplies seconds. Convert that one host
                // quantum once to the nearest representable nanosecond, never from frame time.
                if (!std::isfinite(delta.seconds) || delta.seconds <= 0 ||
                    delta.seconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max()) / 1'000'000'000.0)
                    return Result<void>::Failure(
                        MakeError(InvalidProjectScene, "Play requires a finite positive fixed quantum representable in nanoseconds."));
                return native_.AdvanceFixedTick(Duration::FromNanoseconds(std::llround(delta.seconds * 1'000'000'000.0)));
            }

            void Shutdown() noexcept override {
                if (scenes_)
                    scenes_->Shutdown();
                if (loads_)
                    loads_->Shutdown();
                if (physics_)
                    physics_->Shutdown();
            }

        private:
            /** @brief Captures immutable registry/cook payloads before constructing any Scene/native owner. */
            Result<void> CaptureAssets(const std::filesystem::path &root, JobSystem &jobs, const EditorProjectScenePolicy &policy,
                                       const Runtime::RuntimeSceneDefinition *definition) {
                if (!root.empty()) {
                    auto candidate = Assets::PrepareAssetRegistryCandidate(root);
                    if (candidate.HasError())
                        return Result<void>::Failure(candidate.ErrorValue());
                    auto report = registry_.Publish(std::move(candidate).Value());
                    if (report.status != Assets::AssetRegistryBuildStatus::Complete)
                        return Result<void>::Failure(report.diagnostics.empty() ? MakeError(InvalidProjectScene)
                                                                                : report.diagnostics.front().error);
                }
                if (definition && !definition->AssetDependencies().empty() && !policy.cooked)
                    return Result<void>::Failure(
                        MakeError(InvalidProjectScene,
                                  "This Play Scene requires committed assets: supply --cooked-root and --cook-target."));
                if (policy.cooked) {
                    auto captured = CommittedSceneAssets::Capture(*policy.cooked, registry_.Snapshot());
                    if (captured.HasError())
                        return Result<void>::Failure(captured.ErrorValue());
                    assets_ = std::move(captured).Value();
                }
                loads_ = std::make_unique<Assets::AssetLoadService>(jobs, assets_ ? assets_->Provider() : emptyProvider_);
                return Result<void>::Success();
            }

            /** @brief Captures the selected project schema before any candidate/native-world staging. */
            Result<Physics::PhysicsSceneActivationSettings> CaptureSettings(const std::filesystem::path &root,
                                                                            const Physics::PhysicsRuntimeMode mode) {
                auto physics = Physics::PhysicsWorldSettings::Capture({});
                if (physics.HasError())
                    return Result<Physics::PhysicsSceneActivationSettings>::Failure(physics.ErrorValue());
                auto character = Character::CharacterWorldSettings::Capture({});
                if (character.HasError())
                    return Result<Physics::PhysicsSceneActivationSettings>::Failure(character.ErrorValue());
                Physics::PhysicsWorldSimulationBinding simulation;
                if (mode == Physics::PhysicsRuntimeMode::Canonical) {
                    const auto metadata = LoadProjectMetadata(root);
                    if (metadata.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(metadata.ErrorValue());
                    auto collision = LoadProjectCollisionSettings(root, metadata.Value().projectId);
                    if (collision.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(collision.ErrorValue());
                    collision_ = std::move(collision).Value().settings;
                    auto captured = collision_->Capture(authority_.Capture().collisionFilterGeneration);
                    if (captured.HasError())
                        return Result<Physics::PhysicsSceneActivationSettings>::Failure(captured.ErrorValue());
                    simulation = std::move(captured).Value();
                    capturedFilterGeneration_ = simulation.generation;
                }
                return Result<Physics::PhysicsSceneActivationSettings>::Success(
                    {std::move(physics).Value(), std::move(character).Value(), std::move(simulation)});
            }

            const std::thread::id ownerLane_{std::this_thread::get_id()};
            Assets::AssetRegistry registry_;
            std::unique_ptr<CommittedSceneAssets> assets_;
            Assets::MemoryAssetProvider emptyProvider_;
            std::unique_ptr<Assets::AssetLoadService> loads_;
            std::shared_ptr<const ProjectCollisionSettings> collision_;
            std::unique_ptr<Physics::PhysicsRuntime> physics_;
            Physics::PhysicsSceneActivationAuthority authority_;
            std::uint64_t capturedFilterGeneration_{};
            ScenePhysicsHost native_;
            std::unique_ptr<Runtime::RuntimeSceneService> scenes_;
            Runtime::SceneRuntimeId identity_;
        };
    }  // namespace

    /** @copydoc PrepareEditorProjectPreview */
    Result<EditorProjectPreviewScene> PrepareEditorProjectPreview(const std::filesystem::path &root, JobSystem &jobs,
                                                                  const EditorProjectScenePolicy &policy) {
        auto owner = std::make_unique<ProjectPlaySceneHost>(Runtime::SceneRuntimeId{1},
                                                            ScenePhysicsIdentityRange{{1}, {Editor::EditorPlaySceneNamespace - 1}});
        if (auto prepared = owner->Prepare(root, jobs, policy, nullptr); prepared.HasError())
            return Result<EditorProjectPreviewScene>::Failure(prepared.ErrorValue());
        const auto mode = owner->PhysicsMode();
        auto scenes = owner->ReleaseScenes();
        return Result<EditorProjectPreviewScene>::Success({std::move(owner), std::move(scenes), mode});
    }

    /** @copydoc ValidateEditorPreviewProjectScope */
    Result<void> ValidateEditorPreviewProjectScope(const std::filesystem::path &startupRoot, const std::filesystem::path &incomingRoot,
                                                   const Physics::PhysicsRuntimeMode mode) {
        if (mode == Physics::PhysicsRuntimeMode::Null)
            return Result<void>::Success();
        std::error_code error;
        if (const bool same = std::filesystem::equivalent(startupRoot, incomingRoot, error); error || !same)
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Canonical Physics preview owns its startup project's captures. "
                                                                        "Restart the editor with --project for the selected project."));
        return Result<void>::Success();
    }

    /** @copydoc MakeEditorProjectSceneFactory */
    Editor::EditorProjectPlaySceneFactory MakeEditorProjectSceneFactory(JobSystem &jobs, EditorProjectScenePolicy policy) {
        return [&jobs, policy = std::move(policy)](const std::filesystem::path &root, const Runtime::SceneRuntimeId identity,
                                                   const Runtime::RuntimeSceneDefinition &definition) {
            if (identity.value < Editor::EditorFirstPlayScene.value)
                return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Failure(MakeError(InvalidProjectScene));
            auto host =
                std::make_unique<ProjectPlaySceneHost>(identity, ScenePhysicsIdentityRange{Editor::EditorFirstPlayScene,
                                                                                           {std::numeric_limits<std::uint64_t>::max()}});
            if (auto prepared = host->Prepare(root, jobs, policy, &definition); prepared.HasError())
                return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Failure(prepared.ErrorValue());
            return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Success(std::move(host));
        };
    }
}  // namespace Horo::Application::Internal
