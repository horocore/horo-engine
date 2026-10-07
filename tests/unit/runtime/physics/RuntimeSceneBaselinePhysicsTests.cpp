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

        struct CanonicalBaselineFixture final {
            std::unique_ptr<PhysicsRuntime> runtime;
            PhysicsSceneActivationAuthority authority;
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            JobSystem jobs{{1, 4}};
            Assets::AssetLoadService loads{jobs, provider};
            Runtime::RuntimeSceneService scenes{registry, loads};
            std::unique_ptr<BaselinePhysicsObserver> observer;
            BaselinePhysicsObserver *observed{};
            CancellationSource cancellation;
            Assets::AssetId material{Asset(91)};
            Assets::AssetTypeId type{AssetType()};
            std::unique_ptr<Assets::AssetPayloadCache> cache;
            Assets::AssetPayloadLease pin;
            std::shared_ptr<BaselineAdmission> check{std::make_shared<BaselineAdmission>()};
            Runtime::SceneStructuralAdmission admission;

            CanonicalBaselineFixture() {
                auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
                REQUIRE(created.HasValue());
                runtime = std::move(created).Value();
                observer = std::make_unique<BaselinePhysicsObserver>(*runtime, authority);
                observed = observer.get();
                REQUIRE(scenes.AddStructuralParticipant(observer->participant.MakeStructuralParticipant()).HasValue());
                REQUIRE(scenes.AddActivationParticipant(std::move(observer)).HasValue());
                REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
                REQUIRE(registry.Publish({Record(material, type)}).status == Assets::AssetRegistryBuildStatus::Complete);
                Runtime::SceneDefinitionBuilder empty{{99}, {1}};
                REQUIRE(scenes.QueuePreparation(RequireDefinition(std::move(empty))).HasValue());
                Commit();
                Assets::AssetCookArtifact artifact;
                artifact.id = material;
                artifact.type = type;
                artifact.target = AssetCookTargetId::Parse("test-host").Value();
                artifact.payload = {1, 2, 3};
                artifact.payloadDigest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
                auto encoded = Assets::EncodeCookedArtifact(artifact).Value();
                cache = Assets::AssetPayloadCache::Create(2, 1024 * 1024).Value();
                pin = cache->Admit(std::as_bytes(std::span{encoded})).Value();
                admission = {scenes.ActiveScene()->RuntimeId(), registry.Snapshot().Revision(), cancellation.Token(), {}};
            }

            void Commit() {
                REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(cancellation.Token())).HasValue());
                REQUIRE_FALSE(scenes.TakeOperationError().has_value());
            }

            void Attach(const std::uint64_t cell) {
                Runtime::SceneCommandBuffer commands;
                auto ownership = std::make_shared<BaselineSourceOwnership>(Runtime::SceneDefinitionId{19 + cell});
                REQUIRE(commands
                            .AttachBaseline(BaselineDefinition(material, type, cell), {{{material, type}, pin}}, admission, {4, 32, 32}, {},
                                            check, std::move(ownership))
                            .HasValue());
                REQUIRE(scenes.QueueStructuralCommands(std::move(commands)).HasValue());
                Commit();
            }

            void Detach(const std::uint64_t cell) {
                Runtime::SceneCommandBuffer commands;
                REQUIRE(commands
                            .DetachBaseline({19 + cell}, {1}, admission, check,
                                            std::make_shared<BaselineSourceOwnership>(Runtime::SceneDefinitionId{19 + cell}))
                            .HasValue());
                REQUIRE(scenes.QueueStructuralCommands(std::move(commands)).HasValue());
                Commit();
            }
        };
    }  // namespace

    TEST_CASE("Canonical Physics keeps stable body handles across independent cell baselines",
              "[physics][scene][cell_attachment][native]") {
        CanonicalBaselineFixture fixture;
        fixture.Attach(0);
        REQUIRE(fixture.observed->candidate != nullptr);
        const auto world = fixture.observed->candidate->WorldIdentity();
        const auto domain = fixture.scenes.ActiveScene()->RuntimeId();
        const auto first = *fixture.scenes.ActiveScene()->Find({1});
        const auto body = fixture.observed->candidate->FindRuntimeBody(first, {100});
        const auto shape = fixture.observed->candidate->FindRuntimeShape(first, {200});
        const auto constraint = fixture.observed->candidate->FindRuntimeConstraint(first, {500});
        REQUIRE(body.has_value());
        REQUIRE(shape.has_value());
        REQUIRE(constraint.has_value());
        fixture.Attach(1);
        const auto second = *fixture.scenes.ActiveScene()->Find({11});
        REQUIRE(fixture.observed->candidate->FindRuntimeBody(first, {100}) == body);
        REQUIRE(fixture.observed->candidate->FindRuntimeShape(first, {200}) == shape);
        REQUIRE(fixture.observed->candidate->FindRuntimeConstraint(first, {500}) == constraint);
        REQUIRE(fixture.observed->candidate->FindRuntimeBody(second, {100})->world == world);
        REQUIRE(fixture.scenes.ActiveScene()->RuntimeId() == domain);
        fixture.scenes.Shutdown();
    }

    TEST_CASE("Canonical Physics retires one baseline without disturbing another", "[physics][scene][cell_attachment][native]") {
        CanonicalBaselineFixture fixture;
        fixture.Attach(0);
        fixture.Attach(1);
        const auto first = *fixture.scenes.ActiveScene()->Find({1});
        const auto second = *fixture.scenes.ActiveScene()->Find({11});
        const auto world = fixture.observed->candidate->WorldIdentity();
        fixture.Detach(0);
        REQUIRE_FALSE(fixture.observed->candidate->FindRuntimeBody(first, {100}).has_value());
        REQUIRE_FALSE(fixture.observed->candidate->FindRuntimeShape(first, {200}).has_value());
        REQUIRE_FALSE(fixture.observed->candidate->FindRuntimeConstraint(first, {500}).has_value());
        REQUIRE(fixture.observed->candidate->FindRuntimeBody(second, {100}).has_value());
        REQUIRE(fixture.observed->candidate->WorldIdentity() == world);
        fixture.scenes.Shutdown();
    }
#endif
}  // namespace Horo::Physics
