#include "AllocationProbe.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "RuntimeSceneTestSupport.h"
#include "TriggerVolumeCellTestValues.h"

#include <algorithm>
#include <array>
#include <catch2/catch_message.hpp>
#include <chrono>
#include <functional>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>

namespace Horo::Runtime {
    namespace {
        template <typename T> T Take(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "success"));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        Assets::AssetId Asset(std::uint8_t id) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = id;
            return Assets::AssetId::FromBytes(bytes);
        }

        SceneAssetDependency Dependency(std::uint8_t id, std::string_view type = "core.mesh") {
            return {Asset(id), Take(Assets::AssetTypeId::Parse(type))};
        }

        RuntimeSceneDefinition Definition(std::uint64_t revision) {
            SceneDefinitionBuilder builder{{17}, {revision}};
            RuntimeEntityDefinition root;
            root.object = {1};
            root.localTransform.translation = {3, 4, 5};
            root.primitiveMesh = PrimitiveMeshDescriptor{};
            root.components.camera = CameraComponent{};
            builder.Add(root);
            builder.Add(RuntimeSceneTestSupport::Entity(2, 1));
            REQUIRE(builder.RequireAsset(Dependency(1)).HasValue());
            return Take(std::move(builder).Build());
        }

        struct Observations final {
            std::array<std::size_t, 4> calls{};
            std::size_t prepares{};
            std::size_t publications{};
            std::size_t lastMaximum{};
            std::optional<RuntimeSceneDefinition> prepared;
            std::function<Result<std::vector<SceneAssetDependency>>(std::uint8_t)> discover;
        };

        class Candidate final : public SceneActivationCandidate {
        public:
            explicit Candidate(Observations &observed) : observed_(observed) {}

            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                ++observed_.publications;
            }

            void Shutdown() noexcept override {}

        private:
            Observations &observed_;
        };

        class Participant final : public SceneActivationParticipant {
        public:
            explicit Participant(Observations &observed) : observed_(observed) {}

            Result<std::vector<SceneAssetDependency>> DiscoverDependencies(const RuntimeSceneAssetView &asset, const CancellationToken &,
                                                                           std::size_t maximum) override {
                const auto id = asset.id.Bytes().back();
                if (id >= observed_.calls.size() || !std::ranges::equal(asset.bytes, bytes_))
                    return Result<std::vector<SceneAssetDependency>>::Failure({ErrorCode{"test.discovery.invalid"},
                                                                               ErrorDomainId{"test.discovery"},
                                                                               ErrorSeverity::Error,
                                                                               "Invalid fixture input.",
                                                                               {}});
                ++observed_.calls[id];
                observed_.lastMaximum = maximum;
                if (observed_.discover)
                    return observed_.discover(id);
                return Result<std::vector<SceneAssetDependency>>::Success({});
            }

            Result<std::unique_ptr<SceneActivationCandidate>> Prepare(const RuntimeSceneDefinition &definition,
                                                                      const RuntimeSceneView &) override {
                ++observed_.prepares;
                observed_.prepared = definition;
                return Result<std::unique_ptr<SceneActivationCandidate>>::Success(std::make_unique<Candidate>(observed_));
            }

        private:
            Observations &observed_;
            const std::array<std::uint8_t, 3> bytes_{1, 2, 3};
        };

        class DiscoveryPublicationCheck final : public ScenePublicationCheck {
        public:
            Result<void> ValidatePublication() const override {
                return Result<void>::Success();
            }
        };

        struct Fixture final {
            Observations observed;
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider provider;
            JobSystem jobs{{1, 8}};
            Assets::AssetLoadService loads{jobs, provider};
            RuntimeSceneService service;
            CancellationSource cancellation;

            explicit Fixture(RuntimeSceneAssetLimits limits = {}) : service(registry, loads, limits) {
                std::vector<Assets::AssetRecord> records;
                for (std::uint8_t id = 1; id <= 3; ++id) {
                    const auto path = "assets/" + std::to_string(id) + ".bin";
                    records.push_back(
                        {Asset(id), Dependency(id).expectedType, Take(ProjectPath::Parse(path)), Take(ProjectPath::Parse(path + ".horo"))});
                    provider.Insert(Asset(id), {1, 2, 3});
                }
                REQUIRE(registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
                REQUIRE(service.AddActivationParticipant(std::make_unique<Participant>(observed)).HasValue());
            }

            ~Fixture() {
                service.Shutdown();
                loads.Shutdown();
                jobs.Shutdown(ShutdownPolicy::Drain);
            }

            void Start() {
                REQUIRE(service.Startup(cancellation.Token()).HasValue());
            }

            std::optional<Error> Pump(std::uint64_t revision) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline) {
                    auto advanced = service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges,
                                                    RuntimeSceneTestSupport::Context(cancellation.Token()));
                    if (advanced.HasError())
                        return std::move(advanced).ErrorValue();
                    if (auto error = service.TakeOperationError())
                        return error;
                    const auto active = service.ActiveScene();
                    if (active && active->DefinitionRevision() == SceneDefinitionRevision{revision})
                        return {};
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                FAIL("Scene dependency discovery did not terminate");
                return {};
            }

            ScenePublicationStatus PumpReceipt(const ScenePublicationReceipt &receipt) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto advanced = service.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges,
                                                          RuntimeSceneTestSupport::Context(cancellation.Token()));
                    const auto observed = Take(receipt.Snapshot());
                    if (observed.status != ScenePublicationStatus::Pending)
                        return observed.status;
                    REQUIRE(advanced.HasValue());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                FAIL("Scene publication receipt remained pending");
                return ScenePublicationStatus::Pending;
            }
        };

        void CheckPreserved(const RuntimeSceneDefinition &actual, const RuntimeSceneDefinition &original) {
            CHECK(actual.Id() == original.Id());
            CHECK(actual.Revision() == original.Revision());
            REQUIRE(actual.Entities().size() == original.Entities().size());
            for (std::size_t i = 0; i < actual.Entities().size(); ++i) {
                const auto &a = actual.Entities()[i];
                const auto &b = original.Entities()[i];
                CHECK(a.object == b.object);
                CHECK(a.parent == b.parent);
                CHECK(a.localTransform == b.localTransform);
                CHECK(a.primitiveMesh == b.primitiveMesh);
                CHECK(a.components == b.components);
            }
            CHECK(std::ranges::equal(actual.TriggerVolumeEvidence(), original.TriggerVolumeEvidence()));
        }
    }  // namespace

    TEST_CASE("Scene cold discovery completes duplicate and cyclic closure before canonical activation", "[scene][assets][discovery]") {
        RuntimeSceneAssetLimits exact;
        exact.maximumDependencyReferences = 3;
        Fixture fixture(exact);
        fixture.observed.discover = [](std::uint8_t id) {
            return Result<std::vector<SceneAssetDependency>>::Success(id == 1 ? std::vector{Dependency(2), Dependency(2)}
                                                                              : std::vector{Dependency(1)});
        };
        fixture.Start();
        const auto original = Definition(1);
        REQUIRE(fixture.service.QueuePreparation(original).HasValue());
        const auto failure = fixture.Pump(1);
        INFO((failure ? failure->code.Value() + ": " + failure->message : "success"));
        REQUIRE_FALSE(failure);
        REQUIRE(fixture.observed.prepared);
        CheckPreserved(*fixture.observed.prepared, original);
        CHECK(fixture.observed.prepared->AssetDependencies().size() == 2);
        CHECK(original.AssetDependencies().size() == 1);
        CHECK(fixture.observed.calls[1] == 1);
        CHECK(fixture.observed.calls[2] == 1);
        CHECK(fixture.observed.prepares == 1);
        CHECK(fixture.observed.publications == 1);
        REQUIRE(fixture.service.ActiveScene()->FindAsset(Asset(2)));
        REQUIRE(fixture.service.QueuePreparation(Definition(2)).HasValue());
        REQUIRE_FALSE(fixture.Pump(2));
        CHECK(fixture.observed.calls[1] == 2);
        CHECK(fixture.observed.calls[2] == 2);
    }

    TEST_CASE("Scene cold discovery charges duplicate edges and respects lower unique capacity", "[scene][assets][discovery][limits]") {
        RuntimeSceneAssetLimits limits;
        SECTION("duplicate edges exhaust reference budget") {
            limits.maximumDependencyReferences = 1;
        }
        SECTION("one free entry is not a reference budget") {
            limits.maximumDependencies = 1;
        }
        SECTION("newly discovered payload respects existing resident budget") {
            limits.maximumResidentBytes = 3;
        }
        Fixture fixture(limits);
        fixture.observed.discover = [](std::uint8_t) {
            return Result<std::vector<SceneAssetDependency>>::Success({Dependency(2), Dependency(2)});
        };
        fixture.Start();
        REQUIRE(fixture.service.QueuePreparation(Definition(1)).HasValue());
        REQUIRE(fixture.Pump(1));
        CHECK(fixture.observed.prepares == 0);
        CHECK(fixture.observed.publications == 0);
        CHECK_FALSE(fixture.service.ActiveScene());
    }

    TEST_CASE("Scene cold discovery rejects invalid hard and zero reference policies", "[scene][assets][discovery][limits]") {
        RuntimeSceneAssetLimits limits;
        SECTION("zero") {
            limits.maximumDependencyReferences = 0;
        }
        SECTION("above hard ceiling") {
            limits.maximumDependencyReferences = 1048577;
        }
        Fixture fixture(limits);
        fixture.Start();
        CHECK(fixture.service.QueuePreparation(Definition(1)).HasError());
        CHECK(fixture.observed.calls[1] == 0);
    }

    TEST_CASE("Scene cold discovery failures preserve lastgood source and owner publication", "[scene][assets][discovery][rollback]") {
        Fixture fixture;
        fixture.Start();
        const auto original = Definition(1);
        REQUIRE(fixture.service.QueuePreparation(original).HasValue());
        REQUIRE_FALSE(fixture.Pump(1));
        const auto before = *fixture.service.ActiveScene();
        SECTION("late type conflict") {
            fixture.observed.discover = [](std::uint8_t) {
                return Result<std::vector<SceneAssetDependency>>::Success({Dependency(1, "core.texture")});
            };
        }
        SECTION("missing registry dependency") {
            fixture.observed.discover = [](std::uint8_t) {
                return Result<std::vector<SceneAssetDependency>>::Success({Dependency(9)});
            };
        }
        SECTION("actual callback allocation failure") {
            fixture.observed.discover = [](std::uint8_t) -> Result<std::vector<SceneAssetDependency>> {
                throw std::bad_alloc{};
            };
        }
        SECTION("unexpected callback exception") {
            fixture.observed.discover = [](std::uint8_t) -> Result<std::vector<SceneAssetDependency>> {
                throw 7;
            };
        }
        REQUIRE(fixture.service.QueuePreparation(Definition(2)).HasValue());
        const auto error = fixture.Pump(2);
        REQUIRE(error);
        REQUIRE(fixture.service.ActiveScene());
        CHECK(fixture.service.ActiveScene()->RuntimeId() == before.RuntimeId());
        CHECK(fixture.service.ActiveScene()->DefinitionRevision() == before.DefinitionRevision());
        CHECK(before.IsCurrent());
        CHECK(fixture.observed.publications == 1);
        CHECK(fixture.observed.prepares == 1);
        CHECK(original.AssetDependencies().size() == 1);
    }

    TEST_CASE("Scene discovery cancellation and deferred shutdown cannot merge or activate callback output",
              "[scene][assets][discovery][lifecycle]") {
        Fixture fixture;
        fixture.Start();
        bool shutdown{};
        SECTION("actual cancellation source") {
            shutdown = false;
        }
        SECTION("actual deferred service shutdown") {
            shutdown = true;
        }
        fixture.observed.discover = [&](std::uint8_t) {
            if (shutdown)
                fixture.service.Shutdown();
            else
                fixture.cancellation.RequestCancellation();
            return Result<std::vector<SceneAssetDependency>>::Success({Dependency(2)});
        };
        const auto receipt =
            Take(fixture.service.QueuePreparationWithPublicationCheck(Definition(1), std::make_unique<DiscoveryPublicationCheck>()));
        CHECK(fixture.PumpReceipt(receipt) == ScenePublicationStatus::Rejected);
        CHECK(fixture.observed.calls[1] == 1);
        CHECK(fixture.observed.calls[2] == 0);
        CHECK(fixture.observed.prepares == 0);
        CHECK(fixture.observed.publications == 0);
        CHECK_FALSE(fixture.service.ActiveScene());
        if (shutdown) {
            const auto after = fixture.service.QueuePreparation(Definition(2));
            REQUIRE(after.HasError());
            CHECK(after.ErrorValue().code.Value() == "scene.service.shutdown");
        }
    }

    namespace {
        thread_local bool allocationFailed{};

        void ObserveFailure(std::size_t) noexcept {
            allocationFailed = true;
        }

        void PrepareExpansion(Fixture &fixture) {
            fixture.Start();
            REQUIRE(fixture.service.QueuePreparation(Definition(1)).HasValue());
            REQUIRE_FALSE(fixture.Pump(1));
            const auto expansion = Result<std::vector<SceneAssetDependency>>::Success({Dependency(2)});
            const auto empty = Result<std::vector<SceneAssetDependency>>::Success({});
            // Prepare fixture descriptors and assertions before injection. The callback
            // still allocates the owned response copy inside the measured operation.
            fixture.observed.discover = [expansion, empty](std::uint8_t id) {
                return id == 1 ? expansion : empty;
            };
        }

        std::size_t ExpansionAllocations() {
            Fixture fixture;
            PrepareExpansion(fixture);
            auto definition = Definition(2);
            auto queued = Result<void>::Success();
            std::optional<Error> failure;
            Tests::AllocationProbe::Measurement measured;
            {
                Tests::AllocationProbe::ScopedMeasurement scope;
                queued = fixture.service.QueuePreparation(std::move(definition));
                if (queued.HasValue())
                    failure = fixture.Pump(2);
                measured = scope.Snapshot();
            }
            REQUIRE(queued.HasValue());
            REQUIRE_FALSE(failure);
            return measured.requests;
        }
    }  // namespace

    TEST_CASE("Scene discovery allocation failures preserve the complete actual lastgood transaction",
              "[scene][assets][discovery][allocation]") {
        const auto allocations = ExpansionAllocations();
        REQUIRE(allocations > 0);
        for (std::size_t index = 0; index < allocations; ++index) {
            Fixture fixture;
            PrepareExpansion(fixture);
            const auto before = *fixture.service.ActiveScene();
            auto definition = Definition(2);
            auto queued = Result<void>::Success();
            std::optional<Error> failure;
            allocationFailed = false;
            {
                Tests::AllocationProbe::ScopedFailure injected(index, ObserveFailure);
                queued = fixture.service.QueuePreparation(std::move(definition));
                if (queued.HasValue())
                    failure = fixture.Pump(2);
            }
            INFO("actual owner allocation index=" << index << " / " << allocations);
            REQUIRE(allocationFailed);
            CHECK((queued.HasError() || failure.has_value()));
            REQUIRE(fixture.service.ActiveScene());
            CHECK(fixture.service.ActiveScene()->RuntimeId() == before.RuntimeId());
            CHECK(fixture.service.ActiveScene()->DefinitionRevision() == before.DefinitionRevision());
            CHECK(before.IsCurrent());
            CHECK(fixture.observed.publications == 1);
        }
    }

}  // namespace Horo::Runtime

namespace Horo::Runtime {
    using Horo::Tests::AllocationProbe::ScopedFailure;
    using Horo::Tests::AllocationProbe::ScopedMeasurement;

    TEST_CASE("Scene builder empty construction remains allocation free with checked storage", "[scene][definition_storage][allocation]") {
        static_assert(std::is_nothrow_constructible_v<SceneDefinitionBuilder, SceneDefinitionId, SceneDefinitionRevision>);
        std::optional<SceneDefinitionBuilder> builder;
        {
            ScopedFailure denied(0);
            builder.emplace(SceneDefinitionId{17}, SceneDefinitionRevision{1});
        }
        const auto source = Definition(1);
        builder->Add(source.Entities().front());
        auto built = Take(std::move(*builder).Build());
        CHECK(built.Entities().size() == 1);
    }

    TEST_CASE("Scene builder first append propagates every cold storage allocation failure", "[scene][definition_storage][allocation]") {
        const auto source = Definition(1);
        const auto &entity = source.Entities().front();
        SceneDefinitionBuilder measured{{17}, {1}};
        std::size_t requests{};
        {
            ScopedMeasurement capture;
            measured.Add(entity);
            requests = capture.Snapshot().requests;
        }
        REQUIRE(requests > 0);
        for (std::size_t index = 0; index < requests; ++index) {
            INFO(index);
            SceneDefinitionBuilder builder{{17}, {1}};
            bool failed{};
            {
                ScopedFailure injected(index);
                try {
                    builder.Add(entity);
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            builder.Add(entity);
            const auto built = Take(std::move(builder).Build());
            REQUIRE(built.Entities().size() == 1);
            CHECK(built.Entities().front().object == entity.object);
        }
    }

    TEST_CASE("Scene builder growth failure preserves previously authored entities", "[scene][definition_storage][allocation]") {
        const auto source = Definition(1);
        const auto &first = source.Entities().front();
        const auto &second = source.Entities().back();
        SceneDefinitionBuilder measured{{17}, {1}};
        measured.Add(first);
        std::size_t requests{};
        {
            ScopedMeasurement capture;
            measured.Add(second);
            requests = capture.Snapshot().requests;
        }
        REQUIRE(requests > 0);
        for (std::size_t index = 0; index < requests; ++index) {
            INFO(index);
            SceneDefinitionBuilder builder{{17}, {1}};
            builder.Add(first);
            bool failed{};
            {
                ScopedFailure injected(index);
                try {
                    builder.Add(second);
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            const auto built = Take(std::move(builder).Build());
            REQUIRE(built.Entities().size() == 1);
            CHECK(built.Entities().front().object == first.object);
        }
    }

    TEST_CASE("Scene builder copy and owner transfer preserve independent authored storage", "[scene][definition_storage][allocation]") {
        static_assert(std::is_nothrow_move_constructible_v<SceneDefinitionBuilder>);
        const auto source = Definition(1);
        SceneDefinitionBuilder original{{17}, {1}};
        original.Add(source.Entities().front());
        SceneDefinitionBuilder copied(original);
        std::optional<SceneDefinitionBuilder> transferred;
        {
            ScopedFailure denied(0);
            transferred.emplace(std::move(original));
        }
        const auto moved = Take(std::move(*transferred).Build());
        copied.Add(source.Entities().back());
        const auto detached = Take(std::move(copied).Build());
        original.Add(source.Entities().front());
        const auto reused = Take(std::move(original).Build());
        CHECK(moved.Entities().size() == 1);
        CHECK(detached.Entities().size() == 2);
        CHECK(reused.Entities().size() == 1);
        CHECK(moved.Entities().data() != detached.Entities().data());
        CHECK(moved.Entities().data() != reused.Entities().data());
    }

    TEST_CASE("Scene definition copies unwind allocation failures before changing the source", "[scene][definition_storage][allocation]") {
        const auto source = Definition(1);
        const auto *entities = source.Entities().data();
        const auto *assets = source.AssetDependencies().data();
        std::optional<RuntimeSceneDefinition> measured;
        std::size_t requests{};
        {
            ScopedMeasurement capture;
            measured.emplace(source);
            requests = capture.Snapshot().requests;
        }
        REQUIRE(requests > 0);
        CHECK(measured->Entities().data() != entities);
        CHECK(measured->AssetDependencies().data() != assets);
        for (std::size_t index = 0; index < requests; ++index) {
            INFO(index);
            std::optional<RuntimeSceneDefinition> destination;
            bool failed{};
            {
                ScopedFailure injected(index);
                try {
                    destination.emplace(source);
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            CHECK_FALSE(destination.has_value());
            CHECK(source.Entities().data() == entities);
            CHECK(source.AssetDependencies().data() == assets);
            CHECK(source.Revision().value == 1);
        }
    }

    TEST_CASE("Scene definition assignment prepares storage before replacing its prior value", "[scene][definition_storage][allocation]") {
        const auto source = Definition(2);
        auto measured = Definition(1);
        std::size_t requests{};
        {
            ScopedMeasurement capture;
            measured = source;
            requests = capture.Snapshot().requests;
        }
        REQUIRE(requests > 0);
        for (std::size_t index = 0; index < requests; ++index) {
            INFO(index);
            auto destination = Definition(1);
            const auto *prior = destination.Entities().data();
            bool failed{};
            {
                ScopedFailure injected(index);
                try {
                    destination = source;
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
            CHECK(destination.Entities().data() == prior);
            CHECK(destination.Revision().value == 1);
            CHECK(source.Revision().value == 2);
        }
    }

    TEST_CASE("Scene definition result transfer performs no allocation", "[scene][definition_storage][allocation]") {
        static_assert(std::is_nothrow_move_constructible_v<RuntimeSceneDefinition>);
        static_assert(std::is_nothrow_move_constructible_v<Result<RuntimeSceneDefinition>>);
        auto source = Definition(1);
        const auto *entities = source.Entities().data();
        const auto *assets = source.AssetDependencies().data();
        std::optional<Result<RuntimeSceneDefinition>> destination;
        {
            ScopedFailure denied(0);
            destination.emplace(Result<RuntimeSceneDefinition>::Success(std::move(source)));
        }
        REQUIRE(destination->HasValue());
        CHECK(destination->Value().Entities().data() == entities);
        CHECK(destination->Value().AssetDependencies().data() == assets);
        CHECK(source.Entities().empty());
        CHECK(source.AssetDependencies().empty());
        CHECK(source.TriggerVolumeEvidence().empty());
    }
}  // namespace Horo::Runtime

namespace Horo::Runtime {
    TEST_CASE("Scene closure expansion preserves nonempty trigger proof and every authored entity field",
              "[scene][assets][discovery][trigger][preservation]") {
        Fixture fixture;
        const auto materialType = Take(Assets::AssetTypeId::Parse("core.physics_material"));
        std::vector<Assets::AssetRecord> records(fixture.registry.Snapshot().Records().begin(),
                                                 fixture.registry.Snapshot().Records().end());
        records.back().type = materialType;
        REQUIRE(fixture.registry.Publish(std::move(records)).status == Assets::AssetRegistryBuildStatus::Complete);
        Test::TriggerCellValues trigger;
        auto root = Definition(1).Entities().front();
        root.components.rigidBody = trigger.entities.front().components.rigidBody;
        root.components.colliders = trigger.entities.front().components.colliders;
        root.components.colliders.front().materials.front().material = Asset(3);
        auto proof = trigger.proof.front();
        proof.binding.material = {Asset(3), materialType};
        SceneDefinitionBuilder builder{{17}, {1}};
        builder.Add(root);
        builder.Add(RuntimeSceneTestSupport::Entity(2, 1));
        REQUIRE(builder.RequireAsset(Dependency(1)).HasValue());
        REQUIRE(builder.RequireTriggerVolume(proof).HasValue());
        const auto original = Take(std::move(builder).Build());
        REQUIRE(original.TriggerVolumeEvidence().size() == 1);
        fixture.observed.discover = [](std::uint8_t id) {
            return Result<std::vector<SceneAssetDependency>>::Success(id == 1 ? std::vector{Dependency(2)}
                                                                              : std::vector<SceneAssetDependency>{});
        };
        fixture.Start();
        REQUIRE(fixture.service.QueuePreparation(original).HasValue());
        const auto failure = fixture.Pump(1);
        INFO((failure ? failure->code.Value() + ": " + failure->message : "success"));
        REQUIRE_FALSE(failure);
        REQUIRE(fixture.observed.prepared);
        CheckPreserved(*fixture.observed.prepared, original);
        CHECK(fixture.observed.prepared->AssetDependencies().size() == 3);
        CHECK(original.AssetDependencies().size() == 2);
        CHECK(original.TriggerVolumeEvidence().front() == proof);
        CHECK(fixture.observed.prepares == 1);
        CHECK(fixture.observed.publications == 1);
        // This verifies canonical Scene closure reconstruction, not native Physics activation.
    }
}  // namespace Horo::Runtime

namespace Horo::Runtime {
    TEST_CASE("Scene definition component construction propagates checked allocation failures", "[scene][definition_storage][allocation]") {
        static_assert(std::is_aggregate_v<RuntimeComponentSet>);
        std::size_t requests{};
        {
            ScopedMeasurement capture;
            RuntimeComponentSet components;
            requests = capture.Snapshot().requests;
        }
        for (std::size_t index = 0; index < requests; ++index) {
            INFO(index);
            bool failed{};
            {
                ScopedFailure injected(index);
                try {
                    RuntimeComponentSet components;
                } catch (const std::bad_alloc &) {
                    failed = true;
                }
            }
            REQUIRE(failed);
        }
        const RuntimeComponentSet recovered;
        CHECK(recovered.colliders.empty());
        CHECK(recovered.physicsConstraints.empty());
        CHECK(recovered.behaviors.empty());
        CHECK(recovered.gameplayComponents.empty());
    }
}  // namespace Horo::Runtime
