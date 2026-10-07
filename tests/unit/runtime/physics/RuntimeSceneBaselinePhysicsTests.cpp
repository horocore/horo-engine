#include "Horo/Assets/AssetCook.h"
#include "PhysicsSceneActivationTestSupport.h"

namespace Horo::Physics {
#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        /** @brief Test observer borrowing the real candidate owned by the canonical Scene service. */
        class BaselinePhysicsObserver final : public Runtime::SceneActivationParticipant {
        public:
            BaselinePhysicsObserver(PhysicsRuntime &runtime, PhysicsSceneActivationAuthority &authority)
                : participant(runtime, authority, Settings()) {}

            Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                               Runtime::RuntimeSceneView scene) override {
                auto result = participant.Prepare(definition, scene);
                if (result.HasValue())
                    candidate = dynamic_cast<PhysicsSceneActivationCandidate *>(result.Value().get());
                return result;
            }

            PhysicsSceneActivationParticipant participant;
            PhysicsSceneActivationCandidate *candidate{};
        };

        class BaselineAdmission final : public Runtime::ScenePublicationCheck {
        public:
            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }
        };

        /** @brief Immutable source namespace for this generic Scene/Physics test, independent from readiness. */
        class BaselineSourceOwnership final : public Runtime::SceneBaselineOwnership {
        public:
            explicit BaselineSourceOwnership(const Runtime::SceneDefinitionId id) : id(id) {}

            bool Matches(const Runtime::SceneBaselineOwnership &other) const noexcept override {
                const auto *source = dynamic_cast<const BaselineSourceOwnership *>(&other);
                return source && source->id == id;
            }

            Runtime::SceneDefinitionId id;
        };

        Runtime::RuntimeSceneDefinition BaselineDefinition(const Assets::AssetId material, const Assets::AssetTypeId type,
                                                           const std::uint64_t cell) {
            Runtime::SceneDefinitionBuilder builder{{19 + cell}, {1}};
            const auto source = PhysicsDefinition(material, type);
            for (auto entity : source.Entities()) {
                entity.object.value += 10 * cell;
                for (auto &collider : entity.components.colliders)
                    collider.body.object.value += 10 * cell;
                for (auto &constraint : entity.components.physicsConstraints) {
                    constraint.first.body.object.value += 10 * cell;
                    if (auto *second = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&constraint.second))
                        second->body.object.value += 10 * cell;
                }
                builder.Add(std::move(entity));
            }
            REQUIRE(builder.RequireAsset({material, type}).HasValue());
            return RequireDefinition(std::move(builder));
        }
    }  // namespace

    TEST_CASE("Canonical Physics keeps one world and stable resident bodies across independent baselines",
              "[physics][scene][cell_attachment][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        PhysicsSceneActivationAuthority authority;
        Assets::AssetRegistry registry;
        Assets::MemoryAssetProvider provider;
        JobSystem jobs{{1, 4}};
        Assets::AssetLoadService loads{jobs, provider};
        Runtime::RuntimeSceneService scenes{registry, loads};
        auto observer = std::make_unique<BaselinePhysicsObserver>(*runtime, authority);
        auto *observed = observer.get();
        REQUIRE(scenes.AddStructuralParticipant(observer->participant.MakeStructuralParticipant()).HasValue());
        REQUIRE(scenes.AddActivationParticipant(std::move(observer)).HasValue());
        CancellationSource cancellation;
        REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
        const auto material = Asset(91);
        const auto type = AssetType();
        REQUIRE(registry.Publish({Record(material, type)}).status == Assets::AssetRegistryBuildStatus::Complete);
        Runtime::SceneDefinitionBuilder empty{{99}, {1}};
        REQUIRE(scenes.QueuePreparation(RequireDefinition(std::move(empty))).HasValue());
        const auto commit = [&] {
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
            REQUIRE_FALSE(scenes.TakeOperationError().has_value());
        };
        commit();
        REQUIRE(observed->candidate != nullptr);
        const auto world = observed->candidate->WorldIdentity();
        const auto domain = scenes.ActiveScene()->RuntimeId();
        Assets::AssetCookArtifact artifact;
        artifact.id = material;
        artifact.type = type;
        artifact.target = AssetCookTargetId::Parse("test-host").Value();
        artifact.payload = {1, 2, 3};
        artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
        auto encoded = Assets::EncodeCookedArtifact(artifact).Value();
        auto cache = Assets::AssetPayloadCache::Create(2, 1024 * 1024).Value();
        auto pin = cache->Admit(std::as_bytes(std::span{encoded})).Value();
        const auto check = std::make_shared<BaselineAdmission>();
        const Runtime::SceneStructuralAdmission admission{domain, registry.Snapshot().Revision(), cancellation.Token(), {}};
        const auto attach = [&](const std::uint64_t cell) {
            Runtime::SceneCommandBuffer commands;
            REQUIRE(commands
                        .AttachBaseline(BaselineDefinition(material, type, cell), {{{material, type}, pin}}, admission, {4, 32, 32}, {},
                                        check, std::make_shared<BaselineSourceOwnership>(Runtime::SceneDefinitionId{19 + cell}))
                        .HasValue());
            REQUIRE(scenes.QueueStructuralCommands(std::move(commands)).HasValue());
            commit();
        };
        attach(0);
        const auto first = *scenes.ActiveScene()->Find({1});
        const auto body = observed->candidate->FindRuntimeBody(first, {100});
        const auto shape = observed->candidate->FindRuntimeShape(first, {200});
        const auto constraint = observed->candidate->FindRuntimeConstraint(first, {500});
        REQUIRE(body.has_value());
        REQUIRE(shape.has_value());
        REQUIRE(constraint.has_value());
        REQUIRE(body->world == world);
        attach(1);
        const auto second = *scenes.ActiveScene()->Find({11});
        REQUIRE(observed->candidate->FindRuntimeBody(first, {100}) == body);
        REQUIRE(observed->candidate->FindRuntimeShape(first, {200}) == shape);
        REQUIRE(observed->candidate->FindRuntimeConstraint(first, {500}) == constraint);
        REQUIRE(observed->candidate->FindRuntimeBody(second, {100})->world == world);
        REQUIRE(scenes.ActiveScene()->RuntimeId() == domain);
        Runtime::SceneCommandBuffer retire;
        REQUIRE(
            retire.DetachBaseline({19}, {1}, admission, check, std::make_shared<BaselineSourceOwnership>(Runtime::SceneDefinitionId{19}))
                .HasValue());
        REQUIRE(scenes.QueueStructuralCommands(std::move(retire)).HasValue());
        commit();
        REQUIRE_FALSE(observed->candidate->FindRuntimeBody(first, {100}).has_value());
        REQUIRE_FALSE(observed->candidate->FindRuntimeShape(first, {200}).has_value());
        REQUIRE_FALSE(observed->candidate->FindRuntimeConstraint(first, {500}).has_value());
        REQUIRE(observed->candidate->FindRuntimeBody(second, {100}).has_value());
        REQUIRE(observed->candidate->WorldIdentity() == world);
        scenes.Shutdown();
        scenes.Shutdown();
    }
#endif
}  // namespace Horo::Physics
