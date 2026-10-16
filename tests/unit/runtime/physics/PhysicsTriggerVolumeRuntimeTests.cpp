#include "Horo/Application/PhysicsTriggerVolumeHost.h"
#include "Horo/Scene/CookedSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "PhysicsSceneActivationTestSupport.h"
#include "PhysicsSceneWorldTestAccess.h"

namespace Horo::Physics {
    namespace {
        SceneSource::SceneSourceDocument TriggerSource(const Runtime::ColliderShapeType shape = Runtime::ColliderShapeType::Box,
                                                       const bool enabled = true, const float parentX = 2.0F) {
            SceneSource::SceneSourceDocument document;
            document.objects.push_back({.id = {10}, .localTransform = {.translation = {parentX, 0, 0}}});
            document.objects.push_back({.id = {1},
                                        .parent = SceneSource::SceneObjectId{10},
                                        .components = {.triggerVolume = Runtime::TriggerVolumeComponent{shape, enabled}}});
            if (!enabled)
                return document;
            Runtime::RigidBodyComponent moving{.id = {11},
                                               .body = {100},
                                               .motion = Runtime::AuthoredPhysicsMotionType::Dynamic,
                                               .mass = Runtime::AuthoredPhysicsMass{1.0F},
                                               .initialLinearVelocity = {2.0F, 0, 0}};
            document.objects.push_back(
                {.id = {2},
                 .localTransform = {.translation = {parentX, 0, 0}},
                 .components = {.rigidBody = moving,
                                .colliders = {Collider(22, 200, 2, 100, Asset(91),
                                                       Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{0.1F}})}}});
            return document;
        }

        Runtime::RuntimeSceneDefinition TriggerDefinition(const SceneSource::SceneSourceDocument &source,
                                                          const SceneSource::TriggerVolumeConversionContext &binding,
                                                          const std::uint64_t revision = 1) {
            auto converted = SceneSource::ConvertSceneSourceToRuntime({source.objects, source.prefabInstances}, {19}, {revision}, binding);
            REQUIRE(converted.HasValue());
            auto encoded = SceneCook::EncodeCookedSceneDefinition(converted.Value());
            REQUIRE(encoded.HasValue());
            auto decoded = SceneCook::DecodeCookedSceneDefinition(encoded.Value(), {19}, {revision});
            REQUIRE(decoded.HasValue());
            REQUIRE(decoded.Value().TriggerVolumeEvidence().size() == (source.objects[1].components.triggerVolume->enabled ? 1 : 0));
            return std::move(decoded).Value();
        }

        SceneSource::TriggerVolumeConversionContext ProjectBinding(const PhysicsSceneActivationSettings &settings,
                                                                   AssetSceneFixture &assets) {
            const auto material =
                PhysicsMaterialAsset::Create({.id = PhysicsMaterialAssetId::FromAssetId(assets.material), .sourceRevision = 1});
            REQUIRE(material.HasValue());
            const auto encoded = EncodePhysicsMaterialAsset(material.Value()).Value();
            const std::span bytes{reinterpret_cast<const std::uint8_t *>(encoded.data()), encoded.size()};
            assets.provider.Insert(assets.material, {bytes.begin(), bytes.end()});
            Assets::AssetCookArtifact artifact{.id = assets.material,
                                               .type = assets.materialType,
                                               .target = Assets::AssetCookTargetId::Parse("headless-test").Value(),
                                               .sourceDigest = ComputeSha256(std::as_bytes(std::span{bytes})),
                                               .payloadDigest = ComputeSha256(std::as_bytes(std::span{bytes})),
                                               .payload = {bytes.begin(), bytes.end()}};
            auto capture = Application::CaptureProjectTriggerVolumeBinding(settings.simulation, Profile(), material.Value(), artifact);
            REQUIRE(capture.HasValue());
            return std::move(capture).Value();
        }

#if HORO_TEST_PHYSICS_NATIVE
        TEST_CASE("Authored trigger cooked scene activates native enter and exit without physical response",
                  "[physics][trigger][native][cook][events]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
            AssetSceneFixture assets;
            auto settings = Settings();
            // The fixture selects a real Project overlap row; the binding never substitutes a default.
            const auto layer = settings.simulation.schema->Layers().front().id;
            const auto channel = settings.simulation.schema->QueryChannels().front().id;
            ProjectCollisionSchema authored{.defaultProfile = Profile(),
                                            .layers = {{layer}},
                                            .pairs = {{layer, layer, SimulationPairResponse::Overlap}},
                                            .queryChannels = {{channel}},
                                            .profiles = {
                                                {Profile(), layer, true, true, true, {{channel, CollisionQueryResponse::Overlap}}}}};
            settings.simulation.schema =
                std::make_shared<const NormalizedCollisionSchema>(NormalizedCollisionSchema::Create(authored).Value());
            const auto binding = ProjectBinding(settings, assets);
            const auto definition = TriggerDefinition(TriggerSource(), binding);
            const auto view = assets.Prepare(definition);
            PhysicsSceneActivationAuthority authority;
            PhysicsSceneActivationParticipant participant{*runtime, authority, settings};
            auto prepared = participant.Prepare(definition, view);
            REQUIRE(prepared.HasValue());
            auto &candidate = dynamic_cast<PhysicsSceneActivationCandidate &>(*prepared.Value());
            auto &world = PhysicsSceneContainmentTestAccess::World(candidate);
            const auto moving = candidate.FindBody({2}, {100});
            REQUIRE(moving.has_value());
            auto capability = world.IssueQueryEventCapability().Value();
            std::size_t enters{}, exits{};
            for (std::uint64_t tick = 1; tick <= 90; ++tick) {
                REQUIRE(world
                            .AdvanceFixedTick({.simulationTick = tick,
                                               .sceneGeneration = view.RuntimeId().value,
                                               .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                            .HasValue());
                const auto publication = world.PublishedTick();
                std::array<PhysicsEventRecord, 8> events{};
                const auto read = capability.ReadEvents({.identity = capability.Identity(),
                                                         .completedTick = publication.eventTick,
                                                         .publicationRevision = publication.publicationRevision,
                                                         .maximumRecords = events.size()},
                                                        events);
                REQUIRE(read.HasValue());
                for (std::size_t index = 0; index < read.Value().recordCount; ++index) {
                    enters += events[index].kind == PhysicsEventKind::TriggerEnter;
                    exits += events[index].kind == PhysicsEventKind::TriggerExit;
                    REQUIRE((events[index].kind == PhysicsEventKind::TriggerEnter || events[index].kind == PhysicsEventKind::TriggerExit));
                }
            }
            REQUIRE(enters == 1);
            REQUIRE(exits == 1);
            const auto state = world.ReadSceneBodyReconciliation(*moving);
            REQUIRE(state.HasValue());
            REQUIRE(state.Value().state.pose.translation.x > 4.0F);
            candidate.Shutdown();
            REQUIRE(candidate.BodyBindings().empty());
            REQUIRE(world.ReadSceneBodyReconciliation(*moving).HasError());
        }

        TEST_CASE("Trigger binding mismatch rejects native allocation and play replacement preserves the old world",
                  "[physics][trigger][native][lifecycle]") {
            auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
            AssetSceneFixture assets;
            const auto settings = Settings();
            auto binding = ProjectBinding(settings, assets);
            auto source = TriggerSource();
            const auto first = TriggerDefinition(source, binding);
            const auto initialView = assets.Prepare(first);
            PhysicsPlayWorldSession play{*runtime, settings};
            REQUIRE(play.Prepare(first, initialView).HasValue());
            REQUIRE(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, initialView).HasValue());
            const auto oldWorld = play.WorldIdentity();
            const auto oldBody = play.ResolveBody({1}, {1}).Value();
            auto wrong = binding;
            wrong.binding.materialPayloadDigest.bytes.front() ^= 1;
            const auto rejected = TriggerDefinition(source, wrong, 2);
            const auto rejectedView = assets.Prepare(rejected);
            Test::RequireError(play.Prepare(rejected, rejectedView), PhysicsErrors::MaterialDescriptorInvalid);
            REQUIRE(play.WorldIdentity() == oldWorld);
            REQUIRE(play.ValidateBody(oldBody).HasValue());
            {
                // Same material identity/revision, different valid coefficients and self-consistent bytes.
                AssetSceneFixture differentAssets;
                auto descriptor =
                    PhysicsMaterialAsset::Create({.id = PhysicsMaterialAssetId::FromAssetId(differentAssets.material), .sourceRevision = 1})
                        .Value()
                        .Descriptor();
                descriptor.friction = 0.75F;
                const auto changedMaterial = PhysicsMaterialAsset::Create(descriptor).Value();
                const auto changedBytes = EncodePhysicsMaterialAsset(changedMaterial).Value();
                const auto payload = std::span{reinterpret_cast<const std::uint8_t *>(changedBytes.data()), changedBytes.size()};
                differentAssets.provider.Insert(differentAssets.material, {payload.begin(), payload.end()});
                const auto expected = TriggerDefinition(source, binding, 3);
                const auto differentView = differentAssets.Prepare(expected);
                Test::RequireError(play.Prepare(expected, differentView), PhysicsErrors::MaterialDescriptorInvalid);
                REQUIRE(play.WorldIdentity() == oldWorld);
                REQUIRE(play.ValidateBody(oldBody).HasValue());
            }
            for (const auto shape : {Runtime::ColliderShapeType::Sphere, Runtime::ColliderShapeType::Capsule}) {
                source = TriggerSource(shape, true, 3.0F);
                const auto replacement = TriggerDefinition(source, binding, 3 + static_cast<std::uint64_t>(shape));
                const auto replacementView = assets.Prepare(replacement);
                REQUIRE(play.Prepare(replacement, replacementView).HasValue());
                REQUIRE(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, replacementView).HasValue());
                REQUIRE(play.ValidateBody(oldBody).HasError());
            }
            source = TriggerSource(Runtime::ColliderShapeType::Box, false);
            const auto disabled = TriggerDefinition(source, binding, 8);
            const auto disabledView = assets.Prepare(disabled);
            REQUIRE(play.Prepare(disabled, disabledView).HasValue());
            REQUIRE(play.Commit(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, disabledView).HasValue());
            REQUIRE(play.ResolveBody({1}, {1}).HasError());
            bool rejectedThread{};
            std::thread foreign([&] {
                rejectedThread = play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasError();
            });
            foreign.join();
            REQUIRE(rejectedThread);
            REQUIRE(play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            REQUIRE(play.Stop(Runtime::RuntimePhase::CommitDeferredLifecycleChanges).HasValue());
            REQUIRE_FALSE(play.IsActive());
        }
#endif
    }  // namespace
}  // namespace Horo::Physics
