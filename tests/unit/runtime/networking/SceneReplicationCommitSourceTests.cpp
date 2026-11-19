#include "AllocationProbe.h"
#include "Horo/Runtime/RuntimeHost.h"
#include "ReplicationCaptureTestSupport.h"

namespace Horo::Network {
    using namespace CaptureTestSupport;

    namespace {
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
                using enum SceneReplicationProperty;
                const std::array fields{SceneReplicationFieldBinding{FieldIdValue(1), TranslationX},
                                        SceneReplicationFieldBinding{FieldIdValue(2), TranslationY},
                                        SceneReplicationFieldBinding{FieldIdValue(3), TranslationZ}};
                source = SceneReplicationCommitSource::Create(scene, fields).Value();
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
    }  // namespace

    TEST_CASE("Application-owned Scene capture accepts only the real committed fixed callback and exact mapped schema",
              "[network][capture][scene]") {
        SceneFixture fixture;

        struct SceneScope final {
            std::shared_ptr<const Runtime::RuntimeScene> scene;
        };

        auto owner = std::make_shared<SceneScope>(SceneScope{fixture.scene});
        const std::weak_ptr<const SceneScope> lifetime = owner;
        CaptureClock clock;
        auto runtime = Runtime::RuntimeHost::Create(clock, {.fixedStep = Duration::FromMilliseconds(10)}).Value();
        const auto mapping = Object();
        using enum SceneReplicationProperty;
        const std::array fields{SceneReplicationFieldBinding{FieldIdValue(1), TranslationX},
                                SceneReplicationFieldBinding{FieldIdValue(2), TranslationY},
                                SceneReplicationFieldBinding{FieldIdValue(3), TranslationZ}};
        auto source =
            SceneReplicationCommitSource::CreateObserved(fixture.scene->View().RuntimeId(), runtime->DispatchSource(),
                                                         mapping.provenance.schema, mapping.provenance.schemaVersion, fields, owner);
        REQUIRE(source.HasValue());
        CHECK(source.Value()->CommitSimulationTick({}, 1).HasError());
        const std::array targets{ReplicationCaptureTarget{mapping, source.Value()}};
        auto capture = ReplicationStateCapture::Prepare(Read(fixture.lifecycle), fixture.registry, targets).Value();
        auto observer = std::make_unique<ObservedScene>(fixture, source.Value());
        auto &state = *observer;
        REQUIRE(runtime->AddParticipant(std::move(observer)).HasValue());
        REQUIRE(runtime->Startup().HasValue());
        REQUIRE(runtime->RunFrame().HasValue());
        CHECK(source.Value()->BeginRead(mapping, 1).HasError());
        SECTION("failed fixed work remains entirely unpublished") {
            state.reject = true;
            clock.now = Duration::FromMilliseconds(10);
            REQUIRE(runtime->RunFrame().HasError());
            CHECK_FALSE(state.premature);
            CHECK_FALSE(state.observed);
            CHECK(source.Value()->CaptureAfterCommit(fixture.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush).HasError());
        }
        SECTION("complete post-commit capture reads only the configured public transform") {
            clock.now = Duration::FromMilliseconds(10);
            REQUIRE(runtime->RunFrame().HasValue());
            CHECK_FALSE(state.premature);
            CHECK(state.observed);
            CHECK_FALSE(state.repeated);
            CHECK_FALSE(source.Value()->ObserveCommittedTick(fixture.scene->View(), state.evidence));
            auto mismatched = mapping;
            ++mismatched.provenance.schemaVersion.minor;
            CHECK(source.Value()->BeginRead(mismatched, 1).HasError());
            REQUIRE(
                source.Value()->CaptureAfterCommit(fixture.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush).Value().published ==
                1);
            const auto captured = capture->Latest(mapping.object).Value();
            CHECK(captured->SimulationTick() == 1);
            CHECK(std::get<double>(captured->Fields()[0].value) == 3.0);
            CHECK(std::get<double>(captured->Fields()[1].value) == 4.0);
            CHECK(std::get<double>(captured->Fields()[2].value) == 5.0);

            const auto opened = source.Value()->BeginRead(mapping, 1);
            REQUIRE(opened.HasValue());
            owner.reset();
            CHECK_FALSE(lifetime.expired());
            CHECK(source.Value()->IsCurrent(opened.Value()));
            source.Value()->EndRead(opened.Value());
            CHECK(lifetime.expired());
            REQUIRE(fixture.scene->View().IsCurrent());
            CHECK(source.Value()->BeginRead(mapping, 1).HasError());
            CHECK(source.Value()->CaptureAfterCommit(fixture.lifecycle, *capture, Runtime::RuntimePhase::NetworkFlush).HasError());
        }
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
