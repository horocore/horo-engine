#pragma once
/** @file PhysicsSceneStateTestSupport.h
 * @brief Genuine private Scene candidate/paired-tick proof, not aggregate Save publication proof.
 */
#include "EditorPlaySavePhysicsContent.h"
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"
#include "PhysicsSceneActivationTestSupport.h"
#include "PhysicsSceneStateTarget.h"
#include "PhysicsStateWorldTestSupport.h"
#include "ScenePhysicsHost.h"

#include <algorithm>
#include <array>
#include <vector>

namespace Horo::Physics::StateTest {
    inline Detail::SceneStateStoragePolicy SceneStoragePolicy() {
        return {{128, 2U * 1024U * 1024U}, 3, 256U * 1024U * 1024U};
    }

    /** @brief Same logical definition with deliberate physical/entity registration order variation. */
    inline Runtime::RuntimeSceneDefinition ContinuationSceneDefinition(Assets::AssetId material, Assets::AssetTypeId type, bool reverse) {
        const auto original = AnalyticVariantDefinition(material, type);
        auto entities = std::vector<Runtime::RuntimeEntityDefinition>(original.Entities().begin(), original.Entities().end());
        if (reverse)
            std::ranges::reverse(entities);
        Runtime::SceneDefinitionBuilder builder{original.Id(), original.Revision()};
        for (auto &entity : entities) {
            if (entity.components.rigidBody && entity.components.rigidBody->motion == Runtime::AuthoredPhysicsMotionType::Dynamic) {
                entity.components.rigidBody->initialLinearVelocity = {0.75F, 0.125F, -0.375F};
                entity.components.rigidBody->initialAngularVelocity = {0.125F, 0.25F, -0.0625F};
            }
            // Keep the fixed joint aligned at admission rather than pulling the dynamic sphere away from the floor.
            if (entity.object.value == 1)
                for (auto &constraint : entity.components.physicsConstraints)
                    std::get<Runtime::PhysicsConstraintBodyEndpoint>(constraint.second).localFrame.translation.y = -2.0F;
            if (entity.object.value == 3)
                entity.localTransform.translation.y = -0.59F;  // Real shallow sphere/plane contact remains after settling.
            builder.Add(std::move(entity));
        }
        for (const auto &asset : original.AssetDependencies())
            Check(builder.RequireAsset(asset));
        return Take(std::move(builder).Build());
    }

    inline PhysicsSceneActivationSettings ContinuationSceneSettings(std::uint64_t filterEpoch = 1) {
        auto settings = Settings();
        std::array<std::uint8_t, 16> layerBytes{};
        layerBytes.back() = 2;
        std::array<std::uint8_t, 16> channelBytes{};
        channelBytes.back() = 3;
        const auto layer = CollisionLayerId::FromBytes(layerBytes);
        const auto channel = PhysicsQueryChannelId::FromBytes(channelBytes);
        ProjectCollisionSchema authored{.defaultProfile = Profile(),
                                        .layers = {{layer}},
                                        .pairs = {{layer, layer, SimulationPairResponse::Block}},
                                        .queryChannels = {{channel}},
                                        .profiles = {{Profile(), layer, true, true, true, {{channel, CollisionQueryResponse::Block}}}}};
        settings.simulation = {std::make_shared<const NormalizedCollisionSchema>(Take(NormalizedCollisionSchema::Create(authored))),
                               filterEpoch};
        return settings;
    }

    /** @brief Real privately owned Physics participant over an actual asset-ready core Scene. */
    struct SceneOwnerFixture final {
        PhysicsSceneActivationAuthority authority;
        Application::Internal::ScenePhysicsHost host;
        AssetSceneFixture assets;
        Runtime::RuntimeSceneDefinition definition;
        Runtime::RuntimeSceneView view;
        std::unique_ptr<Runtime::SceneActivationParticipant> participant;
        std::unique_ptr<Runtime::SceneStructuralParticipant> structural;
        std::unique_ptr<Runtime::SceneActivationCandidate> candidate;
        Runtime::PersistentEntityIdentityMap identities;
        std::shared_ptr<const Application::Internal::PlaySavePhysicsContent> content;

        SceneOwnerFixture(PhysicsRuntime &runtime, std::uint64_t scene, std::uint64_t filterEpoch = 1, bool reverse = false)
            : assets(false, {scene}), definition(ContinuationSceneDefinition(assets.material, assets.materialType, reverse)),
              identities(InitialIdentities(scene)) {
            Check(assets.service.Startup(assets.cancellation.Token()));
            view = assets.Prepare(definition);
            REQUIRE(filterEpoch > 0);
            for (std::uint64_t epoch = 1; epoch < filterEpoch; ++epoch)
                Check(authority.AdvanceCollisionFilterGeneration());
            auto settings = ContinuationSceneSettings(filterEpoch);
            participant = Take(host.MakeParticipant(runtime, authority, std::move(settings)));
            structural = Take(host.MakeStructuralParticipant());
            candidate = Take(participant->Prepare(definition, view));
            identities = MapIdentities();
            content = Take(Application::Internal::PlaySavePhysicsContent::Capture(view, {64, 64, 2U * 1024U * 1024U}));
        }

        ~SceneOwnerFixture() {
            candidate->Shutdown();
            candidate.reset();
            structural.reset();
            participant.reset();
        }

        static Runtime::PersistentEntityIdentityMap InitialIdentities(std::uint64_t scene) {
            return Take(Runtime::PersistentEntityIdentityMap::Create({scene}, {}, {}));
        }

        Runtime::PersistentEntityIdentityMap MapIdentities() const {
            std::vector<Runtime::PersistentEntityRecord> records;
            std::vector<Runtime::PersistentEntityRuntimeBinding> bindings;
            for (const auto &entity : definition.Entities()) {
                std::array<std::uint8_t, 16> bytes{};
                bytes.front() = 0x91;
                bytes.back() = static_cast<std::uint8_t>(entity.object.value);
                const auto id = Take(Runtime::PersistentEntityId::FromBytes(bytes));
                records.push_back({id,
                                   {7},
                                   Runtime::AuthoredPersistentEntityProvenance{definition.Id(), entity.object},
                                   Runtime::PersistentEntityDisposition::Live});
                const auto actual = view.Find(entity.object);
                REQUIRE(actual);
                bindings.push_back({id, {7}, *actual});
            }
            return Take(Runtime::PersistentEntityIdentityMap::Create(view.RuntimeId(), records, bindings));
        }

        void Publish() {
            Check(candidate->ValidatePublication());
            candidate->Publish();
        }

        void Advance(unsigned count = 1) {
            for (unsigned i = 0; i < count; ++i)
                Check(host.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)));
        }

        std::shared_ptr<Detail::PreparedPhysicsSceneCapture> PrepareCapture() {
            const auto target = Take(host.PublishedStateTarget());
            return Take(target.PrepareCapture(identities, SceneStoragePolicy(), content->Fingerprint(), content));
        }

        Result<std::shared_ptr<Detail::PreparedPhysicsSceneRestore>> RestoreResult(std::span<const std::byte> wire) {
            const auto target = Take(host.PendingStateTarget(view.RuntimeId()));
            auto input = target.DecodeInput(wire, 8U * 1024U * 1024U, 256U * 1024U * 1024U);
            if (input.HasError())
                return Result<std::shared_ptr<Detail::PreparedPhysicsSceneRestore>>::Failure(input.ErrorValue());
            return target.PrepareRestore(std::move(input).Value(), identities, SceneStoragePolicy(), content->Fingerprint(), content, {});
        }

        std::shared_ptr<Detail::PreparedPhysicsSceneRestore> Restore(std::span<const std::byte> wire) {
            return Take(RestoreResult(wire));
        }
    };
}  // namespace Horo::Physics::StateTest
