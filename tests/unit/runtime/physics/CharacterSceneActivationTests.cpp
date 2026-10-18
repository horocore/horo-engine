#include "CharacterScenePlacement.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PhysicsSceneActivationTestSupport.h"
#include "PhysicsSceneHostAccess.h"
#include "PhysicsSceneWorldTestAccess.h"
#include "ScenePhysicsHost.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <thread>
#include <vector>

namespace Horo::Physics {
    namespace {
        Runtime::CharacterControllerComponent SceneCharacter(Assets::AssetId material) {
            Runtime::CharacterControllerComponent value{.id = {7}, .enabled = true};
            value.collision.profile = Profile();
            value.collision.queryChannel = Settings().simulation.schema->QueryChannels().front().id;
            value.collision.defaultMaterial = Runtime::PhysicsColliderMaterialBinding{PhysicsMaterialSlotId::FromValue(1), material};
            return value;
        }

        SceneSource::SceneSourceDocument CharacterSource(Assets::AssetId material, bool floor = true) {
            SceneSource::SceneSourceDocument source;
            source.objects.push_back(
                {.id = {10}, .name = "Visual parent", .localTransform = {.translation = {2, 0, 0}, .scale = {2, 2, 2}}});
            source.objects.push_back({.id = {1},
                                      .parent = SceneSource::SceneObjectId{10},
                                      .name = "Controller",
                                      .localTransform = {.translation = {0, 0.495F, 0},
                                                         .rotation = {0, 0.70710677F, 0, 0.70710677F},
                                                         .scale = {0.5F, 0.5F, 0.5F}},
                                      .components = {.characterController = SceneCharacter(material)}});
            source.objects.push_back(
                {.id = {2}, .parent = SceneSource::SceneObjectId{1}, .name = "Visual child", .localTransform = {.translation = {0, 1, 0}}});
            if (floor)
                source.objects.push_back({.id = {3},
                                          .name = "Floor",
                                          .components = {.rigidBody = Runtime::RigidBodyComponent{.id = {30}, .body = {300}},
                                                         .colliders = {Collider(31, 301, 3, 300, material,
                                                                                Runtime::PhysicsAnalyticCollider{
                                                                                    Runtime::PhysicsStaticPlaneCollider{{0, 1, 0}, 0}})}}});
            return source;
        }

        Runtime::RuntimeSceneDefinition CookCharacter(const SceneSource::SceneSourceDocument &source, std::uint64_t revision = 1) {
            auto converted = SceneSource::ConvertSceneSourceToRuntime({source.objects, source.prefabInstances}, {19}, {revision});
            REQUIRE(converted.HasValue());
            auto cooked = SceneCook::EncodeCookedSceneDefinition(converted.Value());
            REQUIRE(cooked.HasValue());
            auto decoded = SceneCook::DecodeCookedSceneDefinition(cooked.Value(), {19}, {revision});
            REQUIRE(decoded.HasValue());
            CHECK(decoded.Value().Entities()[1].components.characterController == source.objects[1].components.characterController);
            return std::move(decoded).Value();
        }

        void InstallCharacterMaterial(AssetSceneFixture &assets) {
            auto material = PhysicsMaterialAsset::Create({.id = PhysicsMaterialAssetId::FromAssetId(assets.material), .sourceRevision = 1});
            REQUIRE(material.HasValue());
            auto encoded = EncodePhysicsMaterialAsset(material.Value());
            REQUIRE(encoded.HasValue());
            std::vector<std::uint8_t> bytes;
            for (auto byte : encoded.Value())
                bytes.push_back(std::to_integer<std::uint8_t>(byte));
            assets.provider.Insert(assets.material, std::move(bytes));
        }

        TEST_CASE("Null Physics explicitly refuses enabled Character while disabled data survives cook", "[physics][scene][character]") {
            auto runtime = RequireRuntime();
            PhysicsSceneActivationAuthority authority;
            AssetSceneFixture assets;
            InstallCharacterMaterial(assets);
            auto source = CharacterSource(assets.material, false);
            const auto enabled = CookCharacter(source);
            const auto scene = assets.Prepare(enabled);
            PhysicsSceneActivationParticipant participant{*runtime, authority, Settings()};
            Test::RequireError(participant.Prepare(enabled, scene), PhysicsErrors::CapabilityUnavailable);
            source.objects[1].components.characterController->enabled = false;
            source.objects[1].components.characterController->collision = {};
            const auto disabled = CookCharacter(source, 2);
            auto disabledScene = RequireScene(disabled);
            auto prepared = participant.Prepare(disabled, disabledScene->View());
            REQUIRE(prepared.HasValue());
            CHECK(prepared.Value()->InitialPlacements().empty());
            CHECK(disabled.Entities()[1].components.characterController == source.objects[1].components.characterController);
            prepared.Value()->Shutdown();
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Character preparation requires declared project selectors before native publication",
                  "[physics][scene][character][bindings][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            PhysicsSceneActivationAuthority authority;
            AssetSceneFixture assets;
            InstallCharacterMaterial(assets);
            const auto settings = Settings();
            auto source = CharacterSource(assets.material, false);
            auto &collision = source.objects[1].components.characterController->collision;
            collision.requiredLayer = settings.simulation.schema->Layers().front().id;
            collision.requiredProfile = Profile();
            std::array<std::uint8_t, 16> unknown{};
            unknown.back() = 99;
            bool admitted = true;
            SECTION("Declared layer and profile are retained") {
                // Keep the valid baseline; preparation is asserted after the sections.
            }
            SECTION("An undeclared required layer fails closed") {
                collision.requiredLayer = CollisionLayerId::FromBytes(unknown);
                admitted = false;
            }
            SECTION("An undeclared required profile fails closed") {
                collision.requiredProfile = CollisionProfileId::FromBytes(unknown);
                admitted = false;
            }
            const auto definition = CookCharacter(source);
            const auto scene = assets.Prepare(definition);
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, settings};
            auto candidate = participant.Prepare(definition, scene);
            if (admitted) {
                REQUIRE(candidate.HasValue());
                CHECK(candidate.Value()->InitialPlacements().size() == 1);
                candidate.Value()->Shutdown();
            } else {
                REQUIRE(candidate.HasError());
                CHECK_FALSE(candidate.ErrorValue().message.empty());
            }
        }

        /** @brief Observes the real aggregate-owned Physics candidate without adding a second activation owner. */
        class CharacterOwnerObserver final : public Runtime::SceneActivationParticipant {
        public:
            CharacterOwnerObserver(PhysicsRuntime &runtime, PhysicsSceneActivationAuthority &authority)
                : owner_(runtime, authority, Settings()) {}

            Runtime::SceneActivationPhase PreparationPhase() const noexcept override {
                return owner_.PreparationPhase();
            }

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                               Runtime::RuntimeSceneView scene) override {
                auto prepared = owner_.Prepare(definition, scene);
                if (prepared.HasValue())
                    candidate = dynamic_cast<PhysicsSceneActivationCandidate *>(prepared.Value().get());
                return prepared;
            }

            PhysicsSceneActivationCandidate *candidate{};

        private:
            PhysicsSceneActivationParticipant owner_;
        };

        Error RequireReplacementRejected(AssetSceneFixture &assets, Runtime::RuntimeSceneDefinition definition) {
            REQUIRE(assets.service.QueuePreparation(std::move(definition)).HasValue());
            for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
                REQUIRE(assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(assets.cancellation.Token()))
                            .HasValue());
                if (auto error = assets.service.TakeOperationError())
                    return std::move(*error);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            FAIL("Timed out waiting for Character replacement rejection.");
            return MakeError(PhysicsErrors::InvalidState);
        }

        void RequireProjectedPose(Runtime::RuntimeSceneView active, const PhysicsSceneActivationCandidate &candidate,
                                  const Character::CharacterWorld &character, const SceneSource::SceneSourceDocument &authored,
                                  const Runtime::RuntimeSceneDefinition &definition) {
            REQUIRE(character.ActiveControllerCount() == 1);
            REQUIRE(candidate.CharacterBindings().size() == 1);
            const auto placement = character.ControllerTransform(candidate.CharacterBindings().front().handle).Value();
            const auto view = active.Get(*active.Find({1}));
            REQUIRE(view.HasValue());
            CHECK(view.Value().localTransform->translation.y > authored.objects[1].localTransform.translation.y);
            CHECK(std::abs(view.Value().localTransform->translation.y * 2 - placement.position.y) < 1e-5F);
            CHECK(view.Value().localTransform->scale == authored.objects[1].localTransform.scale);
            CHECK(definition.Entities()[1].localTransform == authored.objects[1].localTransform);
        }

        void RequireNativeDescendantRejected(AssetSceneFixture &assets, const SceneSource::SceneSourceDocument &authored,
                                             Runtime::SceneRuntimeId oldScene, const Character::CharacterCapability &retained) {
            auto invalid = authored;
            invalid.objects.back().parent = SceneSource::SceneObjectId{1};
            const auto rejected = RequireReplacementRejected(assets, CookCharacter(invalid, 2));
            CHECK(rejected.code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
            REQUIRE(assets.service.ActiveScene().has_value());
            CHECK(assets.service.ActiveScene()->RuntimeId() == oldScene);
            CHECK(retained.Validate().HasValue());
        }

        TEST_CASE("Aggregate Character activation projects recovered parented pose without dirtying authored Scene",
                  "[physics][scene][character][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            PhysicsSceneActivationAuthority authority;
            AssetSceneFixture assets{false, {71}};
            InstallCharacterMaterial(assets);
            auto observer = std::make_unique<CharacterOwnerObserver>(*runtime.Value(), authority);
            auto *owner = observer.get();
            REQUIRE(assets.service.AddActivationParticipant(std::move(observer)).HasValue());
            REQUIRE(assets.service.Startup(assets.cancellation.Token()).HasValue());
            const auto authored = CharacterSource(assets.material);
            const auto definition = CookCharacter(authored);
            auto active = assets.Prepare(definition);
            REQUIRE(owner->candidate != nullptr);
            auto &character = PhysicsSceneContainmentTestAccess::CharacterWorld(*owner->candidate);
            RequireProjectedPose(active, *owner->candidate, character, authored, definition);
            CHECK(active.RuntimeId() == Runtime::SceneRuntimeId{71});
            CHECK(character.Descriptor().sceneGeneration == active.RuntimeId().value);
            const auto scope = Detail::PhysicsSceneHostAccess::Borrow(*owner->candidate);
            REQUIRE(scope.HasValue());
            CHECK(scope.Value().scene == active.RuntimeId());
            CHECK(&scope.Value().character == &character);
            const auto handle = owner->candidate->CharacterBindings().front().handle;
            auto retained = character.IssueCapability().Value();
            RequireNativeDescendantRejected(assets, authored, active.RuntimeId(), retained);
            REQUIRE(assets.service.QueueUnload().HasValue());
            REQUIRE(assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(assets.cancellation.Token()))
                        .HasValue());
            CHECK_FALSE(assets.service.ActiveScene().has_value());
            CHECK(retained.Validate().HasError());
            CHECK(retained.ControllerTransform(handle).HasError());
            REQUIRE(assets.service.QueueUnload().HasValue());
            REQUIRE(assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(assets.cancellation.Token()))
                        .HasValue());
            CHECK(authored.objects[1].localTransform.translation.y == 0.495F);
        }

        void RegisterHostPhysics(Application::Internal::ScenePhysicsHost &host, AssetSceneFixture &assets, PhysicsRuntime &runtime,
                                 PhysicsSceneActivationAuthority &authority) {
            CHECK(host.MakeStructuralParticipant().HasError());
            auto participant = host.MakeParticipant(runtime, authority, Settings());
            REQUIRE(participant.HasValue());
            REQUIRE(assets.service.AddActivationParticipant(std::move(participant).Value()).HasValue());
            CHECK(host.MakeParticipant(runtime, authority, Settings()).HasError());
            auto structural = host.MakeStructuralParticipant();
            REQUIRE(structural.HasValue());
            REQUIRE(assets.service.AddStructuralParticipant(std::move(structural).Value()).HasValue());
            REQUIRE(assets.service.Startup(assets.cancellation.Token()).HasValue());
        }

        void RequireForeignHostBorrowRejected(Application::Internal::ScenePhysicsHost &host) {
            std::optional<Result<Detail::PhysicsSceneHostScope>> foreign;
            std::thread worker{[&] {
                foreign.emplace(host.Borrow());
            }};
            worker.join();
            REQUIRE(foreign.has_value());
            Test::RequireError(*foreign, PhysicsErrors::ThreadAffinityViolation);
        }

        /** @brief Advances the actual host once, preserving phase context and typed failure diagnostics. */
        void RequireHostTick(Application::Internal::ScenePhysicsHost &host, std::string_view phase) {
            INFO(phase);
            const auto tick = host.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667));
            INFO((tick.HasError() ? tick.ErrorValue().code.Value() + ": " + tick.ErrorValue().message : "tick succeeded"));
            REQUIRE(tick.HasValue());
        }

        TEST_CASE("Application observer follows the sole Scene-owned pair across failed replacement and retirement",
                  "[physics][scene][character][host][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            PhysicsSceneActivationAuthority authority;
            Application::Internal::ScenePhysicsHost host;
            AssetSceneFixture assets{false, {101}};
            InstallCharacterMaterial(assets);
            RegisterHostPhysics(host, assets, *runtime.Value(), authority);
            CHECK(host.Borrow().HasError());
            const auto source = CharacterSource(assets.material);
            const auto first = assets.Prepare(CookCharacter(source));
            REQUIRE(host.Borrow().HasValue());
            CHECK(host.Borrow().Value().scene == first.RuntimeId());
            CHECK(host.Borrow().Value().character.ActiveControllerCount() == 1);
            CHECK(host.Borrow().Value().physics.PublishedTick().completedTick == 0);
            CHECK(host.Borrow().Value().character.PublishedTick().completedTick == 0);
            const auto preparedRevision = host.Borrow().Value().character.Descriptor().physicsSnapshotRevision;
            RequireHostTick(host, "first tick");
            CHECK(host.Borrow().Value().character.Descriptor().physicsSnapshotRevision == preparedRevision);
            RequireHostTick(host, "second tick");
            CHECK(host.Borrow().Value().physics.PublishedTick().completedTick == 2);
            CHECK(host.Borrow().Value().character.PublishedTick().completedTick == 2);
            auto retained = host.Borrow().Value().character.IssueCapability().Value();
            RequireNativeDescendantRejected(assets, source, first.RuntimeId(), retained);
            CHECK(host.Borrow().Value().scene == first.RuntimeId());
            const auto replacement = assets.Prepare(CookCharacter(source, 2));
            CHECK(replacement.RuntimeId() == Runtime::SceneRuntimeId{102});
            REQUIRE(host.Borrow().HasValue());
            CHECK(host.Borrow().Value().scene == replacement.RuntimeId());
            CHECK(host.Borrow().Value().character.ActiveControllerCount() == 1);
            CHECK(retained.Validate().HasError());
            auto replacementGrant = host.Borrow().Value().character.IssueCapability().Value();
            CHECK(host.Borrow().Value().physics.PublishedTick().completedTick == 0);
            RequireHostTick(host, "replacement tick");
            RequireForeignHostBorrowRejected(host);
            REQUIRE(assets.service.QueueUnload().HasValue());
            REQUIRE(assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(assets.cancellation.Token()))
                        .HasValue());
            CHECK(host.Borrow().HasError());
            CHECK(replacementGrant.Validate().HasError());
            CHECK(host.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)).HasError());
            assets.service.Shutdown();
            CHECK(host.Borrow().HasError());
        }

        TEST_CASE("Application reserved Scene range rejects before native participant preparation", "[physics][scene][host][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            PhysicsSceneActivationAuthority authority;
            Application::Internal::ScenePhysicsHost host{{{200}, {200}}};
            AssetSceneFixture assets{false, {201}};
            RegisterHostPhysics(host, assets, *runtime.Value(), authority);
            const auto rejected = assets.service.QueuePreparation(Definition());
            Test::RequireError(rejected, PhysicsErrors::InvalidState);
            CHECK_FALSE(assets.service.ActiveScene());
            CHECK(host.Borrow().HasError());
            assets.service.Shutdown();
        }

        TEST_CASE("Cooked Character recovers against real initial colliders before the first Physics tick",
                  "[physics][scene][character][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            AssetSceneFixture assets;
            InstallCharacterMaterial(assets);
            const auto source = CharacterSource(assets.material);
            const auto definition = CookCharacter(source);
            const auto view = assets.Prepare(definition);
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime.Value(), authority, Settings()};
            auto prepared = participant.Prepare(definition, view);
            REQUIRE(prepared.HasValue());
            auto &candidate = dynamic_cast<PhysicsSceneActivationCandidate &>(*prepared.Value());
            auto &physics = PhysicsSceneContainmentTestAccess::World(candidate);
            auto &character = PhysicsSceneContainmentTestAccess::CharacterWorld(candidate);
            REQUIRE(candidate.CharacterBindings().size() == 1);
            REQUIRE(candidate.InitialPlacements().size() == 1);
            CHECK(Detail::PhysicsSceneHostAccess::Borrow(candidate).HasError());
            candidate.Publish();
            auto scope = Detail::PhysicsSceneHostAccess::Borrow(candidate);
            REQUIRE(scope.HasValue());
            CHECK(&scope.Value().physics == &physics);
            CHECK(&scope.Value().character == &character);
            CHECK(scope.Value().scene == view.RuntimeId());
            const auto handle = candidate.CharacterBindings()[0].handle;
            const auto pose = character.ControllerTransform(handle);
            REQUIRE(pose.HasValue());
            CHECK(pose.Value().position.x == 2.0F);
            CHECK(pose.Value().position.y > 0.99F);
            CHECK(std::abs(pose.Value().heading.y - 0.70710677F) < 1e-5F);
            CHECK(pose.Value().sourceTick == 0);
            CHECK(physics.PublishedTick().completedTick == 0);
            const auto body = candidate.FindBody({3}, {300});
            REQUIRE(body.has_value());
            CHECK(physics.ReadSceneBodyReconciliation(*body).Value().state.pose.translation == Math::Vec3{});
            auto retained = character.IssueCapability();
            REQUIRE(retained.HasValue());
            candidate.Shutdown();
            CHECK(Detail::PhysicsSceneHostAccess::Borrow(candidate).HasError());
            CHECK(retained.Value().Validate().HasError());
            CHECK(retained.Value().ControllerTransform(handle).HasError());
            CHECK(candidate.CharacterBindings().empty());
            CHECK(candidate.InitialPlacements().empty());
            candidate.Shutdown();
        }

        TEST_CASE("Initial Character query admission rejects stale rebind wrong thread and completed tick",
                  "[physics][scene][character][canonical]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
            REQUIRE(runtime.HasValue());
            auto settings = Settings();
            auto physics = runtime.Value()->PrepareWorld(settings.physics, settings.simulation);
            REQUIRE(physics.HasValue());
            auto identity = runtime.Value()->IssueWorldIdentity();
            REQUIRE(identity.HasValue());
            REQUIRE(physics.Value()->Activate(identity.Value()).HasValue());
            auto character = Character::CharacterWorld::Prepare({7, identity.Value(), 1, 1, 1}, settings.character);
            REQUIRE(character.HasValue());
            using Placement = Character::Detail::CharacterScenePlacement;
            Test::RequireError(Placement::BindInitialQueries(*physics.Value(), *character.Value(), {8}), PhysicsErrors::QuerySnapshotStale);
            std::optional<Result<void>> foreign;
            std::thread thread{[&] {
                foreign.emplace(Placement::BindInitialQueries(*physics.Value(), *character.Value(), {7}));
            }};
            thread.join();
            REQUIRE(foreign.has_value());
            Test::RequireError(*foreign, PhysicsErrors::ThreadAffinityViolation);
            REQUIRE(Placement::BindInitialQueries(*physics.Value(), *character.Value(), {7}).HasValue());
            Test::RequireError(Placement::BindInitialQueries(*physics.Value(), *character.Value(), {7}), PhysicsErrors::InvalidState);
            REQUIRE(physics.Value()->AdvanceFixedTick({1, 7, Duration::FromNanoseconds(16'666'667)}).HasValue());
            Test::RequireError(Placement::BindInitialQueries(*physics.Value(), *character.Value(), {7}), PhysicsErrors::InvalidState);
            character.Value()->Shutdown();
            physics.Value()->Shutdown();
        }
#endif
    }  // namespace
}  // namespace Horo::Physics
