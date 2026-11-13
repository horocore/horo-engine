#include "AllocationProbe.h"
#include "Horo/Foundation/Telemetry/Operation.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabExpansionCache.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Prefab/PrefabTemplateCook.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "PrefabMetricsTestSupport.h"
#include "PrefabSpawnTestSupport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <utility>

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

        /** @brief Creates the canonical hard-object-limit workload with an exact owned source commitment. */
        PrefabDependencySource MaximumObjectSource(const PrefabDocument &baseline) {
            auto data = baseline.Data();
            data.objects.clear();
            for (std::uint32_t index = 0; index < PrefabHardLimits::SourceObjectCount; ++index) {
                data.objects.push_back({.localId = {index},
                                        .parentLocalId = index == 0 ? std::nullopt : std::optional{LocalObjectId{0}},
                                        .name = "Qualification object"});
            }
            auto document = PrefabDocument::Create(std::move(data), Limits());
            REQUIRE(document.HasValue());
            const auto bytes = document.Value().SerializeCanonical();
            REQUIRE(bytes.HasValue());
            const PrefabSourceRevision revision{document.Value().Data().projectVersion,
                                                ComputeSha256(std::as_bytes(std::span{bytes.Value()}))};
            return {std::move(document).Value(), revision};
        }

        /** @brief Explicit workload size and independent cumulative allocation and per-sample latency ceilings. */
        struct QualificationWorkload final {
            std::uint32_t objects;
            std::size_t samples;
            std::size_t maximumRequests;
            std::size_t maximumBytes;
            std::chrono::milliseconds maximumLatency;
        };

        /** @brief Measures the owner-thread callback window and reports bounded samples outside its allocation scope. */
        void QualifyPipelinePhase(const QualificationWorkload &workload, const char *phase, const char *cache, const std::size_t inputBytes,
                                  const auto &operation) {
            std::array<std::chrono::microseconds, 16> elapsed;
            REQUIRE(workload.samples >= 2);
            REQUIRE(workload.samples <= elapsed.size());
            const auto samples = std::span{elapsed}.first(workload.samples);
            Tests::AllocationProbe::Measurement maximum;
            for (std::size_t index = 0; index < samples.size(); ++index) {
                Tests::AllocationProbe::Measurement allocations;
                const auto begin = std::chrono::steady_clock::now();
                bool succeeded{};
                {
                    Tests::AllocationProbe::ScopedMeasurement measurement;
                    succeeded = operation();
                    allocations = measurement.Snapshot();
                }
                samples[index] = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin);
                INFO("phase=" << phase << " objects=" << workload.objects << " sample=" << index);
                REQUIRE(succeeded);
                CHECK(allocations.requests <= workload.maximumRequests);
                CHECK(allocations.requestedBytes <= workload.maximumBytes);
                CHECK(samples[index] < workload.maximumLatency);
                maximum.requests = std::max(maximum.requests, allocations.requests);
                maximum.requestedBytes = std::max(maximum.requestedBytes, allocations.requestedBytes);
                maximum.largestRequest = std::max(maximum.largestRequest, allocations.largestRequest);
            }
            std::ranges::sort(samples);
            const auto median = (samples[samples.size() / 2 - 1] + samples[samples.size() / 2]) / 2;
            std::cout << "prefab-pipeline-qualification build=" << HORO_PREFAB_QUALIFICATION_BUILD_TYPE
                      << " platform=" << HORO_PREFAB_QUALIFICATION_PLATFORM << " phase=" << phase << " cache=" << cache
                      << " objects_per_operation=" << workload.objects << " input_bytes=" << inputBytes << " samples=" << samples.size()
                      << " maximum_cpp_requests=" << maximum.requests << " maximum_cpp_requested_bytes=" << maximum.requestedBytes
                      << " largest_cpp_request=" << maximum.largestRequest << " median_us=" << median.count()
                      << " maximum_us=" << samples.back().count() << '\n';
        }
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

    TEST_CASE("Prefab in-flight cancellation bounds cold maximum-object resolution work",
              "[prefab][prefab-metrics][qualification][cancellation]") {
        Sources sources{PrefabMetricHandles{}};
        sources.documents.back() = MaximumObjectSource(sources.documents.back().document);
        const auto bytes = sources.documents.back().document.SerializeCanonical();
        REQUIRE(bytes.HasValue());
        const auto resolver = sources.Resolver({});
        const auto instance = PrefabInstanceId::Create(41).Value();
        Tests::AllocationProbe::Measurement baseline;
        const auto accepted = [&] {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto result = resolver.Resolve(Test::Asset(2), instance, Limits());
            baseline = measurement.Snapshot();
            return result;
        }();
        REQUIRE(accepted.HasValue());
        REQUIRE(accepted.Value().Objects().size() == PrefabHardLimits::SourceObjectCount);
        REQUIRE(baseline.requests > 1);
        CancellationSource cancellation;

        struct CancelAtAllocation final {
            const CancellationSource *source;
            std::size_t remaining;
            bool fired{};
            std::chrono::steady_clock::time_point requestedAt;
        } state{&cancellation, baseline.requests / 2};

        Tests::AllocationProbe::Measurement interrupted;
        const auto rejected = [&] {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            Tests::AllocationProbe::ScopedObserver observer{+[](std::size_t, void *context) noexcept {
                auto &state = *static_cast<CancelAtAllocation *>(context);
                if (state.remaining > 0 && --state.remaining == 0) {
                    state.fired = true;
                    state.requestedAt = std::chrono::steady_clock::now();
                    state.source->RequestCancellation();
                }
            }, &state};
            auto result = resolver.Resolve(Test::Asset(2), instance, Limits(), cancellation.Token());
            interrupted = measurement.Snapshot();
            return result;
        }();
        const auto returnedAt = std::chrono::steady_clock::now();
        REQUIRE(state.fired);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::Cancelled.code.Value());
        CHECK(interrupted.requests < baseline.requests);
        CHECK(returnedAt - state.requestedAt < std::chrono::milliseconds{250});
        CHECK(accepted.Value().Objects().size() == PrefabHardLimits::SourceObjectCount);
        REQUIRE(resolver.Resolve(Test::Asset(2), instance, Limits()).HasValue());
        std::cout << "prefab-cancellation-qualification objects=" << PrefabHardLimits::SourceObjectCount
                  << " source_bytes=" << bytes.Value().size()
                  << " cache=cold instrumentation=disabled baseline_cpp_requests=" << baseline.requests
                  << " baseline_cpp_requested_bytes=" << baseline.requestedBytes << " interrupted_cpp_requests=" << interrupted.requests
                  << " interrupted_cpp_requested_bytes=" << interrupted.requestedBytes
                  << " cancellation_us=" << std::chrono::duration_cast<std::chrono::microseconds>(returnedAt - state.requestedAt).count()
                  << '\n';
    }

    TEST_CASE("Prefab canonical parse expand cook and committed spawn enforce bounded qualification samples",
              "[prefab][prefab-metrics][qualification][performance]") {
        const auto limits = Limits();
        const auto instance = PrefabInstanceId::Create(41).Value();
        const auto target = AssetCookTargetId::Parse("linux-x64").Value();
        const std::array workloads{
            QualificationWorkload{2, 16, 4096, 4U * 1024U * 1024U, std::chrono::milliseconds{250}},
            QualificationWorkload{static_cast<std::uint32_t>(PrefabHardLimits::SourceObjectCount), 4, 128U * 1024U, 64U * 1024U * 1024U,
                                  std::chrono::milliseconds{1000}},
        };
        for (const auto &workload : workloads) {
            Sources sources{PrefabMetricHandles{}};
            if (workload.objects == PrefabHardLimits::SourceObjectCount)
                sources.documents.back() = MaximumObjectSource(sources.documents.back().document);
            const auto encoded = sources.documents.back().document.SerializeCanonical();
            REQUIRE(encoded.HasValue());
            const auto &source = encoded.Value();
            REQUIRE(sources.documents.back().document.Data().objects.size() == workload.objects);
            const auto resolver = sources.Resolver({});
            QualifyPipelinePhase(workload, "parse", "disabled", source.size(), [&] {
                const auto parsed = PrefabDocument::Parse(source, limits);
                return parsed.HasValue() && parsed.Value().Data().objects.size() == workload.objects;
            });
            QualifyPipelinePhase(workload, "expand", "disabled", source.size(), [&] {
                const auto expanded = resolver.Resolve(Test::Asset(2), instance, limits);
                return expanded.HasValue() && expanded.Value().Objects().size() == workload.objects;
            });
            QualifyPipelinePhase(workload, "cook", "disabled", source.size(), [&] {
                const auto cooked = CookPrefabTemplate(resolver, sources.registry.Snapshot(), Test::Asset(2), {}, target, limits);
                return cooked.HasValue() && cooked.Value().Data().entities.size() == workload.objects;
            });
            SpawnTest::Fixture fixture;
            auto runtimeTemplate = fixture.Data();
            if (workload.objects == PrefabHardLimits::SourceObjectCount) {
                const auto child = runtimeTemplate.entities.back();
                runtimeTemplate.entities.resize(1);
                for (std::uint32_t index = 1; index < workload.objects; ++index) {
                    auto entity = child;
                    const auto address = PrefabObjectAddress::Create({}, LocalObjectId{index});
                    REQUIRE(address.HasValue());
                    entity.provenance.sourceObject = address.Value();
                    runtimeTemplate.entities.push_back(std::move(entity));
                }
            }
            const auto templateBytes = CookedPrefab::Create(runtimeTemplate, limits);
            REQUIRE(templateBytes.HasValue());
            fixture.Put(std::move(runtimeTemplate));
            fixture.Warm();
            const auto request = fixture.Request();
            QualifyPipelinePhase(workload, "spawn_commit", "warm-template", templateBytes.Value().Bytes().size(), [&] {
                const auto spawned = fixture.context->Spawn(request);
                if (spawned.HasError())
                    return false;
                if (fixture.service->Advance(fixture.tick).HasError())
                    return false;
                if (fixture.scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, fixture.Frame()).HasError())
                    return false;
                if (fixture.service->Advance(fixture.tick).HasError())
                    return false;
                return spawned.Value().State() == PrefabSpawnState::Committed;
            });
            const auto expectedEntities = 1 + workload.objects * workload.samples;
            CHECK(fixture.Count() == expectedEntities);
            fixture.service->Shutdown();
            CHECK(fixture.context->Spawn(request).HasError());
            fixture.runner->Shutdown();
            CHECK(fixture.trace.destroys == expectedEntities);
        }
    }

    TEST_CASE("Prefab disabled measurements allocate no producer-side memory", "[prefab][prefab-metrics]") {
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
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Success) == 0);
    }

    TEST_CASE("Prefab terminal measurements allocate no producer-side memory", "[prefab][prefab-metrics][allocation]") {
#if defined(_MSC_VER) && _ITERATOR_DEBUG_LEVEL != 0
        SKIP("MSVC Debug record moves allocate iterator proxies; mandatory Windows Release runs this exact allocation check.");
#endif
        Recorder recorder;
        PrefabMetricOperation prepared{recorder.handles, PrefabMetricPhase::Spawn};
        Tests::AllocationProbe::Measurement terminal;
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            prepared.Observe(PrefabMetricItem::RuntimeEntities, 2);
            prepared.Complete();
            terminal = measurement.Snapshot();
        }
        CHECK(terminal.requests == 0);
        recorder.Flush();
        CHECK(recorder.sink->Attempts(PrefabMetricPhase::Spawn, PrefabMetricOutcome::Success) == 1);
    }

    TEST_CASE("Prefab retired observations cannot emit spans into a restarted Telemetry generation", "[prefab][prefab-metrics]") {
        Recorder recorder;
        PrefabMetricOperation pending{recorder.handles, PrefabMetricPhase::Spawn};
        const auto oldGeneration = Telemetry::Runtime::Generation();
        recorder.Resume();
        REQUIRE(Telemetry::Runtime::Shutdown());
        Recorder replacement;
        CHECK(Telemetry::Runtime::Generation() != oldGeneration);
        pending.Complete();
        PrefabMetricOperation stale{recorder.handles, PrefabMetricPhase::Spawn};
        CHECK_FALSE(stale.Enabled());
        stale.Complete();
        replacement.Flush();
        CHECK(replacement.sink->Spans() == 0);
        const auto control = [] {
            return Telemetry::Record{.subsystem = "prefab",
                                     .payload = Telemetry::SpanRecord{.operationId = Telemetry::AllocateOperationId(), .name = "control"}};
        };
        CHECK_FALSE(Telemetry::Runtime::EmitRecord(control(), 0));
        CHECK_FALSE(Telemetry::Runtime::EmitRecord(control(), oldGeneration));
        CHECK(Telemetry::Runtime::EmitRecord(control()));
        replacement.Flush();
        CHECK(replacement.sink->Spans() == 1);
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
