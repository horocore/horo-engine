#include "../world_streaming/StreamingCellCandidateTestSupport.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

#if HORO_TEST_PHYSICS_NATIVE
#include "../ai/PerceptionSightTestSupport.h"
#endif

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        namespace W = WorldStreaming;
        using W::CandidateTestSupport::Cell;
        using W::TestSupport::Asset;
        using W::TestSupport::IdentityFrom;
        using W::TestSupport::RequireError;
        using W::TestSupport::World;

        struct Counts final {
            int authoritiesReleased{};
            int prepared{};
            int published{};
            int rolledBack{};
            int notified{};
            bool reject{};
            bool invalidateFence{};
        };

        class Authority final : public SceneCellPayloadAuthority {
        public:
            Authority(SceneCellPayloadIdentity identity, W::StreamingFence fence, Counts &counts)
                : identity(identity), fence(fence), counts(counts) {}

            ~Authority() override {
                ++counts.authoritiesReleased;
            }

            Result<void> ValidatePublication(const SceneCellPayloadIdentity &value, const W::StreamingFence &attempt) const override {
                if (!ready || value != identity || attempt != fence)
                    return Result<void>::Failure(MakeError(SceneCellPayloadErrors::Stale));
                return Result<void>::Success();
            }

            SceneCellPayloadIdentity identity;
            W::StreamingFence fence;
            Counts &counts;
            bool ready{true};
        };

        class Candidate final : public SceneStructuralCandidate {
        public:
            explicit Candidate(Counts &counts) : counts(counts) {}

            ~Candidate() override {
                if (!published)
                    ++counts.rolledBack;
            }

            Result<void> ValidatePublication() const override {
                return counts.reject ? Result<void>::Failure(MakeError(SceneCellPayloadErrors::Unsupported)) : Result<void>::Success();
            }

            void Publish() noexcept override {
                published = true;
                ++counts.published;
            }

            Result<void> AfterPublication() override {
                ++counts.notified;
                return Result<void>::Success();
            }

            Counts &counts;
            bool published{};
        };

        class Participant final : public SceneStructuralParticipant {
        public:
            Participant(Counts &counts, Authority *authority = nullptr) : counts(counts), authority(authority) {}

            SceneStructuralOwner Owner() const noexcept override {
                return SceneStructuralOwner::Gameplay;
            }

            Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(const RuntimeSceneView &active,
                                                                      std::span<const RuntimeEntityView> created,
                                                                      std::span<const EntityRef> destroyed) override {
                REQUIRE(active.IsCurrent());
                for (const auto &entity : created) {
                    REQUIRE(entity.entity.runtime == active.RuntimeId());
                    const auto prior = active.Find(*entity.authoredObject);
                    REQUIRE((!prior || std::ranges::find(destroyed, *prior) != destroyed.end()));
                }
                for (const auto entity : destroyed)
                    REQUIRE(active.Get(entity).HasValue());
                ++counts.prepared;
                if (counts.invalidateFence && authority)
                    authority->ready = false;
                std::unique_ptr<SceneStructuralCandidate> result = std::make_unique<Candidate>(counts);
                return Result<std::unique_ptr<SceneStructuralCandidate>>::Success(std::move(result));
            }

            Counts &counts;
            Authority *authority{};
        };

        RuntimeSceneCellPayload Payload(const int cell = 0, const std::uint64_t revision = 1,
                                        const std::span<const SceneAssetDependency> dependencies = {}, const bool empty = false,
                                        const bool withCamera = false) {
            const auto manifest = W::CandidateTestSupport::Manifest();
            const SceneCellPayloadIdentity identity{World(), Cell(cell), {static_cast<std::uint64_t>(77 + cell)}, {revision}};
            // Child precedes parent deliberately: attachment must resolve complete hierarchy before publication.
            const auto first = static_cast<std::uint64_t>(10 + cell * 10);
            std::array entities{RuntimeEntityDefinition{.object = {first + 1}, .parent = SceneObjectId{first}},
                                RuntimeEntityDefinition{.object = {first}}};
            if (withCamera) {
                entities.back().primitiveMesh = PrimitiveMeshDescriptor::Defaults(PrimitiveMeshType::Box);
                entities.back().components.camera = CameraComponent{};
            }
            auto result = CookRuntimeSceneCellPayload(manifest.Descriptor(),
                                                      {identity,
                                                       empty ? std::span<const RuntimeEntityDefinition>{}
                                                             : std::span<const RuntimeEntityDefinition>{entities},
                                                       dependencies},
                                                      identity, {32, 32, 1024 * 1024});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        void Commit(RuntimeSceneService &service) {
            REQUIRE(service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, FrameContext{1, {}, 0, 0, {}, false, {}}).HasValue());
        }

        void Activate(RuntimeSceneService &service) {
            SceneDefinitionBuilder builder{{1}, {1}};
            builder.Add({.object = {100}});
            REQUIRE(service.QueuePreparation(std::move(builder).Build().Value()).HasValue());
            Commit(service);
            REQUIRE(service.ActiveScene().has_value());
        }

        SceneCellAttachmentRequest Request(const RuntimeSceneService &service, const RuntimeSceneCellPayload &payload) {
            return {service.ActiveScene()->RuntimeId(),
                    {World(), IdentityFrom<W::PartitionEpoch>(1), payload.Identity().cell, IdentityFrom<W::StreamingGeneration>(1)},
                    {},
                    {4, 32, 32},
                    {},
                    {},
                    {}};
        }

        std::shared_ptr<Authority> Owner(const RuntimeSceneCellPayload &payload, const SceneCellAttachmentRequest &request,
                                         Counts &counts) {
            return std::make_shared<Authority>(payload.Identity(), request.fence, counts);
        }

        void Attach(RuntimeSceneService &service, const RuntimeSceneCellPayload &payload, const SceneCellAttachmentRequest &request,
                    const std::shared_ptr<Authority> &authority) {
            REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
            Commit(service);
            REQUIRE_FALSE(service.TakeOperationError().has_value());
        }

        struct StartupObservation final {
            SceneRuntimeId oldRuntime;
            SceneInstanceAddress address;
            SceneCellPayloadIdentity identity;
            W::PartitionEpoch epoch;
            std::optional<RuntimeSceneView> candidate;
            CancellationSource *cancelAtValidation{};
            bool rejectPreparation{};
            int published{};
            int discarded{};
        };

        class StartupActivation final : public SceneActivationCandidate {
        public:
            explicit StartupActivation(StartupObservation *observed) : observed_(observed) {}

            Result<void> ValidatePublication() const override {
                if (observed_ && observed_->cancelAtValidation)
                    observed_->cancelAtValidation->RequestCancellation();
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                if (observed_)
                    ++observed_->published;
                published_ = true;
            }

            void Shutdown() noexcept override {
                if (observed_ && !published_)
                    ++observed_->discarded;
            }

        private:
            StartupObservation *observed_;
            bool published_{};
        };

        class ObserveStartup final : public SceneActivationParticipant {
        public:
            ObserveStartup(RuntimeSceneService &parent, StartupObservation &observed) : parent_(parent), observed_(observed) {}

            Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &,
                                                                      const RuntimeSceneView &candidate) override {
                using Output = Result<std::unique_ptr<SceneActivationCandidate>>;
                if (candidate.RuntimeId() != observed_.address.parent)
                    return Output::Success(std::make_unique<StartupActivation>(nullptr));
                REQUIRE(parent_.ActiveScene());
                CHECK(parent_.ActiveScene()->RuntimeId() == observed_.oldRuntime);
                CHECK_FALSE(parent_.ActiveScene()->Find({10}));
                REQUIRE(FindStreamedSceneInstance(candidate, observed_.address, observed_.identity, observed_.epoch));
                const auto root = candidate.Find({10});
                const auto child = candidate.Find({11});
                REQUIRE(root);
                REQUIRE(child);
                const auto camera = candidate.Get(*root);
                REQUIRE(camera.HasValue());
                REQUIRE(camera.Value().components->camera);
                REQUIRE(*camera.Value().primitiveMesh);
                CHECK(candidate.Get(*child).Value().parent == *root);
                observed_.candidate = candidate;
                if (observed_.rejectPreparation)
                    return Output::Failure(MakeError(SceneCellPayloadErrors::Unsupported));
                return Output::Success(std::make_unique<StartupActivation>(&observed_));
            }

        private:
            RuntimeSceneService &parent_;
            StartupObservation &observed_;
        };
    }  // namespace

    TEST_CASE("Cells attach to one canonical runtime and preserve resident handles", "[scene][cell_attachment]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto root = *service.ActiveScene()->Find({100});
        const auto runtime = service.ActiveScene()->RuntimeId();
        const auto first = Payload();
        const auto firstRequest = Request(service, first);
        Attach(service, first, firstRequest, Owner(first, firstRequest, counts));
        const auto resident = *service.ActiveScene()->Find({10});
        const auto child = service.ActiveScene()->Get(*service.ActiveScene()->Find({11})).Value();
        REQUIRE(child.parent == resident);
        const auto viewBeforeSecond = *service.ActiveScene();
        const auto second = Payload(1);
        const auto request = Request(service, second);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, second, request, {}, Owner(second, request, counts)).HasValue());
        REQUIRE(viewBeforeSecond.IsCurrent());
        REQUIRE_FALSE(service.ActiveScene()->Find({20}).has_value());
        REQUIRE(service.OnPhase(RuntimePhase::VariableUpdate, FrameContext{1, {}, 0, 0, {}, false, {}}).HasValue());
        REQUIRE_FALSE(service.ActiveScene()->Find({20}).has_value());
        Commit(service);
        REQUIRE_FALSE(viewBeforeSecond.IsCurrent());
        REQUIRE(service.ActiveScene()->RuntimeId() == runtime);
        REQUIRE(service.ActiveScene()->DefinitionId() == SceneDefinitionId{1});
        REQUIRE(service.ActiveScene()->Find({100}) == root);
        REQUIRE(service.ActiveScene()->Find({10}) == resident);
        REQUIRE(service.ActiveScene()->Get(resident).HasValue());
        REQUIRE(service.ActiveScene()->BaselineCount() == 2);
        REQUIRE(service.ActiveScene()->FindBaseline(first.Identity().scene)->entities.size() == 2);
        REQUIRE(counts.authoritiesReleased == 2);
    }

    TEST_CASE("Cell replacement and eviction retire only exact ownership", "[scene][cell_attachment][lifecycle]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto first = Payload();
        const auto second = Payload(1);
        const auto initial = Request(service, first);
        const auto otherRequest = Request(service, second);
        Attach(service, first, initial, Owner(first, initial, counts));
        Attach(service, second, otherRequest, Owner(second, otherRequest, counts));
        const auto old = *service.ActiveScene()->Find({10});
        const auto unaffected = *service.ActiveScene()->Find({20});
        const auto replacement = Payload(0, 2);
        auto request = Request(service, replacement);
        request.expectedRevision = {1};
        Attach(service, replacement, request, Owner(replacement, request, counts));
        REQUIRE(service.ActiveScene()->Get(old).HasError());
        REQUIRE(service.ActiveScene()->Find({20}) == unaffected);
        REQUIRE(service.ActiveScene()->FindBaseline(first.Identity().scene)->revision == SceneDefinitionRevision{2});
        REQUIRE(QueueRuntimeSceneCellDetachment(service, first.Identity(), initial, Owner(first, initial, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
        REQUIRE(service.ActiveScene()->BaselineCount() == 2);
        REQUIRE(QueueRuntimeSceneCellDetachment(service, replacement.Identity(), request, Owner(replacement, request, counts)).HasValue());
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError().has_value());
        REQUIRE_FALSE(service.ActiveScene()->Find({10}).has_value());
        REQUIRE(service.ActiveScene()->Find({20}) == unaffected);
        REQUIRE(service.ActiveScene()->BaselineCount() == 1);
    }

    TEST_CASE("Perception retires listeners and sources when their Scene cell unloads", "[scene][cell_attachment][lifecycle][ai]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto cell = Payload();
        const auto request = Request(service, cell);
        Attach(service, cell, request, Owner(cell, request, counts));

        AI::PerceptionSpatialBroadphase perception;
        const auto sense = AI::SenseTypeIds::Sight;
        const auto root = *service.ActiveScene()->Find({100});
        const AI::PerceptionSpatialListener resident{.entity = root, .senses = {sense}, .senseCount = 1};
        const auto publishCell = [&](const std::uint64_t revision) {
            const auto view = *service.ActiveScene();
            const auto listener = view.Find({10});
            const auto source = view.Find({11});
            REQUIRE(listener);
            REQUIRE(source);
            const std::array listeners{resident, AI::PerceptionSpatialListener{.entity = *listener, .senses = {sense}, .senseCount = 1}};
            const std::array sources{AI::PerceptionSpatialSource{.entity = *source, .layers = 1, .senses = {sense}, .senseCount = 1}};
            const auto published = perception.Publish(view, revision, listeners, sources);
            REQUIRE(published.HasValue());
            return published.Value();
        };
        const AI::PerceptionSpatialQuery query{.listener = root, .sense = sense, .radiusMillimeters = 1000, .visibleLayers = 1};
        const auto before = publishCell(1);
        const auto oldListener = *service.ActiveScene()->Find({10});
        const auto oldSource = *service.ActiveScene()->Find({11});
        REQUIRE(before->ListenerCount() == 2);
        REQUIRE(before->Allows(oldListener, oldSource, sense));
        const auto pending = before->Query(query);
        REQUIRE(pending.HasValue());
        REQUIRE(pending.Value().count == 1);
        REQUIRE(pending.Value().candidates[0].entity == oldSource);

        REQUIRE(QueueRuntimeSceneCellDetachment(service, cell.Identity(), request, Owner(cell, request, counts)).HasValue());
        REQUIRE(before->ReadAuthorization()->IsCurrent());
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError().has_value());
        const auto unloaded = *service.ActiveScene();
        REQUIRE_FALSE(FindRuntimeSceneCellAttachment(unloaded, cell.Identity(), request.fence.epoch));
        REQUIRE(unloaded.Get(oldListener).HasError());
        REQUIRE(unloaded.Get(oldSource).HasError());
        REQUIRE(perception.Publish(unloaded, 2, std::span{&resident, 1}, {}).HasValue());
        const auto current = perception.Current();
        CHECK(current->ListenerCount() == 1);
        CHECK(current->SourceCount() == 0);
        CHECK(current->ListenerPosition(oldListener).HasError());
        CHECK(current->Query(query).Value().count == 0);
        CHECK_FALSE(before->ReadAuthorization()->IsCurrent());
        CHECK(before->Query(query).HasError());

        auto reload = request;
        reload.fence.generation = IdentityFrom<W::StreamingGeneration>(2);
        Attach(service, cell, reload, Owner(cell, reload, counts));
        const auto reloaded = *service.ActiveScene();
        const auto newListener = *reloaded.Find({10});
        const auto newSource = *reloaded.Find({11});
        CHECK(newListener != oldListener);
        CHECK(newSource != oldSource);
        CHECK(reloaded.Get(pending.Value().candidates[0].entity).HasError());
        const auto after = publishCell(3);
        CHECK(after->ListenerCount() == 2);
        CHECK(after->SourceCount() == 1);
        CHECK_FALSE(after->Allows(oldListener, newSource, sense));
        CHECK_FALSE(after->Allows(newListener, oldSource, sense));
        CHECK(after->Allows(newListener, newSource, sense));
        const auto reacquired = after->Query(query);
        REQUIRE(reacquired.HasValue());
        REQUIRE(reacquired.Value().count == 1);
        CHECK(reacquired.Value().candidates[0].entity == newSource);
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Cell unload invalidates queued sight rays before the source is reattached",
              "[scene][cell_attachment][lifecycle][ai][sight]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto cell = Payload();
        const auto request = Request(service, cell);
        Attach(service, cell, request, Owner(cell, request, counts));

        const auto initialScene = *service.ActiveScene();
        const auto root = initialScene.Find({100});
        const auto oldSource = initialScene.Find({11});
        REQUIRE(root);
        REQUIRE(oldSource);
        const auto sense = AI::SenseTypeIds::Sight;
        const AI::PerceptionSpatialListener listener{.entity = *root, .senses = {sense}, .senseCount = 1};
        AI::PerceptionSpatialBroadphase broadphase;
        AI::SightTest::SightWorld physics;
        AI::PerceptionSight sight;
        const auto queueSight = [&](const EntityRef source, const std::uint64_t revision) {
            const auto scene = *service.ActiveScene();
            const AI::PerceptionSpatialSource candidate{.entity = source,
                                                        .position = Math::WorldCoordinate64::FromMillimeters(0, 0, -10'000),
                                                        .layers = 1,
                                                        .senses = {sense},
                                                        .senseCount = 1};
            const auto spatial = broadphase.Publish(scene, revision, std::span{&listener, 1}, std::span{&candidate, 1});
            REQUIRE(spatial.HasValue());
            const auto frame = physics.CurrentFrame(*root);
            AI::PerceptionSightBudget budget{.simulationTick = physics.tick, .remainingRaycasts = 1};
            const auto queued = sight.Begin(scene, *spatial.Value(), physics.capability, frame, {}, budget);
            REQUIRE(queued.HasValue());
            REQUIRE(queued.Value().pending);
            REQUIRE(budget.remainingRaycasts == 0);
            return AI::SightTest::Publication(physics.capability, frame);
        };
        const auto oldPublication = queueSight(*oldSource, 1);

        REQUIRE(QueueRuntimeSceneCellDetachment(service, cell.Identity(), request, Owner(cell, request, counts)).HasValue());
        Commit(service);
        REQUIRE_FALSE(service.TakeOperationError());
        const auto cancelled = sight.Poll(*service.ActiveScene(), 1, oldPublication);
        REQUIRE(cancelled.HasValue());
        CHECK_FALSE(cancelled.Value().pending);
        REQUIRE(cancelled.Value().count == 1);
        CHECK(cancelled.Value().observations[0].source == *oldSource);
        CHECK(cancelled.Value().observations[0].evidence == AI::PerceptionSightEvidence::Stale);
        CHECK_FALSE(cancelled.Value().observations[0].visible);
        REQUIRE(physics.world->ProcessQueryBatch().HasValue());

        auto reload = request;
        reload.fence.generation = IdentityFrom<W::StreamingGeneration>(2);
        Attach(service, cell, reload, Owner(cell, reload, counts));
        const auto newSource = service.ActiveScene()->Find({11});
        REQUIRE(newSource);
        REQUIRE(newSource != oldSource);
        physics.Advance();
        const auto newPublication = queueSight(*newSource, 2);
        REQUIRE(physics.world->ProcessQueryBatch().HasValue());
        const auto reacquired = sight.Poll(*service.ActiveScene(), 2, newPublication);
        REQUIRE(reacquired.HasValue());
        CHECK_FALSE(reacquired.Value().pending);
        REQUIRE(reacquired.Value().count == 1);
        CHECK(reacquired.Value().observations[0].source == *newSource);
        CHECK(reacquired.Value().observations[0].visible);
    }
#endif

    TEST_CASE("Cell cancellation and stale provider barrier preserve canonical storage", "[scene][cell_attachment][rollback]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        auto request = Request(service, payload);
        CancellationSource cancellation;
        request.cancellation = cancellation.Token();
        auto authority = Owner(payload, request, counts);
        auto view = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        authority.reset();
        cancellation.RequestCancellation();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.cancelled");
        REQUIRE(view.IsCurrent());
        REQUIRE(service.ActiveScene()->BaselineCount() == 0);
        REQUIRE(counts.authoritiesReleased == 1);
        request.cancellation = {};
        authority = Owner(payload, request, counts);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        authority->ready = false;
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(view.IsCurrent());
    }

    TEST_CASE("Cell explicit cancellation is fenced and allows a replacement request", "[scene][cell_attachment][cancel]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, Owner(payload, request, counts)).HasValue());
        auto wrongIdentity = payload.Identity();
        wrongIdentity.scene = {999};
        REQUIRE(CancelRuntimeSceneCellOperation(service, wrongIdentity, request).HasError());
        auto wrongRequest = request;
        wrongRequest.runtime = {999};
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), wrongRequest).HasError());
        wrongRequest = request;
        wrongRequest.fence.generation = IdentityFrom<W::StreamingGeneration>(2);
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), wrongRequest).HasError());
        wrongIdentity = payload.Identity();
        wrongIdentity.revision = {2};
        REQUIRE(CancelRuntimeSceneCellOperation(service, wrongIdentity, request).HasError());
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), request).HasValue());
        REQUIRE(counts.authoritiesReleased == 1);
        Attach(service, payload, request, Owner(payload, request, counts));
        REQUIRE(CancelRuntimeSceneCellOperation(service, payload.Identity(), request).HasError());
        REQUIRE(service.ActiveScene()->BaselineCount() == 1);
    }

    TEST_CASE("Cell aggregate capacity includes empty cells and resident entities", "[scene][cell_attachment][capacity]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto first = Payload();
        const auto request = Request(service, first);
        Attach(service, first, request, Owner(first, request, counts));
        const auto second = Payload(1);
        auto secondRequest = Request(service, second);
        secondRequest.limits.maximumEntities = 3;
        const auto oldView = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, second, secondRequest, {}, Owner(second, secondRequest, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.capacity_exceeded");
        REQUIRE(oldView.IsCurrent());
        const auto empty = Payload(1, 1, {}, true);
        auto emptyRequest = Request(service, empty);
        emptyRequest.limits.maximumAttachments = 1;
        REQUIRE(QueueRuntimeSceneCellAttachment(service, empty, emptyRequest, {}, Owner(empty, emptyRequest, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.capacity_exceeded");
        emptyRequest.limits.maximumAttachments = 2;
        Attach(service, empty, emptyRequest, Owner(empty, emptyRequest, counts));
        REQUIRE(service.ActiveScene()->FindBaseline(empty.Identity().scene)->entities.empty());
        REQUIRE(service.ActiveScene()->BaselineCount() == 2);
    }

    TEST_CASE("Cell required owners and final publication predicate reject atomically", "[scene][cell_attachment][provider]") {
        Counts counts;
        RuntimeSceneService service;
        const auto payload = Payload();
        REQUIRE(service.AddStructuralParticipant(std::make_unique<Participant>(counts)).HasValue());
        Activate(service);
        const auto request = Request(service, payload);
        auto authority = Owner(payload, request, counts);
        counts.reject = true;
        const auto before = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.unsupported");
        REQUIRE(counts.prepared == 1);
        REQUIRE(counts.rolledBack == 1);
        REQUIRE(counts.published == 0);
        REQUIRE(before.IsCurrent());
        counts.reject = false;
        Attach(service, payload, request, authority);
        REQUIRE(counts.published == 1);
        REQUIRE(counts.notified == 1);
    }

    TEST_CASE("Cell fence is sampled again after fallible provider preparation", "[scene][cell_attachment][provider][stale]") {
        Counts counts;
        RuntimeSceneService service;
        const auto payload = Payload();
        SceneCellAttachmentRequest request{{1},
                                           {World(), IdentityFrom<W::PartitionEpoch>(1), Cell(), IdentityFrom<W::StreamingGeneration>(1)},
                                           {},
                                           {4, 32, 32}};
        auto authority = Owner(payload, request, counts);
        REQUIRE(service.AddStructuralParticipant(std::make_unique<Participant>(counts, authority.get())).HasValue());
        Activate(service);
        request.runtime = service.ActiveScene()->RuntimeId();
        counts.invalidateFence = true;
        const auto before = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.cell_payload.stale");
        REQUIRE(counts.published == 0);
        REQUIRE(counts.rolledBack == 1);
        REQUIRE(before.IsCurrent());
    }

    TEST_CASE("Cell entities cannot escape their baseline retirement owner", "[scene][cell_attachment][ownership]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        Attach(service, payload, request, Owner(payload, request, counts));
        const auto child = *service.ActiveScene()->Find({11});
        SceneCommandBuffer commands;
        commands.Destroy(child);
        REQUIRE(service.QueueStructuralCommands(std::move(commands)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.invalid");
        REQUIRE(service.ActiveScene()->Get(child).HasValue());
        SceneCommandBuffer external;
        static_cast<void>(external.Create({.parent = child, .authoredObject = SceneObjectId{999}}));
        REQUIRE(service.QueueStructuralCommands(std::move(external)).HasValue());
        Commit(service);
        REQUIRE(QueueRuntimeSceneCellDetachment(service, payload.Identity(), request, Owner(payload, request, counts)).HasValue());
        const auto before = *service.ActiveScene();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.invalid");
        REQUIRE(before.IsCurrent());
        REQUIRE(service.ActiveScene()->Get(child).HasValue());
    }

    TEST_CASE("Cell pending and resident ownership retires on world replacement and shutdown", "[scene][cell_attachment][shutdown]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        Attach(service, payload, request, Owner(payload, request, counts));
        const auto old = *service.ActiveScene()->Find({10});
        Activate(service);
        REQUIRE(service.ActiveScene()->Get(old).HasError());
        REQUIRE(service.ActiveScene()->BaselineCount() == 0);
        auto nextRequest = Request(service, payload);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, nextRequest, {}, Owner(payload, nextRequest, counts)).HasValue());
        service.Shutdown();
        service.Shutdown();
        REQUIRE(counts.authoritiesReleased == 2);
        REQUIRE_FALSE(service.ActiveScene().has_value());
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, nextRequest, {}, Owner(payload, nextRequest, counts)).HasError());
    }

    TEST_CASE("Cell invalid runtime identity limits and duplicate attachment preserve residents", "[scene][cell_attachment][invalid]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        auto request = Request(service, payload);
        auto authority = Owner(payload, request, counts);
        const auto before = *service.ActiveScene();
        request.runtime = {999};
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).ErrorValue().code.Value() ==
                "scene.cell_payload.stale");
        request = Request(service, payload);
        request.fence.epoch = IdentityFrom<W::PartitionEpoch>(2);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).ErrorValue().code.Value() ==
                "scene.cell_payload.stale");
        request = Request(service, payload);
        request.limits.maximumAttachments = 0;
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasError());
        request = Request(service, payload);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, {}).HasError());
        REQUIRE(before.IsCurrent());
        Attach(service, payload, request, authority);
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, authority).HasValue());
        const auto published = *service.ActiveScene();
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
        REQUIRE(published.IsCurrent());
        REQUIRE(service.ActiveScene()->BaselineCount() == 1);
    }

    TEST_CASE("Cell required Physics admission rejects a missing composed owner", "[scene][cell_attachment][unsupported]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto manifest = W::CandidateTestSupport::Manifest();
        const SceneCellPayloadIdentity identity{World(), Cell(), {77}, {1}};
        RuntimeEntityDefinition entity{.object = {10}};
        entity.components.rigidBody =
            RigidBodyComponent{.id = {10}, .body = {100}, .motion = AuthoredPhysicsMotionType::Static, .mass = AuthoredPhysicsNoMass{}};
        // A valid authored body needs an explicit collider before owner admission can be exercised.
        entity.components.colliders.push_back({.id = {11},
                                               .collider = {1},
                                               .body = {{10}, {100}},
                                               .collisionProfile = Physics::CollisionProfileId::FromBytes({1}),
                                               .materials = {{Physics::PhysicsMaterialSlotId::FromValue(1), Asset(1)}}});
        const std::array entities{entity};
        auto cooked = CookRuntimeSceneCellPayload(manifest.Descriptor(), {identity, entities}, identity, {8, 8, 1024 * 1024});
        INFO((cooked.HasError() ? cooked.ErrorValue().message : ""));
        REQUIRE(cooked.HasValue());
        const auto payload = std::move(cooked).Value();
        const auto request = Request(service, payload);
        const auto before = *service.ActiveScene();
        REQUIRE(QueueRuntimeSceneCellAttachment(service, payload, request, {}, Owner(payload, request, counts)).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.unsupported");
        REQUIRE(before.IsCurrent());
        REQUIRE(service.ActiveScene()->BaselineCount() == 0);
        REQUIRE_FALSE(service.ActiveScene()->Find({10}).has_value());
    }

    TEST_CASE("Cell resident identity rejects foreign partition cell and mounted epoch owners", "[scene][cell_attachment][identity]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        const auto request = Request(service, payload);
        Attach(service, payload, request, Owner(payload, request, counts));
        const auto resident = *service.ActiveScene()->Find({10});
        REQUIRE(FindRuntimeSceneCellAttachment(*service.ActiveScene(), payload.Identity(), request.fence.epoch).has_value());
        for (int variant = 0; variant < 3; ++variant) {
            auto identity = payload.Identity();
            auto foreign = request;
            if (variant == 0) {
                identity.partition = World(2);
                foreign.fence.partition = identity.partition;
            } else if (variant == 1) {
                identity.cell = Cell(1);
                foreign.fence.cell = identity.cell;
            } else {
                foreign.fence.epoch = IdentityFrom<W::PartitionEpoch>(2);
            }
            auto validForeign = std::make_shared<Authority>(identity, foreign.fence, counts);
            REQUIRE(validForeign->ValidatePublication(identity, foreign.fence).HasValue());
            REQUIRE_FALSE(FindRuntimeSceneCellAttachment(*service.ActiveScene(), identity, foreign.fence.epoch).has_value());
            REQUIRE(QueueRuntimeSceneCellDetachment(service, identity, foreign, validForeign).HasValue());
            const auto before = *service.ActiveScene();
            Commit(service);
            REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
            REQUIRE(before.IsCurrent());
            REQUIRE(service.ActiveScene()->Find({10}) == resident);
            REQUIRE(service.ActiveScene()->BaselineCount() == 1);
            identity.revision = {2};
            const auto manifest = W::CandidateTestSupport::Manifest();
            const auto &source = manifest.Descriptor();
            auto partition = W::WorldPartitionDescriptor::Create({}, identity.partition, source.Bounds(), source.Grid(), source.Layers(),
                                                                 source.Cells(), {4, 8, 1024})
                                 .Value();
            const std::array entities{RuntimeEntityDefinition{.object = {10}}};
            auto replacement = CookRuntimeSceneCellPayload(partition, {identity, entities}, identity, {8, 8, 1024 * 1024}).Value();
            foreign.expectedRevision = {1};
            validForeign = std::make_shared<Authority>(identity, foreign.fence, counts);
            REQUIRE(QueueRuntimeSceneCellAttachment(service, replacement, foreign, {}, validForeign).HasValue());
            Commit(service);
            REQUIRE(service.TakeOperationError()->code.Value() == "scene.baseline.stale");
            REQUIRE(before.IsCurrent());
            REQUIRE(service.ActiveScene()->Find({10}) == resident);
        }
    }

    TEST_CASE("Streamed instances publish and replace through the exact parent cell ownership", "[scene][cell_attachment][instances]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto first = Payload();
        const auto request = Request(service, first);
        const SceneInstanceAddress address{request.runtime, {2}, first.Identity().scene, SceneInstanceMode::Streamed};
        const auto authority = Owner(first, request, counts);
        const auto queued = QueueStreamedSceneInstance(service, address, first, request, {}, authority);
        REQUIRE(queued.HasValue());
        CHECK_FALSE(queued.Value().Receipt()->Complete());
        CHECK_FALSE(CancelRuntimeSceneCellOperation(service, first.Identity(), request).HasValue());
        CHECK_FALSE(FindStreamedSceneInstance(*service.ActiveScene(), address, first.Identity(), request.fence.epoch));
        Commit(service);
        REQUIRE(queued.Value().Receipt()->ResultValue());
        const auto view = service.ActiveScene();
        REQUIRE(FindStreamedSceneInstance(*view, address, first.Identity(), request.fence.epoch));
        REQUIRE(FindRuntimeSceneCellAttachment(*view, first.Identity(), request.fence.epoch));
        const auto child = view->Find({10});
        REQUIRE(child);
        CHECK(child->runtime == address.parent);

        auto foreign = address;
        foreign.instance = {3};
        CHECK_FALSE(FindStreamedSceneInstance(*view, foreign, first.Identity(), request.fence.epoch));
        CHECK_FALSE(FindStreamedSceneInstance(*view, address, first.Identity(), IdentityFrom<W::PartitionEpoch>(2)));
        const auto second = Payload(0, 2);
        auto next = Request(service, second);
        next.expectedRevision = first.Identity().revision;
        next.fence.generation = IdentityFrom<W::StreamingGeneration>(2);
        const auto ready = Owner(second, next, counts);
        CHECK(QueueStreamedSceneInstance(service, foreign, second, next, {}, ready, &queued.Value()).HasError());
        CHECK(QueueStreamedSceneInstance(service, address, second, next, {}, ready).HasError());
        const auto replacement = QueueStreamedSceneInstance(service, address, second, next, {}, ready, &queued.Value());
        REQUIRE(replacement.HasValue());
        Commit(service);
        REQUIRE(replacement.Value().Receipt()->ResultValue());
        CHECK_FALSE(service.ActiveScene()->Get(*child).HasValue());
        CHECK_FALSE(FindStreamedSceneInstance(*service.ActiveScene(), address, first.Identity(), request.fence.epoch));
        REQUIRE(FindStreamedSceneInstance(*service.ActiveScene(), address, second.Identity(), request.fence.epoch));
        CHECK(QueueStreamedSceneRemoval(service, queued.Value(), request, authority).HasError());

        REQUIRE(QueueRuntimeSceneCellDetachment(service, second.Identity(), next, ready).HasValue());
        Commit(service);
        REQUIRE(service.TakeOperationError());
        REQUIRE(FindStreamedSceneInstance(*service.ActiveScene(), address, second.Identity(), request.fence.epoch));
        const auto removal = QueueStreamedSceneRemoval(service, replacement.Value(), next, ready);
        REQUIRE(removal.HasValue());
        Commit(service);
        REQUIRE(removal.Value().Receipt()->ResultValue());
        CHECK_FALSE(service.ActiveScene()->Find({10}));
        CHECK(service.ActiveScene()->Find({100}));
    }

    TEST_CASE("Streamed operations cannot outlive the parent or reuse a cancelled attempt", "[scene][cell_attachment][instances]") {
        Counts counts;
        RuntimeSceneService service;
        Activate(service);
        const auto payload = Payload();
        auto request = Request(service, payload);
        const SceneInstanceAddress address{request.runtime, {2}, payload.Identity().scene, SceneInstanceMode::Streamed};
        const auto authority = Owner(payload, request, counts);
        auto unsupported = address;
        unsupported.mode = SceneInstanceMode::Activated;
        CHECK(QueueStreamedSceneInstance(service, unsupported, payload, request, {}, authority).HasError());

        auto tooSmall = request;
        tooSmall.limits.maximumEntities = 1;
        REQUIRE(QueueStreamedSceneInstance(service, address, payload, tooSmall, {}, authority).HasError());
        const auto cancelled = QueueStreamedSceneInstance(service, address, payload, request, {}, authority);
        REQUIRE(cancelled.HasValue());
        REQUIRE(cancelled.Value().Cancel(service).HasValue());
        CHECK(cancelled.Value().Receipt()->Failure());
        const auto newer = QueueStreamedSceneInstance(service, address, payload, request, {}, authority);
        REQUIRE(newer.HasValue());
        CHECK(cancelled.Value().Cancel(service).HasError());
        authority->ready = false;
        Commit(service);
        REQUIRE(newer.Value().Receipt()->Failure());
        CHECK_FALSE(service.ActiveScene()->Find({10}));

        authority->ready = true;
        CancellationSource parentLifetime;
        request.ownerCancellation = parentLifetime.Token();
        const auto stopping = QueueStreamedSceneInstance(service, address, payload, request, {}, authority);
        REQUIRE(stopping.HasValue());
        parentLifetime.RequestCancellation();
        Commit(service);
        const auto *rejection = stopping.Value().Receipt()->Failure();
        REQUIRE(rejection);
        CHECK(rejection->code.Value() == SceneCellPayloadErrors::Cancelled.code.Value());
        CHECK_FALSE(service.ActiveScene()->Find({10}));

        request.ownerCancellation = {};
        const auto published = QueueStreamedSceneInstance(service, address, payload, request, {}, authority);
        REQUIRE(published.HasValue());
        Commit(service);
        REQUIRE(published.Value().Receipt()->ResultValue());
        Activate(service);
        CHECK(service.ActiveScene()->RuntimeId() != address.parent);
        CHECK_FALSE(service.ActiveScene()->Find({10}));
        CHECK(QueueStreamedSceneInstance(service, address, payload, request, {}, authority).HasError());
        CHECK(QueueStreamedSceneRemoval(service, published.Value(), request, authority).HasError());

        request = Request(service, payload);
        auto successor = address;
        successor.parent = request.runtime;
        const auto pending = QueueStreamedSceneInstance(service, successor, payload, request, {}, authority);
        REQUIRE(pending.HasValue());
        service.Shutdown();
        REQUIRE(pending.Value().Receipt()->Failure());
        CHECK_FALSE(service.ActiveScene());
    }

    TEST_CASE("Startup Streamed baseline enters the private root before Scene activation participants",
              "[scene][cell_attachment][initial_streamed]") {
        Counts counts;
        StartupObservation observed{.oldRuntime = {1}};
        CancellationSource ownerLifetime;
        RuntimeSceneService service;
        REQUIRE(service.AddActivationParticipant(std::make_unique<ObserveStartup>(service, observed)).HasValue());
        Activate(service);
        const auto old = *service.ActiveScene();
        const auto payload = Payload(0, 1, {}, false, true);
        auto request = Request(service, payload);
        request.runtime = service.NextRuntimeId();
        request.ownerCancellation = ownerLifetime.Token();
        observed.address = {request.runtime, {2}, payload.Identity().scene, SceneInstanceMode::Streamed};
        observed.identity = payload.Identity();
        observed.epoch = request.fence.epoch;
        const auto parent = std::move(SceneDefinitionBuilder{{2}, {1}}).Build().Value();
        auto authority = Owner(payload, request, counts);

        SECTION("Early prepare rejection preserves the old root and does not consume its successor") {
            observed.rejectPreparation = true;
            const auto result = QueuePreparationWithInitialStreamedScene(service, parent, observed.address, payload, request, authority);
            RequireError(result, SceneCellPayloadErrors::Unsupported);
            REQUIRE(observed.candidate);
            CHECK_FALSE(observed.candidate->IsCurrent());
            CHECK(service.NextRuntimeId() == request.runtime);
            REQUIRE(service.ActiveScene());
            CHECK(service.ActiveScene()->SameIncarnation(old));
            CHECK(observed.published == 0);
            return;
        }
        SECTION("Live runtime and noninitial revisions cannot be used for startup") {
            auto foreign = observed.address;
            foreign.parent = old.RuntimeId();
            auto liveRequest = request;
            liveRequest.runtime = old.RuntimeId();
            RequireError(QueuePreparationWithInitialStreamedScene(service, parent, foreign, payload, liveRequest, authority),
                         SceneCellPayloadErrors::Stale);
            auto replacement = request;
            replacement.expectedRevision = {1};
            RequireError(QueuePreparationWithInitialStreamedScene(service, parent, observed.address, payload, replacement, authority),
                         SceneCellPayloadErrors::Stale);
            auto tooSmall = request;
            tooSmall.limits.maximumEntities = 1;
            RequireError(QueuePreparationWithInitialStreamedScene(service, parent, observed.address, payload, tooSmall, authority),
                         SceneCellPayloadErrors::CapacityExceeded);
            CHECK_FALSE(observed.candidate);
            CHECK(service.ActiveScene()->SameIncarnation(old));
            return;
        }

        const auto queued = QueuePreparationWithInitialStreamedScene(service, parent, observed.address, payload, request, authority);
        REQUIRE(queued.HasValue());
        REQUIRE(observed.candidate);
        CHECK(queued.Value().Snapshot().Value().status == ScenePublicationStatus::Pending);
        CHECK(service.ActiveScene()->SameIncarnation(old));
        CHECK_FALSE(service.ActiveScene()->Find({10}));
        std::weak_ptr<Authority> retained = authority;

        SECTION("Cutoff publishes both root and cell with the original prepared candidate") {
            authority.reset();
            CHECK_FALSE(retained.expired());
            Commit(service);
            const auto current = service.ActiveScene();
            REQUIRE(current);
            CHECK_FALSE(old.IsCurrent());
            CHECK(current->SameIncarnation(*observed.candidate));
            REQUIRE(FindStreamedSceneInstance(*current, observed.address, observed.identity, observed.epoch));
            const auto receipt = queued.Value().Snapshot().Value();
            CHECK(receipt.status == ScenePublicationStatus::Published);
            CHECK(receipt.scene == observed.address.parent);
            CHECK(receipt.structuralRevision == current->StructuralRevision());
            CHECK(retained.expired());
            CHECK(observed.published == 1);
            CHECK(observed.discarded == 0);
            return;
        }
        SECTION("Owner cancellation during final candidate validation suppresses aggregate publication") {
            observed.cancelAtValidation = &ownerLifetime;
            authority.reset();
            Commit(service);
            const auto error = service.TakeOperationError();
            REQUIRE(error);
            CHECK(error->code.Value() == SceneCellPayloadErrors::Cancelled.code.Value());
        }
        SECTION("Streaming authority revoked before cutoff suppresses aggregate publication") {
            authority->ready = false;
            authority.reset();
            Commit(service);
            const auto error = service.TakeOperationError();
            REQUIRE(error);
            CHECK(error->code.Value() == SceneCellPayloadErrors::Stale.code.Value());
        }
        const auto rejected = queued.Value().Snapshot().Value();
        CHECK(rejected.status == ScenePublicationStatus::Rejected);
        CHECK_FALSE(rejected.scene.IsValid());
        CHECK_FALSE(observed.candidate->IsCurrent());
        CHECK(service.ActiveScene()->SameIncarnation(old));
        CHECK_FALSE(service.ActiveScene()->Find({10}));
        CHECK(retained.expired());
        CHECK(observed.published == 0);
        CHECK(observed.discarded == 1);
    }
}  // namespace Horo::Runtime
