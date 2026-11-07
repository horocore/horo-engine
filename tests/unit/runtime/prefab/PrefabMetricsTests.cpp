#include "AllocationProbe.h"
#include "Horo/Foundation/Telemetry/Operation.h"
#include "Horo/Prefab/PrefabExpansionCache.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Prefab/PrefabTemplateCook.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabMetricsTestSupport.h"
#include "PrefabSpawnTestSupport.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace Horo::Prefab::MetricTest {
    namespace {
        thread_local bool InjectedFailure{};

        /** @brief Reads the existing immutable canonical family; this feature creates no parallel baseline. */
        std::string Read(const char *name) {
            std::ifstream input{std::filesystem::path{HORO_PREFAB_CORPUS_DIR} / name, std::ios::binary};
            REQUIRE(input.is_open());
            return {std::istreambuf_iterator<char>{input}, {}};
        }

        PrefabLimitProfile Limits() {
            return PrefabLimitProfile::Create({}).Value();
        }

        struct Sources final {
            Assets::AssetRegistry registry;
            std::vector<PrefabDependencySource> documents;

            explicit Sources(const PrefabMetricHandles &metrics) {
                std::vector<Assets::AssetRecord> records;
                for (const auto *name : {"single.prefab", "multi.prefab"}) {
                    const auto bytes = Read(name);
                    const auto parsed = PrefabDocument::Parse(bytes, Limits(), {.metrics = &metrics});
                    REQUIRE(parsed.HasValue());
                    documents.push_back(
                        {parsed.Value(), {parsed.Value().Data().projectVersion, ComputeSha256(std::as_bytes(std::span(bytes)))}});
                    const auto path = "assets/" + std::string{name};
                    records.emplace_back(parsed.Value().Data().assetId, Assets::AssetTypeId::Parse("core.prefab").Value(),
                                         ProjectPath::Parse(path).Value(), ProjectPath::Parse(path + ".horo").Value());
                }
                REQUIRE(registry.Publish(records).status == Assets::AssetRegistryBuildStatus::Complete);
            }

            PrefabSourceResolverSnapshot Resolver(const PrefabMetricHandles &metrics) const {
                return BuildPrefabSourceResolverSnapshot(registry.Snapshot(), documents, Limits(), metrics).Value();
            }
        };
    }  // namespace

    TEST_CASE("Prefab metrics measure real canonical stages and external cache outcomes", "[prefab][prefab-metrics]") {
        Recorder recorder;
        Sources sources{recorder.handles};
        const auto resolver = sources.Resolver(recorder.handles);
        const auto candidate = resolver.Resolve(Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits());
        REQUIRE(candidate.HasValue());
        const auto identities = RemapPrefabCandidateToScene(candidate.Value(), {}, {}, Limits());
        REQUIRE(identities.HasValue());
        std::vector<PrefabRuntimeComponentProjection> projections;
        for (const auto &object : candidate.Value().Objects())
            projections.push_back({object.key, {}});
        REQUIRE(ExpandPrefabSceneSubtree(candidate.Value(), identities.Value(), projections, {}, Limits()).HasValue());
        const auto cooked = CookPrefabTemplate(resolver, sources.registry.Snapshot(), Test::Asset(2), {},
                                               AssetCookTargetId::Parse("linux-x64").Value(), Limits());
        REQUIRE(cooked.HasValue());
        Math::Transform assigned;
        assigned.translation = {9, 0, 0};
        const auto edit = PreparePrefabValueEdit(candidate.Value(), {},
                                                 {PrefabValueEditKind::Assign, PrefabObjectAddress::Create({}, {17}).Value(), assigned},
                                                 nullptr, Limits());
        REQUIRE(edit.HasValue());
        REQUIRE(edit.Value().effective);
        CHECK(edit.Value().effective->Objects()[1].effectiveLocalTransform.translation.x == 9);
        CHECK(candidate.Value().Objects()[1].effectiveLocalTransform.translation.x == 2);
        PrefabExpansionCache cache{{.maximumEntries = 1}};
        REQUIRE(cache.Resolve(resolver, Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits()).HasValue());
        REQUIRE(cache.Resolve(resolver, Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits()).HasValue());
        REQUIRE(cache.Resolve(resolver, Test::Asset(1), PrefabInstanceId::Create(41).Value(), Limits()).HasValue());
        const auto silent = sources.Resolver({});
        CHECK(cache.CaptureKey(resolver, Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits()).Value() ==
              cache.CaptureKey(silent, Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits()).Value());
        PrefabExpansionCache constrained{{.maximumRetainedBytes = 1}};
        CHECK(constrained.Resolve(resolver, Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits()).HasError());
        const auto bytes = Read("multi.prefab");
        CHECK(PrefabDocument::Parse(bytes, Limits(), {.maximumSourceBytes = bytes.size() - 1, .metrics = &recorder.handles}).HasError());
        CHECK(PrefabDocument::Parse("{", Limits(), {.metrics = &recorder.handles}).HasError());
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        CHECK(resolver.Resolve(Test::Asset(2), PrefabInstanceId::Create(41).Value(), Limits(), cancelled.Token()).HasError());
        recorder.Flush();
        for (const auto phase : {PrefabMetricPhase::Parse, PrefabMetricPhase::Graph, PrefabMetricPhase::Resolve, PrefabMetricPhase::Expand,
                                 PrefabMetricPhase::Cache, PrefabMetricPhase::Cook, PrefabMetricPhase::Rebase})
            CHECK(recorder.sink->Attempts(phase, PrefabMetricOutcome::Success) >= 1);
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Parse, PrefabMetricOutcome::Overflow) == 1);
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Parse, PrefabMetricOutcome::Failed) == 1);
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Resolve, PrefabMetricOutcome::Cancelled) == 1);
        CHECK(recorder.sink->Cache(PrefabMetricCacheEvent::Hit) == 1);
        CHECK(recorder.sink->Cache(PrefabMetricCacheEvent::Miss) == 3);
        CHECK(recorder.sink->Cache(PrefabMetricCacheEvent::Evicted) == 1);
        CHECK(recorder.sink->Cache(PrefabMetricCacheEvent::Rejected) == 1);
        CHECK(recorder.sink->Spans() > 0);
    }

    TEST_CASE("Prefab dimensions stay fixed when source identities paths and names change", "[prefab][prefab-metrics]") {
        Recorder recorder;
        const auto baseline = PrefabDocument::Parse(Read("single.prefab"), Limits()).Value();
        const auto registrations = Telemetry::Runtime::GetDiagnosticSnapshot().availabilityCount;
        for (std::uint16_t index = 1; index <= 64; ++index) {
            auto data = baseline.Data();
            data.assetId = Test::Asset(static_cast<std::uint16_t>(100 + index));
            data.objects[0].name = "user-path-private-" + std::to_string(index);
            const auto wire = PrefabDocument::Create(std::move(data), Limits()).Value().SerializeCanonical().Value();
            REQUIRE(PrefabDocument::Parse(wire, Limits(), {.metrics = &recorder.handles}).HasValue());
            if (index % 8 == 0)
                recorder.Flush();
        }
        recorder.Flush();
        CHECK(Telemetry::Runtime::GetDiagnosticSnapshot().availabilityCount == registrations);
        std::lock_guard lock{recorder.sink->mutex};
        std::set<std::pair<std::uint32_t, std::array<std::uint16_t, Telemetry::MaximumMetricDimensions>>> series;
        for (std::size_t index = 0; index < recorder.sink->records.size(); ++index) {
            const auto &record = recorder.sink->records[index];
            CHECK(record.context.Fields().empty());
            if (const auto *metric = std::get_if<Telemetry::MetricRecord>(&record.payload)) {
                series.emplace(metric->instrumentId, metric->dimensionValueIds);
                for (const auto &dimension : recorder.sink->descriptors[index].dimensions)
                    CHECK((dimension.key == "phase" || dimension.key == "outcome" || dimension.key == "kind"));
            } else if (const auto *span = std::get_if<Telemetry::SpanRecord>(&record.payload)) {
                CHECK(span->name == "prefab.parse");
                CHECK(span->fields.empty());
                CHECK(span->operationId != 0);
            }
        }
        CHECK(series.size() <= 54);
    }

    TEST_CASE("Prefab disabled and terminal measurements allocate no producer-side memory", "[prefab][prefab-metrics]") {
        Recorder recorder;
        Telemetry::Runtime::SetEnabled(false);
        Tests::AllocationProbe::Measurement disabled;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            for (int index = 0; index < 32; ++index) {
                PrefabMetricOperation operation{recorder.handles, PrefabMetricPhase::Spawn};
                operation.Observe(PrefabMetricItem::RuntimeEntities, 2);
                operation.Complete();
            }
            disabled = measurement.Snapshot();
        }
        CHECK(disabled.requests == 0);
        Telemetry::Runtime::SetEnabled(true);
        PrefabMetricOperation prepared{recorder.handles, PrefabMetricPhase::Spawn};
        Tests::AllocationProbe::Measurement terminal;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            prepared.Observe(PrefabMetricItem::RuntimeEntities, 2);
            prepared.Complete();
            terminal = measurement.Snapshot();
        }
        CHECK(terminal.requests == 0);
        InjectedFailure = false;
        PrefabMetricHandles unavailable;
        {
            Tests::AllocationProbe::ScopedFailure failure{0, +[](std::size_t) noexcept {
                InjectedFailure = true;
            }};
            unavailable = RegisterPrefabMetricHandles(Telemetry::MetricCollectionLevel::Detailed, true);
        }
        CHECK(InjectedFailure);
        CHECK_FALSE(unavailable.attempts[0][0]);
        recorder.Flush();
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Success) == 1);
    }

    TEST_CASE("Prefab retired observations cannot emit spans into a restarted Telemetry generation", "[prefab][prefab-metrics]") {
        Recorder recorder;
        PrefabMetricOperation pending{recorder.handles, PrefabMetricPhase::Spawn};
        const auto oldGeneration = Telemetry::Runtime::Generation();
        recorder.Resume();
        REQUIRE(Telemetry::Runtime::Shutdown());
        auto replacement = std::make_shared<Sink>();
        REQUIRE(Telemetry::Runtime::Initialize({.metricCollectionLevel = Telemetry::MetricCollectionLevel::Detailed}, replacement));
        CHECK(Telemetry::Runtime::Generation() != oldGeneration);
        pending.Complete();
        PrefabMetricOperation stale{recorder.handles, PrefabMetricPhase::Spawn};
        CHECK_FALSE(stale.Enabled());
        stale.Complete();
        REQUIRE(Telemetry::Runtime::Flush());
        CHECK(replacement->Spans() == 0);
        const auto control = [] {
            return Telemetry::Record{.subsystem = "prefab",
                                     .payload = Telemetry::SpanRecord{.operationId = Telemetry::AllocateOperationId(), .name = "control"}};
        };
        CHECK_FALSE(Telemetry::Runtime::EmitRecord(control(), 0));
        CHECK_FALSE(Telemetry::Runtime::EmitRecord(control(), oldGeneration));
        CHECK(Telemetry::Runtime::EmitRecord(control()));
        REQUIRE(Telemetry::Runtime::Flush());
        CHECK(replacement->Spans() == 1);
    }

    TEST_CASE("Prefab spawn metrics follow actual terminal receipts once across cancellation and shutdown", "[prefab][prefab-metrics]") {
        Recorder recorder;
        SpawnTest::Fixture fixture{2, {}, recorder.handles};
        const auto successful = fixture.context->Spawn(fixture.Request());
        REQUIRE(successful.HasValue());
        fixture.Drain(successful.Value());
        REQUIRE(successful.Value().State() == PrefabSpawnState::Committed);
        for (int index = 0; index < 8; ++index)
            CHECK(successful.Value().State() == PrefabSpawnState::Committed);
        auto invalid = fixture.Request();
        invalid.notBeforeTick = 0;
        CHECK(fixture.context->Spawn(invalid).HasError());
        const auto cancelled = fixture.context->Spawn(fixture.Request());
        REQUIRE(cancelled.HasValue());
        cancelled.Value().Cancel();
        fixture.Drain(cancelled.Value());
        CHECK(cancelled.Value().State() == PrefabSpawnState::Cancelled);
        std::vector<PrefabOperation> pending;
        for (int index = 0; index < 32; ++index) {
            const auto request = fixture.context->Spawn(fixture.Request());
            REQUIRE(request.HasValue());
            pending.push_back(request.Value());
        }
        CHECK(fixture.context->Spawn(fixture.Request()).HasError());
        fixture.service->Shutdown();
        for (const auto &request : pending)
            CHECK(request.State() == PrefabSpawnState::Cancelled);
        recorder.Flush();
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Success) == 1);
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Failed) == 1);
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Overflow) == 1);
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Cancelled) == 33);
    }

    TEST_CASE("Prefab Telemetry queue loss cannot change canonical parsing", "[prefab][prefab-metrics]") {
        Recorder recorder{Telemetry::MetricCollectionLevel::Detailed, 1};
        recorder.sink->blocked = true;
        PrefabMetricOperation first{recorder.handles, PrefabMetricPhase::Parse};
        first.Complete();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        while (!recorder.sink->entered && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(recorder.sink->entered);
        const auto before = Telemetry::Runtime::GetStatistics().queueFullDrops;
        const auto bytes = Read("multi.prefab");
        const auto parsed = PrefabDocument::Parse(bytes, Limits(), {.metrics = &recorder.handles});
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().SerializeCanonical().Value() == bytes);
        CHECK(Telemetry::Runtime::GetStatistics().queueFullDrops > before);
        recorder.sink->blocked = false;
        recorder.Flush();
        recorder.Resume();
        REQUIRE(Telemetry::Runtime::Flush());
        const auto admittedBefore = Telemetry::Runtime::GetStatistics();
        const auto observedBefore = recorder.sink->Cache(PrefabMetricCacheEvent::Hit);
        constexpr std::uint64_t attempts = 128;
        for (std::uint64_t index = 0; index < attempts; ++index)
            RecordPrefabCacheEvent(recorder.handles, PrefabMetricCacheEvent::Hit);
        REQUIRE(Telemetry::Runtime::Flush());
        const auto admittedAfter = Telemetry::Runtime::GetStatistics();
        const auto accepted = admittedAfter.acceptedRecords - admittedBefore.acceptedRecords;
        CHECK(recorder.sink->Cache(PrefabMetricCacheEvent::Hit) - observedBefore == static_cast<double>(accepted));
        CHECK(accepted + admittedAfter.contentionDrops - admittedBefore.contentionDrops + admittedAfter.queueFullDrops -
                  admittedBefore.queueFullDrops ==
              attempts);
    }
}  // namespace Horo::Prefab::MetricTest
