#include "Horo/Application/PhysicsTriggerVolumeHost.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
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
                                                                               const Runtime::RuntimeSceneView &scene) override {
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

        class PreparationProbeCandidate final : public Runtime::SceneStructuralCandidate {
        public:
            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }

            void Publish() noexcept override {}

            Result<void> AfterPublication() override {
                return Result<void>::Success();
            }
        };

        /** @brief A second owner registered before Physics proves validation is a batch-wide barrier, not an owner-local check. */
        class PreparationProbeOwner final : public Runtime::SceneStructuralParticipant {
        public:
            explicit PreparationProbeOwner(std::size_t &prepares) : prepares_(prepares) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return Runtime::SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &,
                                                                               std::span<const Runtime::RuntimeEntityView>,
                                                                               std::span<const Runtime::EntityRef>) override {
                ++prepares_;
                return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Success(std::make_unique<PreparationProbeCandidate>());
            }

        private:
            std::size_t &prepares_;
        };

        /** @brief Counts real owner preparation only; proof validation delegates to the production Physics owner. */
        class CountedPhysicsOwner final : public Runtime::SceneStructuralParticipant {
        public:
            CountedPhysicsOwner(std::unique_ptr<Runtime::SceneStructuralParticipant> owner, std::size_t &prepares)
                : owner_(std::move(owner)), prepares_(prepares) {}

            Runtime::SceneStructuralOwner Owner() const noexcept override {
                return owner_->Owner();
            }

            Result<void> ValidateBaselines(const Runtime::RuntimeSceneView &active,
                                           std::span<const Runtime::SceneBaselinePreparationView> baselines) const override {
                return owner_->ValidateBaselines(active, baselines);
            }

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &active,
                                                                               std::span<const Runtime::RuntimeEntityView> created,
                                                                               std::span<const Runtime::EntityRef> destroyed) override {
                ++prepares_;
                return owner_->Prepare(active, created, destroyed);
            }

        private:
            std::unique_ptr<Runtime::SceneStructuralParticipant> owner_;
            std::size_t &prepares_;
        };

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
            std::size_t ownerPrepares{};
            std::size_t otherOwnerPrepares{};

            explicit CanonicalBaselineFixture(const bool probeOtherOwner = false) {
                auto created = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
                REQUIRE(created.HasValue());
                runtime = std::move(created).Value();
                observer = std::make_unique<BaselinePhysicsObserver>(*runtime, authority);
                observed = observer.get();
                if (probeOtherOwner)
                    REQUIRE(scenes.AddStructuralParticipant(std::make_unique<PreparationProbeOwner>(otherOwnerPrepares)).HasValue());
                REQUIRE(scenes
                            .AddStructuralParticipant(
                                std::make_unique<CountedPhysicsOwner>(observer->participant.MakeStructuralParticipant(), ownerPrepares))
                            .HasValue());
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
                const auto materialValue =
                    PhysicsMaterialAsset::Create({.id = PhysicsMaterialAssetId::FromAssetId(material), .sourceRevision = 1});
                REQUIRE(materialValue.HasValue());
                const auto payload = EncodePhysicsMaterialAsset(materialValue.Value());
                REQUIRE(payload.HasValue());
                for (const auto byte : payload.Value())
                    artifact.payload.push_back(std::to_integer<std::uint8_t>(byte));
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

        /** @brief Owns the canonical participant, real material lease and portable binding for atomic batch oracles. */
        struct BoundBaselineFixture final {
            CanonicalBaselineFixture fixture{true};
            SceneSource::TriggerVolumeConversionContext binding;

            BoundBaselineFixture() {
                const auto settings = Settings();
                const auto material =
                    PhysicsMaterialAsset::Create({.id = PhysicsMaterialAssetId::FromAssetId(fixture.material), .sourceRevision = 1})
                        .Value();
                const auto materialBytes = EncodePhysicsMaterialAsset(material).Value();
                const auto bytes = std::span{reinterpret_cast<const std::uint8_t *>(materialBytes.data()), materialBytes.size()};
                Assets::AssetCookArtifact artifact{.id = fixture.material,
                                                   .type = fixture.type,
                                                   .target = Assets::AssetCookTargetId::Parse("test-host").Value(),
                                                   .sourceDigest = ComputeSha256(std::as_bytes(bytes)),
                                                   .payloadDigest = ComputeSha256(std::as_bytes(bytes)),
                                                   .payload = {bytes.begin(), bytes.end()}};
                const auto encoded = Assets::EncodeCookedArtifact(artifact).Value();
                fixture.pin = fixture.cache->Admit(std::as_bytes(std::span{encoded})).Value();
                binding = Application::CaptureProjectTriggerVolumeBinding(settings.simulation, Profile(), material, artifact).Value();
            }

            Runtime::RuntimeSceneDefinition Definition(std::uint64_t cell, std::uint64_t revision, bool valid) const {
                const std::array source{
                    SceneSource::SceneObjectSnapshot{.id = {874 + cell},
                                                     .components = {.triggerVolume = Runtime::TriggerVolumeComponent{}}}};
                auto capture = binding;
                if (!valid)
                    capture.binding.materialPayloadDigest.bytes.front() ^= 1;
                auto converted = SceneSource::ConvertSceneSourceToRuntime({source, {}}, {19 + cell}, {revision}, capture);
                REQUIRE(converted.HasValue());
                return std::move(converted).Value();
            }

            void Attach(Runtime::SceneCommandBuffer &commands, std::uint64_t cell, std::uint64_t revision, bool valid,
                        std::uint64_t expected) {
                REQUIRE(commands
                            .AttachBaseline(Definition(cell, revision, valid), {{{fixture.material, fixture.type}, fixture.pin}},
                                            fixture.admission, {4, 32, 32}, {expected}, fixture.check,
                                            std::make_shared<BaselineSourceOwnership>(Runtime::SceneDefinitionId{19 + cell}))
                            .HasValue());
            }

            void RequireRejectedReplacement() {
                const auto oldView = *fixture.scenes.ActiveScene();
                const auto oldEntity = oldView.Find({874});
                REQUIRE(oldEntity.has_value());
                const auto oldBody = fixture.observed->candidate->FindRuntimeBody(*oldEntity, {1});
                REQUIRE(oldBody.has_value());
                const auto oldWorld = fixture.observed->candidate->WorldIdentity();
                Runtime::SceneCommandBuffer replacement;
                Attach(replacement, 0, 2, true, 1);
                Attach(replacement, 1, 2, false, 1);
                REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(replacement)).HasValue());
                REQUIRE(fixture.scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token()))
                            .HasValue());
                REQUIRE(fixture.scenes.TakeOperationError().has_value());
                REQUIRE(fixture.ownerPrepares == 1);
                REQUIRE(fixture.otherOwnerPrepares == 1);
                REQUIRE(fixture.observed->candidate->WorldIdentity() == oldWorld);
                REQUIRE(fixture.observed->candidate->FindRuntimeBody(*oldEntity, {1}) == oldBody);
                REQUIRE(fixture.scenes.ActiveScene()->FindBaseline({19})->revision.value == 1);
                REQUIRE(fixture.scenes.ActiveScene()->FindBaseline({20})->revision.value == 1);
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

    TEST_CASE("Every baseline proof is checked before any native preparation in mixed atomic batches",
              "[physics][trigger][cell_attachment][native][atomic]") {
        BoundBaselineFixture bound;
        auto &fixture = bound.fixture;
        Runtime::SceneCommandBuffer mixed;
        REQUIRE(mixed.CreateGroup({Runtime::RuntimeEntityGroupEntry{}}, {}, fixture.admission).HasValue());
        SECTION("bad last baseline") {
            bound.Attach(mixed, 0, 1, true, 0);
            bound.Attach(mixed, 1, 1, false, 0);
        }
        SECTION("bad first baseline") {
            bound.Attach(mixed, 0, 1, false, 0);
            bound.Attach(mixed, 1, 1, true, 0);
        }
        REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(mixed)).HasValue());
        REQUIRE(fixture.scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Context(fixture.cancellation.Token()))
                    .HasValue());
        const auto failure = fixture.scenes.TakeOperationError();
        REQUIRE(failure.has_value());
        REQUIRE(failure->code.Value() == PhysicsErrors::MaterialDescriptorInvalid.code.Value());
        REQUIRE(fixture.ownerPrepares == 0);
        REQUIRE(fixture.otherOwnerPrepares == 0);
        REQUIRE_FALSE(fixture.scenes.ActiveScene()->FindBaseline({19}));
        REQUIRE_FALSE(fixture.scenes.ActiveScene()->FindBaseline({20}));
        REQUIRE(fixture.observed->candidate->BodyBindings().empty());

        Runtime::SceneCommandBuffer good;
        bound.Attach(good, 0, 1, true, 0);
        bound.Attach(good, 1, 1, true, 0);
        REQUIRE(good.CreateGroup({Runtime::RuntimeEntityGroupEntry{}}, {}, fixture.admission).HasValue());
        REQUIRE(fixture.scenes.QueueStructuralCommands(std::move(good)).HasValue());
        fixture.Commit();
        REQUIRE(fixture.ownerPrepares == 1);
        REQUIRE(fixture.otherOwnerPrepares == 1);
        bound.RequireRejectedReplacement();
    }
#endif
}  // namespace Horo::Physics
