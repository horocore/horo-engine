#include "AllocationProbe.h"
#include "Horo/Network/SceneReplicationApplyOwner.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "ReplicationCaptureTestSupport.h"
#include "ReplicationStateCodecTestSupport.h"

#include <limits>

namespace Horo::Network {
    using namespace CaptureTestSupport;

    namespace {
        /** @brief Declared test projection shared by the two genuine ownership modes. */
        auto TransformBindings() {
            using enum SceneReplicationProperty;
            return std::array{SceneReplicationFieldBinding{FieldIdValue(1), TranslationX},
                              SceneReplicationFieldBinding{FieldIdValue(2), TranslationY},
                              SceneReplicationFieldBinding{FieldIdValue(3), TranslationZ}};
        }

        struct SceneFixture final {
            std::shared_ptr<Runtime::RuntimeScene> scene;
            std::shared_ptr<SceneReplicationCommitSource> source;
            ReplicationWorldLifecycle lifecycle{Lifecycle()};
            std::shared_ptr<CountingCodec> codec{std::make_shared<CountingCodec>()};
            std::shared_ptr<const ReplicationSerializerRegistry> registry{Registry(codec, 3)};
            std::unique_ptr<ReplicationStateCapture> capture;
            Runtime::EntityRef entity;

            SceneFixture() {
                Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, {1}};
                Runtime::RuntimeEntityDefinition definition;
                definition.object = Runtime::SceneObjectId{1};
                builder.Add(definition);
                auto created = Runtime::RuntimeScene::Create(std::move(builder).Build().Value(), Runtime::SceneRuntimeId{10});
                REQUIRE(created.HasValue());
                scene = std::shared_ptr<Runtime::RuntimeScene>{std::move(created).Value()};
                entity = *scene->View().Find(Runtime::SceneObjectId{1});
                source = SceneReplicationCommitSource::Create(scene, TransformBindings()).Value();
                REQUIRE(lifecycle.RegisterObject(World().scene, World().session, Object()).HasValue());
                const std::array targets{ReplicationCaptureTarget{Object(), source}};
                capture = std::move(ReplicationStateCapture::Prepare(Read(lifecycle), registry, targets)).Value();
            }

            void Commit(const std::uint64_t tick, const Math::Vec3 translation) const {
                Runtime::SceneCommandBuffer commands;
                Math::Transform transform;
                transform.translation = translation;
                commands.SetLocalTransform(entity, transform);
                REQUIRE(source->CommitSimulationTick(commands, tick).HasValue());
            }

            Result<ReplicationCaptureReport> Capture(const Runtime::RuntimePhase phase = Runtime::RuntimePhase::NetworkFlush) {
                return source->CaptureAfterCommit(lifecycle, *capture, phase);
            }

            ReplicationCapturedStatePin Pin() const {
                return capture->Latest(Object().object).Value();
            }
        };

        /** @brief Drives fixed attempts directly through the Runtime clock interface. */
        class CaptureClock final : public Clock {
        public:
            [[nodiscard]] Duration MonotonicNow() const override {
                return now;
            }

            Duration now;
        };

        /** @brief Uses the real runtime callbacks to distinguish attempted Scene writes from committed evidence. */
        class ObservedScene final : public Runtime::RuntimeLifecycleParticipant {
        public:
            ObservedScene(SceneFixture &fixture, std::shared_ptr<SceneReplicationCommitSource> source)
                : fixture_(fixture), source_(std::move(source)) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(Runtime::RuntimePhase, const Runtime::FrameContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
                evidence = context.dispatchEvidence;
                premature = source_->ObserveCommittedTick(fixture_.scene->View(), evidence);
                if (reject)
                    return Result<void>::Failure(MakeError(ReplicationCaptureErrors::Uncommitted));
                Runtime::SceneCommandBuffer commands;
                Math::Transform transform;
                transform.translation = {3, 4, 5};
                commands.SetLocalTransform(fixture_.entity, transform);
                const auto committed = fixture_.scene->Commit(commands);
                return committed.HasError() ? Result<void>::Failure(committed.ErrorValue()) : Result<void>::Success();
            }

            void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
                observed = source_->ObserveCommittedTick(fixture_.scene->View(), context.dispatchEvidence);
                repeated = source_->ObserveCommittedTick(fixture_.scene->View(), context.dispatchEvidence);
            }

            void Shutdown() noexcept override {}

            Runtime::RuntimeDispatchEvidence evidence;
            bool reject{}, premature{}, observed{}, repeated{};

        private:
            SceneFixture &fixture_;
            std::shared_ptr<SceneReplicationCommitSource> source_;
        };

        /** @brief Runs the observed Scene through the real Runtime lifecycle and fixed commit issuer. */
        struct ObservedSceneFixture final {
            SceneFixture scene;
            using SceneScope = std::shared_ptr<const Runtime::RuntimeScene>;
            std::shared_ptr<SceneScope> owner = std::make_shared<SceneScope>(scene.scene);
            std::weak_ptr<const SceneScope> lifetime = owner;
            CaptureClock clock;
            std::unique_ptr<Runtime::RuntimeHost> runtime =
                Runtime::RuntimeHost::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)}).Value();
            const NetworkObjectMappingEntry mapping = Object();
            std::shared_ptr<SceneReplicationCommitSource> source;
            ObservedScene *observer{};

            ObservedSceneFixture() : ObservedSceneFixture(TransformBindings(), {}) {}

            ObservedSceneFixture(std::span<const SceneReplicationFieldBinding> fields, SceneReplicationVelocitySource velocity,
                                 SceneReplicationActionSource action = {}, SceneReplicationTransformSource transform = {}) {
                auto selected =
                    SceneReplicationCommitSource::CreateObservedWithSources(scene.scene->View().RuntimeId(), runtime->DispatchSource(),
                                                                            mapping.provenance.schema, mapping.provenance.schemaVersion,
                                                                            fields, owner,
                                                                            {.velocity = std::move(velocity),
                                                                             .action = std::move(action),
                                                                             .transform = std::move(transform)});
                REQUIRE(selected.HasValue());
                source = std::move(selected).Value();
                auto participant = std::make_unique<ObservedScene>(scene, source);
                observer = participant.get();
                REQUIRE(runtime->AddParticipant(std::move(participant)).HasValue());
                REQUIRE(runtime->Startup().HasValue());
                REQUIRE(runtime->RunFrame().HasValue());
            }

            Result<void> Step() {
                clock.now += Duration::FromMilliseconds(10);
                return runtime->RunFrame();
            }

            Result<ReplicationWorldCaptureRead> Open(const Runtime::RuntimePhase phase = Runtime::RuntimePhase::NetworkFlush) const {
                return source->AcquireCaptureReadAfterCommit(scene.lifecycle, phase);
            }
        };
    }  // namespace

    TEST_CASE("Declared public action tags cross the scalar codec and apply without steering the Scene",
              "[network][capture][scene][actions]") {
        const std::array fields{SceneReplicationFieldBinding{FieldIdValue(1), SceneReplicationProperty::AnimationActionTag}};
        std::int32_t tag = 31;
        ObservedSceneFixture fixture{fields, {}, [&tag](const Runtime::RuntimeEntityView &) {
            return Result<std::int32_t>::Success(tag);
        }};
        REQUIRE(fixture.Step().HasValue());
        const auto descriptor = BuildReplicationDescriptorSnapshot(std::array{Schema(10, {Field(1)})}, Limits).Value();
        const auto scalar = CanonicalScalarReplicationSerializer::Create(
                                {ValueType(1), Codec(1), {.value = "game.replication"}, ReplicationValueKind::SignedInteger, {}, 8, 1})
                                .Value();
        const std::array<std::shared_ptr<const IReplicationFieldSerializer>, 1> serializers{scalar};
        const auto registry =
            std::make_shared<const ReplicationSerializerRegistry>(ReplicationSerializerRegistry::Create(descriptor, serializers).Value());
        const std::array targets{ReplicationCaptureTarget{fixture.mapping, fixture.source}};
        auto capture = ReplicationStateCapture::Prepare(fixture.Open().Value(), registry, targets).Value();
        const auto codec = StateCodecTestSupport::MakeCodec(registry);
        const auto apply = SceneReplicationApplyOwner::Create(fixture.scene.scene, fixture.mapping.provenance.schema,
                                                              fixture.mapping.provenance.schemaVersion, fields)
                               .Value();
        const auto publish = [&] {
            return fixture.source->CaptureAfterCommit(fixture.scene.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush).Value();
        };
        REQUIRE(publish().published == 1);
        const auto first = capture->Latest(fixture.mapping.object).Value();
        const auto decoded = codec->Decode(codec->Encode(first).Value(), fixture.mapping).Value();
        REQUIRE(decoded.Fields().size() == 1);
        CHECK(std::get<std::int64_t>(decoded.Fields().front().value) == tag);
        const std::array updates{ReplicationApplyUpdate{fixture.mapping, StateCodecTestSupport::Recipient(), decoded}};
        REQUIRE(apply->Prepare(updates).Value()->Commit({}).HasValue());
        CHECK(fixture.scene.scene->View().Get(fixture.mapping.entity).Value().localTransform->translation == Math::Vec3{3, 4, 5});
        REQUIRE(fixture.Step().HasValue());
        tag = -1;
        CHECK(publish().failed == 1);
        CHECK(capture->Latest(fixture.mapping.object).Value() == first);
    }

    TEST_CASE("Observed Scene captures only declared Character velocity axes with the committed transform", "[network][capture][scene]") {
        using enum SceneReplicationProperty;
        const std::array fields{SceneReplicationFieldBinding{FieldIdValue(1), TranslationX},
                                SceneReplicationFieldBinding{FieldIdValue(2), LinearVelocityX},
                                SceneReplicationFieldBinding{FieldIdValue(3), LinearVelocityZ}};
        Math::Vec3 velocity{6.5F, 9.0F, -2.0F};
        std::size_t samples{};
        ObservedSceneFixture fixture{fields, [&](const Runtime::RuntimeEntityView &actor) {
            CHECK(actor.entity == Object().entity);
            ++samples;
            return Result<Math::Vec3>::Success(velocity);
        }};
        CHECK(SceneReplicationCommitSource::Create(fixture.scene.scene, fields).HasError());
        CHECK(fixture.Open().HasError());
        CHECK(samples == 0);
        REQUIRE(fixture.Step().HasValue());
        const std::array targets{ReplicationCaptureTarget{fixture.mapping, fixture.source}};
        auto capture = ReplicationStateCapture::Prepare(fixture.Open().Value(), fixture.scene.registry, targets).Value();
        const auto sample = [&] {
            return fixture.source->CaptureAfterCommit(fixture.scene.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush);
        };
        REQUIRE(sample().Value().published == 1);
        const auto first = capture->Latest(fixture.mapping.object).Value();
        REQUIRE(first->Fields().size() == 3);
        CHECK(std::get<double>(first->Fields()[0].value) == 3.0);
        CHECK(std::get<double>(first->Fields()[1].value) == 6.5);
        CHECK(std::get<double>(first->Fields()[2].value) == -2.0);
        CHECK(samples == 1);
        REQUIRE(fixture.Step().HasValue());
        velocity.x = std::numeric_limits<float>::infinity();
        const auto rejected = sample();
        REQUIRE(rejected.HasValue());
        CHECK(rejected.Value().published == 0);
        CHECK(rejected.Value().failed == 1);
        CHECK(capture->Latest(fixture.mapping.object).Value() == first);
    }

    TEST_CASE("Application-owned Scene capture rejects attempted and aborted fixed work", "[network][capture][scene]") {
        ObservedSceneFixture fixture;
        CHECK(fixture.source->CommitSimulationTick({}, 1).HasError());
        CHECK(fixture.Open().HasError());
        fixture.observer->reject = true;
        REQUIRE(fixture.Step().HasError());
        CHECK_FALSE(fixture.observer->premature);
        CHECK_FALSE(fixture.observer->observed);
        const auto read = fixture.Open();
        REQUIRE(read.HasError());
        CHECK(read.ErrorValue().code.Value() == ReplicationCaptureErrors::Uncommitted.code.Value());
    }

    TEST_CASE("Observed Scene capture publishes the adjacent owner's transform at the admitted scheduler tick",
              "[network][capture][scene]") {
        using Projection = Result<std::optional<Math::Transform>>;
        std::optional<Math::Transform> projected{Math::Transform{.translation = {12.0F, 14.0F, 16.0F}}};
        std::uint64_t sampledTick{};
        ObservedSceneFixture fixture{TransformBindings(),
                                     {},
                                     {},
                                     [&projected, &sampledTick](const Runtime::RuntimeSceneView &scene,
                                                                const Runtime::RuntimeEntityView &entity, const std::uint64_t tick) {
            CHECK(scene.IsCurrent());
            CHECK(scene.RuntimeId() == entity.entity.runtime);
            CHECK(entity.entity == Object().entity);
            sampledTick = tick;
            return Projection::Success(projected);
        }};
        REQUIRE(fixture.Step().HasValue());
        const std::array targets{ReplicationCaptureTarget{fixture.mapping, fixture.source}};
        auto capture = ReplicationStateCapture::Prepare(fixture.Open().Value(), fixture.scene.registry, targets).Value();
        const auto sample = [&] {
            const auto captured =
                fixture.source->CaptureAfterCommit(fixture.scene.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush);
            REQUIRE(captured.HasValue());
            return captured.Value();
        };
        REQUIRE(sample().published == 1);
        const auto first = capture->Latest(fixture.mapping.object).Value();
        CHECK(first->SimulationTick() == sampledTick);
        CHECK(std::get<double>(first->Fields()[0].value) == 12.0);
        CHECK(fixture.scene.scene->View().Get(fixture.mapping.entity).Value().localTransform->translation.x == 3.0F);

        REQUIRE(fixture.Step().HasValue());
        projected->translation.x = std::numeric_limits<float>::infinity();
        CHECK(sample().failed == 1);
        CHECK(capture->Latest(fixture.mapping.object).Value() == first);
        projected.reset();
        REQUIRE(fixture.Step().HasValue());
        REQUIRE(sample().published == 1);
        CHECK(std::get<double>(capture->Latest(fixture.mapping.object).Value()->Fields()[0].value) == 3.0);
    }

    TEST_CASE("Application-owned Scene capture reads its exact committed fixed tick and mapped schema", "[network][capture][scene]") {
        ObservedSceneFixture fixture;
        REQUIRE(fixture.Step().HasValue());
        CHECK_FALSE(fixture.observer->premature);
        CHECK(fixture.observer->observed);
        CHECK_FALSE(fixture.observer->repeated);
        CHECK_FALSE(fixture.source->ObserveCommittedTick(fixture.scene.scene->View(), fixture.observer->evidence));
        CHECK(fixture.Open(Runtime::RuntimePhase::FixedUpdate).HasError());

        const auto opened = fixture.Open();
        REQUIRE(opened.HasValue());
        CHECK(opened.Value().SimulationTick() == 1);
        CHECK(opened.Value().MappingRevision() == Read(fixture.scene.lifecycle).MappingRevision());
        auto mismatched = fixture.mapping;
        ++mismatched.provenance.schemaVersion.minor;
        CHECK(fixture.source->BeginRead(mismatched, opened.Value().SimulationTick()).HasError());

        const std::array targets{ReplicationCaptureTarget{fixture.mapping, fixture.source}};
        auto capture = ReplicationStateCapture::Prepare(opened.Value(), fixture.scene.registry, targets).Value();
        REQUIRE(
            fixture.source->CaptureAfterCommit(fixture.scene.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush).Value().published ==
            1);
        const auto captured = capture->Latest(fixture.mapping.object).Value();
        CHECK(captured->SimulationTick() == opened.Value().SimulationTick());
        CHECK(std::get<double>(captured->Fields()[0].value) == 3.0);
        CHECK(std::get<double>(captured->Fields()[1].value) == 4.0);
        CHECK(std::get<double>(captured->Fields()[2].value) == 5.0);
    }

    TEST_CASE("Application-owned Scene capture pins exactly one live source read", "[network][capture][scene]") {
        ObservedSceneFixture fixture;
        REQUIRE(fixture.Step().HasValue());
        const auto opened = fixture.source->BeginRead(fixture.mapping, 1);
        REQUIRE(opened.HasValue());
        fixture.owner.reset();
        CHECK_FALSE(fixture.lifetime.expired());
        CHECK(fixture.source->IsCurrent(opened.Value()));
        fixture.source->EndRead(opened.Value());
        CHECK(fixture.lifetime.expired());
        REQUIRE(fixture.scene.scene->View().IsCurrent());
        CHECK(fixture.source->BeginRead(fixture.mapping, 1).HasError());
        CHECK(fixture.Open().HasError());
    }

    TEST_CASE("Real Scene capture observes the complete committed tick and three declared scalar fields", "[network][capture][scene]") {
        SceneFixture fixture;
        REQUIRE(fixture.Capture().HasError());
        fixture.Commit(1, {3, 4, 5});
        REQUIRE(fixture.Capture(Runtime::RuntimePhase::FixedUpdate).HasError());
        const auto result = fixture.Capture();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().published == 1);
        const auto first = fixture.Pin();
        REQUIRE(first->SimulationTick() == 1);
        REQUIRE(std::get<double>(first->Fields()[0].value) == 3.0);
        REQUIRE(std::get<double>(first->Fields()[1].value) == 4.0);
        REQUIRE(std::get<double>(first->Fields()[2].value) == 5.0);
        fixture.Commit(2, {6, 7, 8});
        REQUIRE(fixture.Capture().Value().published == 1);
        REQUIRE(std::get<double>(first->Fields()[0].value) == 3.0);
        REQUIRE(fixture.codec->encodeCalls == 0);
    }

    TEST_CASE("Failed Scene transactions preserve committed capture and reject incomplete owner state", "[network][capture][scene]") {
        SceneFixture fixture;
        fixture.Commit(1, {1, 2, 3});
        REQUIRE(fixture.Capture().HasValue());
        const auto prior = fixture.Pin();
        Runtime::SceneCommandBuffer partial;
        Math::Transform proposed;
        proposed.translation = {9, 9, 9};
        partial.SetLocalTransform(fixture.entity, proposed);
        partial.Destroy({Runtime::SceneRuntimeId{99}, {0, 1}});
        REQUIRE(fixture.source->CommitSimulationTick(partial, 2).HasError());
        REQUIRE(fixture.scene->View().Get(fixture.entity).Value().localTransform->translation == Math::Vec3{1, 2, 3});
        REQUIRE(fixture.capture->Latest(Object().object).Value() == prior);
        REQUIRE(fixture.Capture().HasError());  // Tick1 was already captured; no fabricated tick2 publication.
        fixture.Commit(2, {4, 5, 6});
        REQUIRE(fixture.Capture().Value().published == 1);
    }

    TEST_CASE("Open real owner reads reject reentrant wrapper mutation and detect direct Scene commit bypass",
              "[network][capture][scene]") {
        SceneFixture fixture;
        fixture.Commit(1, {1, 2, 3});
        const auto begun = fixture.source->BeginRead(Object(), 1);
        REQUIRE(begun.HasValue());
        Runtime::SceneCommandBuffer commands;
        Math::Transform changed;
        changed.translation = {7, 8, 9};
        commands.SetLocalTransform(fixture.entity, changed);
        REQUIRE(fixture.source->CommitSimulationTick(commands, 2).HasError());
        REQUIRE(fixture.scene->Commit(commands).HasValue());
        REQUIRE_FALSE(fixture.source->IsCurrent(begun.Value()));
        fixture.source->EndRead(begun.Value());
        REQUIRE(fixture.Capture().HasError());
        REQUIRE(fixture.capture->Latest(Object().object).HasError());
    }

    TEST_CASE("A real Scene mutation inside a codec callback discards the entire copied candidate", "[network][capture][scene]") {
        SceneFixture fixture;
        fixture.Commit(1, {1, 2, 3});
        fixture.codec->onCompare = [&fixture] {
            Runtime::SceneCommandBuffer changed;
            Math::Transform transform;
            transform.translation = {99, 99, 99};
            changed.SetLocalTransform(fixture.entity, transform);
            static_cast<void>(fixture.scene->Commit(changed));
        };
        const auto result = fixture.Capture();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().failed == 1);
        REQUIRE(result.Value().published == 0);
        REQUIRE(fixture.capture->Latest(Object().object).HasError());
    }

    TEST_CASE("Real Scene NetworkFlush capture stays allocation-free for unchanged committed state",
              "[network][capture][scene][allocation]") {
        SceneFixture fixture;
        fixture.Commit(1, {1, 2, 3});
        REQUIRE(fixture.Capture().HasValue());
        REQUIRE(fixture.source->CommitSimulationTick({}, 2).HasValue());
        const auto before = Tests::AllocationProbe::Count();
        const auto reclaimed = Tests::AllocationProbe::FreeCount();
        const auto result = fixture.Capture();
        const auto after = Tests::AllocationProbe::Count();
        const auto freed = Tests::AllocationProbe::FreeCount();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().unchanged == 1);
        REQUIRE(before == after);
        REQUIRE(reclaimed == freed);
    }

    TEST_CASE("Scene adapter pins the real owner until quiescent shutdown while snapshots contain copied values",
              "[network][capture][scene]") {
        SceneFixture fixture;
        fixture.Commit(1, {1, 2, 3});
        REQUIRE(fixture.Capture().HasValue());
        auto snapshot = fixture.Pin();
        std::weak_ptr<Runtime::RuntimeScene> scene = fixture.scene;
        fixture.scene.reset();
        fixture.source.reset();
        fixture.capture->Shutdown();
        REQUIRE_FALSE(snapshot->IsCurrent());
        REQUIRE_FALSE(scene.expired());
        REQUIRE_FALSE(fixture.capture->CanReclaim());
        REQUIRE(std::get<double>(snapshot->Fields()[0].value) == 1.0);
        snapshot.reset();
        REQUIRE(fixture.capture->CanReclaim());
        fixture.capture.reset();
        REQUIRE(scene.expired());
    }
}  // namespace Horo::Network
