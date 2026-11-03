#include "AllocationProbe.h"
#include "AnimationGraphInstanceTestFixtures.h"
#include "EditorPlaySaveHostCapture.h"
#include "EditorProjectSceneHost.h"
#include "Horo/Animation/AnimationAssetSource.h"
#include "Horo/Animation/AnimationGraphSource.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "PrefabSceneCookHostFixture.h"
#include "editor/gameplay/EditorPlaySessionController.h"

#include <catch2/catch_approx.hpp>
#include <chrono>
#include <thread>

namespace Horo::Application::EditorAnimationProductTest {
    namespace {
        using namespace PrefabCookTestSupport;
        namespace A = Animation;

        template <typename T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Builds inert authoring values from genuine compiled fixture dependencies. */
        Editor::SceneDocumentSnapshot Document(const A::Test::Instance::Prepared &source, bool directClip) {
            A::AnimationAuthoringComponent binding;
            binding.component = A::Test::Id<A::AnimationComponentId>(7);
            binding.skeleton = source.skeleton->Data().skeleton;
            binding.source.kind = directClip ? A::AnimationSourceKind::Clip : A::AnimationSourceKind::Graph;
            if (directClip)
                binding.source.clip = source.compileClips.front().Data().descriptor.id;
            else
                binding.source.graph = source.program->Id();
            return {{1},
                    {1},
                    {{.id = {1}, .name = "Camera", .components = {.camera = Runtime::CameraComponent{}}},
                     {.id = {2}, .name = "Animated", .components = {.animation = binding}}}};
        }

        /** @brief Uses the actual scanner-supported source extension and sidecar identity consumed by the app factory. */
        AssetRecord WriteAsset(HostFixture &project, std::string_view name, Assets::AssetId id, std::string_view type, std::string bytes) {
            const auto path = "assets/" + std::string(name) + ".horoasset";
            project.WriteText(path, bytes);
            project.WriteText(path + ".horo", SidecarJson(id.ToString(), type));
            return {id, Type(type), Take(ProjectPath::Parse(path)), Take(ProjectPath::Parse(path + ".horo"))};
        }

        /** @brief Publishes the real authored sources through the ordinary project AST/cook transaction. */
        void CookProject(HostFixture &project, const A::Test::Instance::Prepared &source, const Editor::SceneDocumentSnapshot &document) {
            std::filesystem::rename(project.project.dir.path / "assets/level.scene", project.project.dir.path / "assets/level.horo");
            std::filesystem::rename(project.project.dir.path / "assets/level.scene.horo",
                                    project.project.dir.path / "assets/level.horo.horo");
            auto records =
                std::vector<AssetRecord>(project.registry.Snapshot().Records().begin(), project.registry.Snapshot().Records().end());
            for (auto &record : records) {
                if (record.id == project.sceneId) {
                    record.sourcePath = Take(ProjectPath::Parse("assets/level.horo"));
                    record.metadataPath = Take(ProjectPath::Parse("assets/level.horo.horo"));
                }
            }
            records.push_back(WriteAsset(project, "skeleton", source.skeleton->Data().skeleton.Asset(), "core.animation.skeleton",
                                         Take(A::SerializeSkeletonAnimationSource(*source.skeleton))));
            records.push_back(WriteAsset(project, "clip", source.compileClips.front().Data().descriptor.id.Asset(), "core.animation.clip",
                                         Take(A::SerializeClipAnimationSource(source.compileClips.front()))));
            records.push_back(WriteAsset(project, "graph", source.program->Id().Asset(), "core.animation.graph",
                                         Take(A::SerializeAnimationGraphSource(A::Test::Graph::Simple()))));
            project.WriteText("assets/level.horo", SceneSource::EncodeSceneSource({document.objects, {}}));
            REQUIRE(project.registry.Publish(std::move(records)).status == AssetRegistryBuildStatus::Complete);
            REQUIRE(project.Cook().HasValue());
        }

        /** @brief Forwards the actual app/controller callbacks; measurements cover only the owner-thread fixed path. */
        class PlayParticipant final : public Runtime::RuntimeLifecycleParticipant {
        public:
            explicit PlayParticipant(Editor::EditorPlaySessionController &play) : play_(play) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &) override {
                if (phase == Runtime::RuntimePhase::VariableUpdate)
                    play_.PresentationUpdate({0});
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
                Tests::AllocationProbe::ScopedMeasurement measured;
                auto result = play_.OnRuntimeFixedUpdate({}, context);
                fixedAllocations += measured.Snapshot().requests;
                return result;
            }

            Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
                Tests::AllocationProbe::ScopedMeasurement measured;
                auto result = play_.PrepareRuntimeFixedCommit(context);
                fixedAllocations += measured.Snapshot().requests;
                return result;
            }

            void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                Tests::AllocationProbe::ScopedMeasurement measured;
                play_.CommitRuntimeFixedUpdate(context);
                fixedAllocations += measured.Snapshot().requests;
            }

            void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                play_.AbortRuntimeFixedUpdate(context);
                if (onAbort)
                    onAbort();
            }

            void Shutdown() noexcept override {
                play_.Stop();
            }

            std::size_t fixedAllocations{};
            std::function<void()> onAbort;

        private:
            Editor::EditorPlaySessionController &play_;
        };

        /** @brief Injects one genuine later-participant rejection before the scheduler's commitment fence. */
        class LaterGate final : public Runtime::RuntimeLifecycleParticipant {
        public:
            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &) override {
                if (fail)
                    return Result<void>::Failure(Error{ErrorCode{"test.editor.animation.later_failure"}});
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            bool fail{};
        };

        /** @brief Owns a real cooked project and actual runtime; no standalone adapter replaces the production Scene factory. */
        struct Fixture final {
            HostFixture project;
            A::Test::Instance::Prepared source;
            Editor::SceneDocumentSnapshot authoring;
            Gameplay::BehaviorRegistry registry;
            DeterministicClock clock;
            Editor::EditorPlaySessionController play;
            std::unique_ptr<Runtime::RuntimeHost> runtime;
            std::shared_ptr<Internal::AnimationRuntimeIdentitySource> identities;
            Internal::EditorProjectScenePolicy policy;
            Editor::EditorPlaySceneHost *owner{};
            PlayParticipant *forward{};
            LaterGate *gate{};

            explicit Fixture(bool directClip = false, Internal::AnimationRuntimeIdentityRange range = {701, 704})
                : authoring(Document(source, directClip)), identities(std::make_shared<Internal::AnimationRuntimeIdentitySource>(range)) {
                CookProject(project, source, authoring);
                REQUIRE(registry.Freeze().HasValue());
                runtime = Take(Runtime::RuntimeHost::Create(clock, {.fixedStep = Duration::FromNanoseconds(10)}));
                policy.cooked = Internal::SceneCookSelection{project.cooked.path, project.request.assets.target};
                policy.animation =
                    Internal::SceneAnimationHostSelection{runtime->DispatchSource(), project.request.assets.target, identities};
                REQUIRE(play.BindRuntimeDispatch(runtime->DispatchSource()).HasValue());
                auto participant = std::make_unique<PlayParticipant>(play);
                forward = participant.get();
                REQUIRE(runtime->AddParticipant(std::move(participant)).HasValue());
                auto later = std::make_unique<LaterGate>();
                gate = later.get();
                REQUIRE(runtime->AddParticipant(std::move(later)).HasValue());
                REQUIRE(runtime->Startup().HasValue());
                REQUIRE(runtime->RunFrame().HasValue());
            }

            ~Fixture() {
                runtime->Shutdown();
            }

            void Start() {
                auto factory = Internal::MakeEditorProjectSceneFactory(project.jobs, policy);
                const Editor::EditorPlaySceneFactory selected{
                    [this, factory = std::move(factory)](Runtime::SceneRuntimeId id, const Runtime::RuntimeSceneDefinition &definition) {
                    auto made = factory(project.project.dir.path, id, definition);
                    if (made.HasValue())
                        owner = made.Value().get();
                    return made;
                }};
                REQUIRE(play.Start(authoring, registry, selected).HasValue());
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (play.State() == Editor::EditorPlaySessionState::Starting && std::chrono::steady_clock::now() < deadline) {
                    play.PresentationUpdate({0});
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                INFO((play.LastError() ? play.LastError()->code.Value() + ": " + play.LastError()->message : "ready"));
                REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
                REQUIRE(owner->AnimationRuntime());
            }

            Result<void> Tick() {
                clock.Advance(Duration::FromNanoseconds(10));
                return runtime->RunFrame();
            }

            Runtime::EntityRef Entity() const {
                const auto view = play.SceneView();
                REQUIRE(view);
                const auto entity = view->Find(Runtime::SceneObjectId{2});
                REQUIRE(entity);
                return *entity;
            }

            Runtime::SceneAnimationObservation Observation() const {
                return Take(owner->AnimationRuntime()->Query(Entity()));
            }

            std::optional<float> TryPoseX() const {
                const auto view = play.SceneView();
                if (!view || !owner)
                    return {};
                const auto entity = view->Find(Runtime::SceneObjectId{2});
                if (!entity)
                    return {};
                auto observed = owner->AnimationRuntime()->Query(*entity);
                if (observed.HasError() || !observed.Value().component)
                    return {};
                std::array<Math::Transform, 1> pose;
                if (owner->AnimationRuntime()->CopyPose(*entity, observed.Value().component->currentPose, pose).HasError())
                    return {};
                return pose[0].translation.x;
            }

            float PoseX() const {
                const auto value = TryPoseX();
                REQUIRE(value);
                return *value;
            }
        };
    }  // namespace

    TEST_CASE("Actual Editor animated Graph and Clip factories use one runtime and preserve pause step resume",
              "[editor][animation][product]") {
        for (const bool clip : {false, true}) {
            Fixture fixture(clip);
            fixture.Start();
            REQUIRE(fixture.Tick().HasValue());
            CHECK(fixture.PoseX() == 10.0F);
            const auto first = fixture.Observation();
            REQUIRE(first.component);
            CHECK(first.component->previousPose == first.component->currentPose);
            REQUIRE(fixture.play.Pause().HasValue());
            REQUIRE(fixture.Tick().HasValue());
            CHECK(fixture.PoseX() == 10.0F);
            REQUIRE(fixture.play.Step().HasValue());
            REQUIRE(fixture.Tick().HasValue());
            CHECK(fixture.PoseX() == 20.0F);
            REQUIRE(fixture.Tick().HasValue());
            CHECK(fixture.PoseX() == 20.0F);
            REQUIRE(fixture.play.Resume().HasValue());
            REQUIRE(fixture.Tick().HasValue());
            CHECK(fixture.PoseX() == Catch::Approx(30.0F).epsilon(0).margin(0.00001F));
            CHECK(fixture.forward->fixedAllocations == 0);
        }
    }

    TEST_CASE("Actual Editor Animation abort preserves the prior pose when a later actual runtime participant fails",
              "[editor][animation][product][abort]") {
        Fixture fixture;
        fixture.Start();
        REQUIRE(fixture.Tick().HasValue());
        std::optional<float> abortedPose;
        fixture.forward->onAbort = [&] {
            abortedPose = fixture.TryPoseX();
        };
        fixture.gate->fail = true;
        const auto failed = fixture.Tick();
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == "test.editor.animation.later_failure");
        REQUIRE(abortedPose);
        CHECK(*abortedPose == 10.0F);
        CHECK(fixture.play.State() == Editor::EditorPlaySessionState::Idle);
        CHECK(fixture.runtime->RunFrame().HasError());
    }

    TEST_CASE("Preview Play and prefab factories share the explicit Animation identity source without reuse",
              "[editor][animation][product][identity]") {
        Fixture fixture;
        auto preview = Take(Internal::PrepareEditorProjectPreview(fixture.project.project.dir.path, fixture.project.jobs, fixture.policy));
        REQUIRE(preview.owners->AnimationRuntime());
        fixture.Start();
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.Observation().instance.owner == A::Test::Id<A::AnimationRuntimeId>(702));
        const auto definition = Take(SceneSource::ConvertSceneSourceToRuntime({fixture.authoring.objects, {}}, {88}, {1}));
        const auto stageFactory = Internal::MakeEditorProjectPrefabStageFactory(fixture.project.jobs, fixture.policy);
        auto stage = Take(stageFactory.create(fixture.project.project.dir.path, Editor::EditorFirstPrefabStage, definition));
        REQUIRE(stage->AnimationRuntime());
        fixture.play.Stop();
        fixture.owner = nullptr;
        fixture.Start();
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.Observation().instance.owner == A::Test::Id<A::AnimationRuntimeId>(704));
        const auto factory = Internal::MakeEditorProjectSceneFactory(fixture.project.jobs, fixture.policy);
        const auto exhausted = factory(fixture.project.project.dir.path, Editor::EditorFirstPlayScene, definition);
        CHECK(exhausted.HasError());
    }

    TEST_CASE("Actual animated Play capture refuses missing live Animation state without changing the running pose",
              "[editor][animation][product][save]") {
        Fixture fixture;
        fixture.Start();
        REQUIRE(fixture.Tick().HasValue());
        const auto view = fixture.play.SceneView();
        REQUIRE(view);
        const auto baseline = Take(
            SceneSource::ConvertSceneSourceToRuntime({fixture.authoring.objects, {}}, view->DefinitionId(), view->DefinitionRevision()));
        const auto captured = Internal::CapturePlaySaveScene(*view, baseline, {});
        REQUIRE(captured.HasError());
        CHECK(captured.ErrorValue().code.Value() == Runtime::SaveErrors::RestoreParticipantInvalid.code.Value());
        CHECK(fixture.play.State() == Editor::EditorPlaySessionState::Playing);
        CHECK(fixture.PoseX() == 10.0F);
        REQUIRE(fixture.Tick().HasValue());
        CHECK(fixture.PoseX() == 20.0F);
    }
}  // namespace Horo::Application::EditorAnimationProductTest
