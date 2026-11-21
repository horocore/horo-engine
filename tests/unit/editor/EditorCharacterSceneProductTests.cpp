#include "CharacterSceneProductFixture.h"
#include "EditorProjectSceneHost.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "navigation/ProjectSceneNavigationFixture.h"

#if HORO_TEST_PHYSICS_NATIVE
#include "HeadlessProjectScene.h"
#include "Horo/Network/NetworkErrors.h"
#include "NetworkModeHostTestSupport.h"
#include "OwnedTestDirectory.h"

#include <catch2/catch_approx.hpp>
#include <fstream>
#include <functional>
#endif

#include <chrono>
#include <thread>

namespace Horo::Application::CharacterSceneProductTest {
    namespace {
        /** @brief Preserves the original typed product-start failure before requiring genuine admission. */
        void RequirePlayStarted(Result<void> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Editor Play started"));
            REQUIRE(result.HasValue());
        }

        /** @brief Pumps only actual asynchronous Scene lifecycle progress, never a Physics/gameplay fixed tick. */
        void FinishPlayPreparation(Editor::EditorPlaySessionController &play) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (play.State() == Editor::EditorPlaySessionState::Starting && std::chrono::steady_clock::now() < deadline) {
                play.PresentationUpdate({0});
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (play.LastError())
                UNSCOPED_INFO(play.LastError()->message);
        }

        Editor::EditorPlaySceneFactory BindProjectFactory(Fixture &fixture, Physics::PhysicsRuntimeMode mode) {
            auto factory =
                Internal::MakeEditorProjectSceneFactory(fixture.project.jobs,
                                                        Internal::EditorProjectScenePolicy{.physicsMode = mode,
                                                                                           .cooked = Internal::
                                                                                               SceneCookSelection{fixture.project.cooked
                                                                                                                      .path,
                                                                                                                  fixture.project.request
                                                                                                                      .assets.target}});
            return
                [factory = std::move(factory), root = fixture.project.project.dir.path](Runtime::SceneRuntimeId identity,
                                                                                        const Runtime::RuntimeSceneDefinition &definition) {
                return factory(root, identity, definition);
            };
        }

    }  // namespace

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        constexpr auto ParityActor = Runtime::SceneObjectId{3};
        constexpr std::size_t ParityTicks = 4;
        constexpr auto ParityStep = Duration::FromNanoseconds(16'666'667);

        struct AuthoritySample final {
            float characterHeight;
            float gameplayPosition;
        };

        /** @brief Runs the graphical host's Play callbacks using actual scheduler-issued attempt evidence. */
        class EditorAuthorityParticipant final : public Runtime::RuntimeLifecycleParticipant {
        public:
            explicit EditorAuthorityParticipant(Editor::EditorPlaySessionController &play) : play_(play) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &) override {
                if (phase == Runtime::RuntimePhase::VariableUpdate)
                    play_.PresentationUpdate({0});
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
                auto result = play_.OnRuntimeFixedUpdate({}, context);
                if (result.HasError() || !afterFixedUpdate)
                    return result;
                return afterFixedUpdate();
            }

            std::function<Result<void>()> afterFixedUpdate;

            Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
                return play_.PrepareRuntimeFixedCommit(context);
            }

            void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                play_.CommitRuntimeFixedUpdate(context);
            }

            void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                play_.AbortRuntimeFixedUpdate(context);
            }

            void Shutdown() noexcept override {
                play_.Stop();
            }

        private:
            Editor::EditorPlaySessionController &play_;
        };

        /** @brief Reads committed outputs of canonical Character recovery and the gameplay module. */
        AuthoritySample CaptureAuthority(const Runtime::RuntimeSceneView &scene) {
            const auto read = [&](Runtime::SceneObjectId object) {
                const auto entity = scene.Find(object);
                REQUIRE(entity);
                const auto value = scene.Get(*entity);
                REQUIRE(value.HasValue());
                return value.Value().localTransform->translation;
            };
            return {read({1}).y, read(ParityActor).x};
        }

        /** @brief Authors and cooks a Scene that exposes each actual fixed gameplay callback as a committed transform. */
        Internal::GameplayWorldSelection AuthorParityScene(Fixture &fixture, const std::filesystem::path &directory) {
            constexpr std::string_view behaviorName = "game.tests.authority_parity";
            Internal::GameplayWorldSelection gameplay{.moduleId = "game.tests",
                                                      .scriptSource = directory / "parity.horo_script",
                                                      .scriptSidecar = directory / "parity.horo_script.meta"};
            std::ofstream{gameplay.scriptSource} << R"(
local step
return horo.behavior {
    display_name = 'Authority parity',
    on_create = function() step = 10 end,
    on_fixed_update = function(ctx)
        step = step + 1
        ctx.transform.set_position(step, 0, 0)
    end
})";
            std::ofstream{gameplay.scriptSidecar} << R"({"schemaVersion":1,"runtime":"lua","behaviorTypeId":")" << behaviorName << R"("})";
            const auto program = Gameplay::LuaBehaviorProgram::LoadFiles(gameplay.scriptSource, gameplay.scriptSidecar);
            REQUIRE(program.HasValue());
            Gameplay::ComponentRegistry components;
            REQUIRE(components.Freeze().HasValue());
            const auto schema = PrefabCookSchemaContext::Capture(components, std::span{&program.Value()->Descriptor(), 1});
            REQUIRE(schema.HasValue());
            fixture.project.request.schemas = schema.Value();
            fixture.source.objects.back().components.behaviors.push_back({Gameplay::BehaviorInstanceId{1},
                                                                          program.Value()->Descriptor().typeId,
                                                                          program.Value()->Descriptor().schemaVersion,
                                                                          true,
                                                                          {}});
            fixture.project.WriteText("assets/level.horo", SceneSource::EncodeSceneSource({fixture.source.objects, {}}));
            fixture.Cook();
            return gameplay;
        }

        /** @brief Runs the real configured Editor Play services over the authored project and its committed cook generation. */
        std::array<AuthoritySample, ParityTicks + 1> CaptureEditorAuthority(Fixture &fixture,
                                                                            const Internal::GameplayWorldSelection &gameplay) {
            auto program = Gameplay::LuaBehaviorProgram::LoadFiles(gameplay.scriptSource, gameplay.scriptSidecar);
            REQUIRE(program.HasValue());
            Gameplay::BehaviorRegistry registry;
            REQUIRE(registry.Register(program.Value()->Registration()).HasValue());
            REQUIRE(registry.Freeze().HasValue());
            Editor::EditorPlaySessionController play;
            DeterministicClock clock;
            auto runtime = Runtime::RuntimeHost::Create(clock);
            REQUIRE(runtime.HasValue());
            REQUIRE(play.BindRuntimeDispatch(runtime.Value()->DispatchSource()).HasValue());
            REQUIRE(runtime.Value()->AddParticipant(std::make_unique<EditorAuthorityParticipant>(play)).HasValue());
            REQUIRE(runtime.Value()->Startup().HasValue());
            REQUIRE(runtime.Value()->RunFrame().HasValue());
            RequirePlayStarted(play.Start({{1}, {1}, fixture.source.objects}, registry,
                                          BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)));
            FinishPlayPreparation(play);
            REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
            std::array<AuthoritySample, ParityTicks + 1> samples;
            REQUIRE(play.SceneView());
            samples.front() = CaptureAuthority(*play.SceneView());
            for (auto &sample : std::span{samples}.subspan(1)) {
                clock.Advance(ParityStep);
                RequirePlayStarted(runtime.Value()->RunFrame());
                REQUIRE(play.SceneView());
                sample = CaptureAuthority(*play.SceneView());
            }
            runtime.Value()->Shutdown();
            return samples;
        }

        /** @brief Uses the product host's actual dedicated scheduler and rejects any accidental presentation construction. */
        std::array<AuthoritySample, ParityTicks + 1> CaptureDedicatedAuthority(const Fixture &fixture,
                                                                               const Internal::GameplayWorldSelection &gameplay) {
            using namespace Network;
            auto prepared =
                Internal::PrepareHeadlessProjectSceneResources({.projectRoot = fixture.project.project.dir.path,
                                                                .scene = "cooked:" + fixture.project.sceneId.ToString(),
                                                                .cookedRoot = fixture.project.cooked.path,
                                                                .cookTarget = std::string{fixture.project.request.assets.target.Value()},
                                                                .physicsMode = Physics::PhysicsRuntimeMode::Canonical});
            if (prepared.HasError())
                UNSCOPED_INFO(prepared.ErrorValue().message);
            REQUIRE(prepared.HasValue());
            auto resources = std::move(prepared).Value();
            const auto owners = std::make_shared<ProductOwners>();
            auto factories = ProductFactories(owners);
            for (const auto kind : {NetworkModeServiceKind::Audio, NetworkModeServiceKind::Renderer, NetworkModeServiceKind::Gui,
                                    NetworkModeServiceKind::Input, NetworkModeServiceKind::LocalPlayer})
                factories.services[static_cast<std::size_t>(kind)] = [](const NetworkModeServiceRequest &) {
                    FAIL_CHECK("Dedicated host attempted to construct a presentation service");
                    return Result<std::unique_ptr<INetworkModeService>>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
                };
            auto input = DefaultNetworkProjectSettings(NetworkProjectSettingsId::Create(10).Value()).Value();
            input.supportedRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::DedicatedServer;
            input.transport.requirement = NetworkProjectTransportRequirement::Required;
            input.transport.capabilities.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] = true;
            input.transport.capabilities.requiredChannels = 2;
            input.transport.capabilities.requiredMaximumMessageBytes = 1024;
            input.replication.completeness = NetworkReplicationInventoryCompleteness::Complete;
            const auto project = NetworkProjectSettings::Create(input);
            if (project.HasError())
                UNSCOPED_INFO(project.ErrorValue().message);
            REQUIRE(project.HasValue());
            const auto mode = NetworkProjectRole::DedicatedServer;
            const auto facts = ProductFacts(project.Value(), mode, false);
            auto composition =
                Internal::ComposeHeadlessSceneNetworkServices(resources->definition,
                                                              {Physics::PhysicsRuntimeMode::Canonical, *resources->settings,
                                                               resources->registry, *resources->loads, resources->load},
                                                              std::move(factories), gameplay,
                                                              Gameplay::GameplayReplicationAdmissionPolicy{.inventory = input.replication});
            REQUIRE(composition.HasValue());
            auto configured = std::move(composition).Value();
            const INetworkModeService *scene{};
            auto &sceneFactory = configured.factories.services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)];
            sceneFactory = [create = std::move(sceneFactory), &scene](const NetworkModeServiceRequest &request) {
                auto service = create(request);
                if (service.HasValue())
                    scene = service.Value().get();
                return service;
            };
            const std::array worlds{World(NetworkModeWorldKind::AuthorityServer, 31)};
            DeterministicClock clock;
            auto created = Internal::NetworkProductHost::Create(clock,
                                                                {project.Value(),
                                                                 facts.product,
                                                                 facts.inventory,
                                                                 facts.host,
                                                                 facts.requirements,
                                                                 facts.selection,
                                                                 6,
                                                                 worlds,
                                                                 {},
                                                                 owners->sessions,
                                                                 NetworkWorldCompositionPurpose::Gameplay,
                                                                 owners->worldSetup,
                                                                 {},
                                                                 std::move(configured.pollRetirement)},
                                                                std::move(configured.factories));
            REQUIRE(created.HasValue());
            auto host = std::move(created).Value();
            RequirePlayStarted(host->Startup());
            REQUIRE(host->RunFrame().HasValue());
            REQUIRE(host->Role(worlds.front().kind, worlds.front().scene).HasValue());
            REQUIRE(scene);
            const auto capture = [&] {
                const auto active = Internal::InspectHeadlessScene(*scene);
                REQUIRE(active);
                return CaptureAuthority(*active);
            };
            std::array<AuthoritySample, ParityTicks + 1> samples;
            samples.front() = capture();
            for (auto &sample : std::span{samples}.subspan(1)) {
                clock.Advance(ParityStep);
                RequirePlayStarted(host->RunFrame());
                sample = capture();
            }
            host->Shutdown();
            CHECK(host->Role(worlds.front().kind, worlds.front().scene).HasError());
            return samples;
        }
    }  // namespace

    TEST_CASE("Dedicated simulation matches configured Editor Play for cooked Character and fixed Lua gameplay",
              "[character][scene][editor][product][canonical][dedicated]") {
        Fixture fixture{1};
        const Tests::OwnedTestDirectory scripts{"authority parity"};
        const auto gameplay = AuthorParityScene(fixture, scripts.Path());
        const auto editor = CaptureEditorAuthority(fixture, gameplay);
        const auto dedicated = CaptureDedicatedAuthority(fixture, gameplay);
        REQUIRE(editor.front().characterHeight > fixture.source.objects.front().localTransform.translation.y);
        for (std::size_t tick = 0; tick < editor.size(); ++tick) {
            CAPTURE(tick);
            CHECK(editor[tick].gameplayPosition == (tick == 0 ? 0.0F : static_cast<float>(10 + tick)));
            CHECK(dedicated[tick].gameplayPosition == editor[tick].gameplayPosition);
            CHECK(dedicated[tick].characterHeight == Catch::Approx(editor[tick].characterHeight).margin(0.00001));
        }
    }

    TEST_CASE("XR Play resolves and stages a native teleport for the real scheduler without Animation",
              "[character][scene][editor][product][canonical][xr]") {
        bool failAfterStaging{};
        SECTION("scheduler commits the prepared native tick") {}
        SECTION("scheduler aborts before native Physics runs") {
            failAfterStaging = true;
        }

        Fixture fixture;
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        DeterministicClock clock;
        auto runtime = Runtime::RuntimeHost::Create(clock);
        REQUIRE(runtime.HasValue());
        REQUIRE(play.BindRuntimeDispatch(runtime.Value()->DispatchSource()).HasValue());

        Internal::ScenePhysicsHost *native{};
        Character::CharacterControllerHandle controller;
        Math::Vec3 originalPosition;
        Math::Vec3 destination;
        std::optional<std::uint64_t> committed;
        std::optional<std::pair<std::uint64_t, std::uint64_t>> aborted;
        std::optional<Math::Vec3> abortedPosition;
        Internal::EditorProjectScenePolicy policy{.physicsMode = Physics::PhysicsRuntimeMode::Canonical,
                                                  .cooked = Internal::SceneCookSelection{fixture.project.cooked.path,
                                                                                         fixture.project.request.assets.target}};
        policy.xrLocomotion = [&](Internal::ScenePhysicsHost &host, const Runtime::RuntimeSceneView &scene,
                                  const Runtime::FixedStepContext &step) {
            native = &host;
            const auto actor = scene.Find(Runtime::SceneObjectId{1});
            REQUIRE(actor);
            const auto resolved = host.ResolveCharacterController(scene, *actor);
            REQUIRE(resolved.HasValue());
            controller = resolved.Value();
            const auto scope = host.Borrow();
            REQUIRE(scope.HasValue());
            const auto transform = scope.Value().character.ControllerTransform(controller);
            REQUIRE(transform.HasValue());
            originalPosition = transform.Value().position;

            const auto ray = Math::TryMakeRay({2, 3, 0}, {0, -1, 0}, 0, 5);
            REQUIRE(ray.HasValue());
            const auto target = host.QueryTeleportTarget(scene.RuntimeId(), controller, ray.Value());
            REQUIRE(target.HasValue());
            REQUIRE(target.Value());
            destination = *target.Value();
            CHECK(destination.x == Catch::Approx(2.0F));
            CHECK(destination.y > 0.0F);
            const auto shortRay = Math::TryMakeRay({2, 3, 0}, {0, -1, 0}, 0, 1);
            REQUIRE(shortRay.HasValue());
            const auto missing = host.QueryTeleportTarget(scene.RuntimeId(), controller, shortRay.Value());
            REQUIRE(missing.HasValue());
            CHECK_FALSE(missing.Value());

            return host.QueueTeleport(scene.RuntimeId(), step,
                                      {.controller = controller,
                                       .tick = 1,
                                       .targetPosition = destination,
                                       .targetHeading = transform.Value().heading},
                                      {});
        };
        policy.commitXrLocomotion = [&](Internal::ScenePhysicsHost &host, const Runtime::FixedStepContext &) noexcept {
            if (auto scope = host.Borrow(); scope.HasValue())
                committed = scope.Value().character.PublishedTick().completedTick;
        };
        policy.cancelXrLocomotion = [&]() noexcept {
            if (!native)
                return;
            if (auto scope = native->Borrow(); scope.HasValue()) {
                aborted.emplace(scope.Value().character.PublishedTick().completedTick, scope.Value().physics.PublishedTick().completedTick);
                if (const auto transform = scope.Value().character.ControllerTransform(controller); transform.HasValue())
                    abortedPosition = transform.Value().position;
            }
        };
        auto actual = Internal::MakeEditorProjectSceneFactory(fixture.project.jobs, std::move(policy));
        const auto factory = [&](Runtime::SceneRuntimeId identity, const Runtime::RuntimeSceneDefinition &definition) {
            return actual(fixture.project.project.dir.path, identity, definition);
        };
        auto participant = std::make_unique<EditorAuthorityParticipant>(play);
        bool observedStaging{};
        participant->afterFixedUpdate = [&] {
            REQUIRE(native);
            const auto scope = native->Borrow();
            REQUIRE(scope.HasValue());
            CHECK(scope.Value().character.PublishedTick().completedTick == 0);
            CHECK(scope.Value().physics.PublishedTick().completedTick == 0);
            CHECK_FALSE(committed);
            const auto transform = scope.Value().character.ControllerTransform(controller);
            REQUIRE(transform.HasValue());
            CHECK(transform.Value().position == originalPosition);
            observedStaging = true;
            return failAfterStaging ? Result<void>::Failure(MakeError(Physics::PhysicsErrors::InvalidState)) : Result<void>::Success();
        };
        REQUIRE(runtime.Value()->AddParticipant(std::move(participant)).HasValue());
        REQUIRE(runtime.Value()->Startup().HasValue());
        REQUIRE(runtime.Value()->RunFrame().HasValue());
        RequirePlayStarted(play.Start({{1}, {1}, fixture.source.objects}, registry, factory));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);

        clock.Advance(ParityStep);
        const auto frame = runtime.Value()->RunFrame();
        CHECK(observedStaging);
        if (failAfterStaging) {
            REQUIRE(frame.HasError());
            CHECK_FALSE(committed);
            REQUIRE(aborted);
            CHECK(*aborted == std::pair<std::uint64_t, std::uint64_t>{0, 0});
            CHECK(abortedPosition == originalPosition);
            return;
        }
        RequirePlayStarted(frame);
        CHECK(committed == 1);
        CHECK_FALSE(aborted);
        REQUIRE(native);
        const auto scope = native->Borrow();
        REQUIRE(scope.HasValue());
        CHECK(scope.Value().physics.PublishedTick().completedTick == 1);
        const auto transform = scope.Value().character.ControllerTransform(controller);
        REQUIRE(transform.HasValue());
        CHECK(transform.Value().position == destination);
        runtime.Value()->Shutdown();
    }

    TEST_CASE("Actual configured Editor Play recovers Character before readiness and keeps authored root unchanged",
              "[character][scene][editor][product][canonical]") {
        Fixture fixture;
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        const Editor::SceneDocumentSnapshot authored{{1}, {1}, fixture.source.objects};
        RequirePlayStarted(play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        REQUIRE(play.SceneView());
        const auto first = play.SceneView()->RuntimeId();
        const auto receipt = play.CollisionReceipt();
        REQUIRE(receipt);
        CHECK(receipt->state == Editor::EditorScenePhysicsState::Active);
        CHECK(receipt->projectId == "p1");
        CHECK(receipt->documentRevision != 0);
        CHECK(receipt->fingerprint != 0);
        CHECK(receipt->generation == 1);
        CHECK(receipt->generation != first.value);  // Collision filter authority is not a Scene runtime identity.

        const auto entity = play.SceneView()->Find({1});
        REQUIRE(entity);
        CHECK(play.SceneView()->Get(*entity).Value().localTransform->translation.y > 0.495F);
        CHECK(authored.objects.front().localTransform.translation.y == 0.495F);
        CHECK(play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)).HasError());
        const auto firstTick = play.FixedUpdate({}, {1.0 / 60.0});
        INFO(
            (firstTick.HasError() ? firstTick.ErrorValue().code.Value() + ": " + firstTick.ErrorValue().message : "Editor tick succeeded"));
        REQUIRE(firstTick.HasValue());
        play.Stop();
        CHECK_FALSE(play.SceneView());
        CHECK_FALSE(play.CollisionReceipt());
        RequirePlayStarted(play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        CHECK(play.SceneView()->RuntimeId().value > first.value);
        play.Stop();
        CHECK(authored.objects.front().localTransform.translation.y == 0.495F);
    }

#endif

    TEST_CASE("Explicit Null Editor Scene host refuses enabled Character without fallback", "[character][scene][editor][product][null]") {
        Fixture fixture;
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        const Editor::SceneDocumentSnapshot authored{{1}, {1}, fixture.source.objects};
        const auto started = play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Null));
        FinishPlayPreparation(play);
        CHECK((started.HasError() || play.State() == Editor::EditorPlaySessionState::Failed));
        REQUIRE(play.LastError());
        CHECK(play.LastError()->code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value());
        CHECK_FALSE(play.SceneView());
        CHECK(authored.objects.front().localTransform.translation.y == 0.495F);
    }

    TEST_CASE("Editor Play admits exact cooked scoped Navigation alongside actual Scene startup and retirement",
              "[navigation][scene][editor][product]") {
        NavigationProductFixture fixture;
        using Input = NavigationProductFixture::Input;
        Input input = Input::Valid;
        bool modifyDocument = false;
        std::optional<Assets::AssetId> selected = fixture.project.sceneId;
        bool expected = HORO_TEST_NAVIGATION_NATIVE != 0;
        SECTION("a matching document plays with the admitted provider") {}
        SECTION("a foreign scoped artifact has no local world") {
            input = Input::ForeignScope;
            expected = false;
        }
        SECTION("a stale cooked project profile fails the aggregate") {
            input = Input::StalePolicy;
            expected = false;
        }
        SECTION("a missing surface cannot activate as an empty world") {
            input = Input::MissingSurface;
            expected = false;
        }
        SECTION("an uncommitted document change cannot admit old topology") {
            modifyDocument = true;
            expected = false;
        }
        SECTION("the selected asset must be a committed Scene") {
            selected = fixture.definitionId;
            expected = false;
        }
        fixture.Publish(input);
        Editor::SceneDocumentSnapshot authored{{3}, {1}, fixture.source.objects};
        if (modifyDocument)
            authored.objects.front().localTransform.translation.x = 5;
        const Internal::EditorProjectScenePolicy policy{.cooked = Internal::SceneCookSelection{fixture.project.cooked.path,
                                                                                               fixture.project.request.assets.target},
                                                        .playNavigationScene = selected};
        auto projectFactory = Internal::MakeEditorProjectSceneFactory(fixture.project.jobs, policy);
        const Editor::EditorPlaySceneFactory factory = [&projectFactory, &fixture](const Runtime::SceneRuntimeId identity,
                                                                                   const Runtime::RuntimeSceneDefinition &definition) {
            return projectFactory(fixture.project.project.dir.path, identity, definition);
        };
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        const auto started = play.Start(authored, registry, factory);
        FinishPlayPreparation(play);
        if (!expected) {
            CHECK((started.HasError() || play.State() == Editor::EditorPlaySessionState::Failed));
            REQUIRE(play.LastError());
            if (modifyDocument)
                CHECK(play.LastError()->code.Value() == Navigation::NavigationErrors::StaleSnapshot.code.Value());
            CHECK_FALSE(play.SceneView());
            return;
        }
        RequirePlayStarted(started);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        REQUIRE(play.SceneView());
        CHECK(play.SceneView()->Find({3}).has_value());
        const auto first = play.SceneView()->RuntimeId();
        play.Stop();
        CHECK_FALSE(play.SceneView());
        RequirePlayStarted(play.Start(authored, registry, factory));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        REQUIRE(play.SceneView());
        CHECK(play.SceneView()->RuntimeId().value > first.value);
        play.Stop();
        CHECK_FALSE(play.SceneView());
    }

    TEST_CASE("Canonical preview project scope cannot reuse another project's immutable capture", "[character][scene][editor][scope]") {
        Fixture first;
        Fixture second;
        CHECK(Internal::ValidateEditorPreviewProjectScope(first.project.project.dir.path, first.project.project.dir.path,
                                                          Physics::PhysicsRuntimeMode::Canonical)
                  .HasValue());
        CHECK(Internal::ValidateEditorPreviewProjectScope(first.project.project.dir.path, second.project.project.dir.path,
                                                          Physics::PhysicsRuntimeMode::Canonical)
                  .HasError());
        CHECK(Internal::ValidateEditorPreviewProjectScope(first.project.project.dir.path, second.project.project.dir.path,
                                                          Physics::PhysicsRuntimeMode::Null)
                  .HasValue());
    }
}  // namespace Horo::Application::CharacterSceneProductTest
