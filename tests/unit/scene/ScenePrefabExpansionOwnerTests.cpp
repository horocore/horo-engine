#include "AllocationProbe.h"
#include "ScenePrefabExpansionTestSupport.h"

#include <algorithm>
#include <array>
#include <semaphore>
#include <thread>

using namespace Horo;
using namespace Horo::Prefab;
using namespace Horo::SceneSource;
using namespace Horo::SceneSource::ExpansionTestSupport;

namespace {
    /** @brief Real completed job and detached request shared by distinct evidence-domain regressions. */
    struct CompletedExpansion final {
        ExpansionFixture fixture;
        JobSystem jobs;
        ScenePrefabExpansionOwner owner{jobs};
        ScenePrefabExpansionRequest request = fixture.Request();

        CompletedExpansion() {
            REQUIRE(owner.Submit(request).HasValue());
            REQUIRE(owner.Join(TestJoin).HasValue());
        }

        void RequireRejectedAndRetired() {
            CHECK(owner.TakeCompleted(request).HasError());
            CHECK(owner.TakeCompleted(request).Value() == std::nullopt);
            REQUIRE(owner.Shutdown(TestJoin).HasValue());
        }
    };

    /** @brief Measures only production completion, excluding framework allocation from the failure-index domain. */
    std::size_t MeasureCompletionAllocations(JobSystem &jobs, const ScenePrefabExpansionRequest &request) {
        ScenePrefabExpansionOwner owner{jobs};
        REQUIRE(owner.Submit(request).HasValue());
        REQUIRE(owner.Join(TestJoin).HasValue());
        std::size_t allocations{};
        const auto result = [&] {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto completed = owner.TakeCompleted(request);
            allocations = measurement.Snapshot().requests;
            return completed;
        }();
        REQUIRE(result.HasValue());
        REQUIRE(result.Value());
        return allocations;
    }
}  // namespace

TEST_CASE("Owned expansion publishes complete current evidence exactly once", "[native][prefab][jobs]") {
    CompletedExpansion completed;
    const auto result = completed.owner.TakeCompleted(completed.request);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value());
    CHECK(result.Value()->Entities().size() == 3);
    CHECK(result.Value()->Entities()[1].localTransform.translation == Math::Vec3{3, 0, 0});
    CHECK(completed.owner.TakeCompleted(completed.request).Value() == std::nullopt);
    REQUIRE(completed.owner.Shutdown(TestJoin).HasValue());
}

TEST_CASE("Owned completion rejects every authored object field changed under the same revision", "[native][prefab][jobs]") {
    CompletedExpansion completed;
    auto &request = completed.request;
    SECTION("same revision cannot hide authored object mutation") {
        request.document.objects.front().name = "Edited";
    }
    SECTION("object identity is complete authored evidence") {
        ++request.document.objects.front().id.value;
    }
    SECTION("object parent is complete authored evidence") {
        request.document.objects.front().parent = SceneObjectId{99};
    }
    SECTION("object local transform is complete authored evidence") {
        request.document.objects.front().localTransform.translation.x = 7;
    }
    SECTION("editor-only authored state cannot hide behind runtime revision") {
        request.document.objects.front().editorState.locked = true;
    }
    SECTION("component values cannot hide behind runtime revision") {
        request.document.objects.front().components.camera = Runtime::CameraComponent{};
    }
    SECTION("mesh asset identity cannot hide behind runtime revision") {
        request.document.objects.front().meshAsset = completed.fixture.asset;
    }
    SECTION("primitive descriptor cannot hide behind runtime revision") {
        request.document.objects.front().primitiveMesh = Runtime::PrimitiveMeshDescriptor::Defaults(Runtime::PrimitiveMeshType::Box);
    }
    SECTION("authored object counts must match") {
        request.document.objects.clear();
    }
    completed.RequireRejectedAndRetired();
}

TEST_CASE("Owned completion rejects every authored placement field changed under the same revision", "[native][prefab][jobs]") {
    CompletedExpansion completed;
    auto &request = completed.request;
    SECTION("placement transform is current input not cached state") {
        request.document.prefabInstances.front().rootTransform.translation.x = 9;
    }
    SECTION("placement identity is complete authored evidence") {
        request.document.prefabInstances.front().instanceId = PrefabInstanceId::Create(8).Value();
    }
    SECTION("placement parent is complete authored evidence") {
        request.document.prefabInstances.front().parent.reset();
    }
    SECTION("placement source is complete authored evidence") {
        request.document.prefabInstances.front().sourcePrefab =
            PrefabAssetReference::Create(Assets::AssetId::FromBytes({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 18})).Value();
    }
    SECTION("placement counts must match") {
        request.document.prefabInstances.clear();
    }
    completed.RequireRejectedAndRetired();
}

TEST_CASE("Owned completion rejects changed session revision source and settings evidence", "[native][prefab][jobs]") {
    CompletedExpansion completed;
    auto &request = completed.request;
    SECTION("document revision advances") {
        ++request.revision.value;
    }
    SECTION("document session closes and another opens") {
        ++request.documentSession;
    }
    SECTION("source changes under same registry revision") {
        request.resolver = completed.fixture.Resolver("Edited");
    }
    SECTION("all settings must match") {
        auto policy = completed.fixture.limits.Policy();
        --policy.maximumRuntimeSpawnDepth;
        request.limits = PrefabLimitProfile::Create(policy).Value();
    }
    completed.RequireRejectedAndRetired();
}

TEST_CASE("Document close replacement and shutdown cancel and join actual queued expansion", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystem jobs;
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};
    auto blocker = jobs.Submit({}, [&](const CancellationToken &) {
        entered.release();
        release.acquire();
    });
    REQUIRE(blocker.HasValue());
    entered.acquire();
    ScenePrefabExpansionOwner owner{jobs};
    auto request = fixture.Request();
    const auto submitted = owner.Submit(request);
    CHECK(submitted.HasValue());
    const auto closed = owner.CloseDocument(TestJoin);
    // Release before fatal assertions: the unrelated owned scheduler task must never leak on test failure.
    release.release();
    CHECK(closed.HasValue());
    REQUIRE(blocker.Value().Wait(TestJoin).HasValue());
    CHECK_FALSE(owner.TakeCompleted(request).Value());
    CHECK(owner.Submit(request).HasError());
    REQUIRE(owner.ReplaceScene(TestJoin).HasValue());
    ++request.documentSession;
    RequireCompletedRetryAndShutdown(owner, request);
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
    REQUIRE(owner.ReplaceScene(TestJoin).HasValue());
    CHECK(owner.Submit(request).HasError());
}

TEST_CASE("Scene encoding admission reports each allocation failure without accepting invalid authored bytes",
          "[native][prefab][allocation]") {
    ExpansionFixture fixture;
    const auto request = fixture.Request();
    const auto canonical = EncodeSceneSource({request.document.objects, request.document.prefabInstances});
    std::size_t encodingAllocations{};
    const auto measured = [&] {
        Tests::AllocationProbe::ScopedMeasurement measurement;
        auto bytes = EncodeSceneSource({request.document.objects, request.document.prefabInstances});
        encodingAllocations = measurement.Snapshot().requests;
        return bytes;
    }();
    REQUIRE(measured == canonical);
    REQUIRE(encodingAllocations > 0);
    REQUIRE(encodingAllocations < 4096);
    JobSystem jobs;
    SECTION("every source encoding index fails before scheduler admission") {
        for (std::size_t index = 0; index < encodingAllocations; ++index) {
            ScenePrefabExpansionOwner owner{jobs};
            const auto failed = [&] {
                Tests::AllocationProbe::ScopedFailure failure{index};
                return owner.Submit(request);
            }();
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheAllocationFailed.code.Value());
            CHECK_FALSE(owner.TakeCompleted(request).Value());
            CHECK(EncodeSceneSource({request.document.objects, request.document.prefabInstances}) == canonical);
            REQUIRE(owner.Shutdown(TestJoin).HasValue());
        }
    }
    SECTION("invalid UTF-8 retains the former typed admission rejection") {
        auto invalid = request;
        invalid.document.objects.front().name = std::string{"\xc3\x28", 2};
        ScenePrefabExpansionOwner owner{jobs};
        const auto rejected = owner.Submit(invalid);
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == PrefabErrors::AdmissionRejected.code.Value());
        CHECK_FALSE(owner.TakeCompleted(request).Value());
        RequireCompletedRetryAndShutdown(owner, request);
    }
}

TEST_CASE("Completed worker cannot publish after parent cancellation or malformed Scene preparation", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystem jobs;
    ScenePrefabExpansionOwner owner{jobs};
    auto request = fixture.Request();
    CancellationSource cancellation;
    SECTION("late cancellation fences succeeded scheduler record") {
        REQUIRE(owner.Submit(request, cancellation.Token()).HasValue());
        REQUIRE(owner.Join(TestJoin).HasValue());
        cancellation.RequestCancellation();
        CHECK(owner.TakeCompleted(request).HasError());
    }
    SECTION("missing external parent rejects entire native conversion") {
        request.document.prefabInstances.front().parent = SceneObjectId{99};
        REQUIRE(owner.Submit(request).HasValue());
        CHECK(owner.Join(TestJoin).HasError());
        CHECK(owner.TakeCompleted(request).HasError());
    }
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
}

TEST_CASE("Expansion owner rejects foreign-thread admission and lifecycle without changing pending ownership", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystem jobs;
    ScenePrefabExpansionOwner owner{jobs};
    const auto request = fixture.Request();
    REQUIRE(owner.Submit(request).HasValue());
    bool submitRejected{};
    bool pollRejected{};
    bool closeRejected{};
    bool shutdownRejected{};
    std::thread foreign{[&] {
        const auto submitted = owner.Submit(request);
        const auto completed = owner.TakeCompleted(request);
        const auto closed = owner.CloseDocument(TestJoin);
        const auto stopped = owner.Shutdown(TestJoin);
        submitRejected =
            submitted.HasError() && submitted.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheThreadViolation.code.Value();
        pollRejected =
            completed.HasError() && completed.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheThreadViolation.code.Value();
        closeRejected = closed.HasError() && closed.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheThreadViolation.code.Value();
        shutdownRejected =
            stopped.HasError() && stopped.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheThreadViolation.code.Value();
    }};
    foreign.join();
    CHECK(submitRejected);
    CHECK(pollRejected);
    CHECK(closeRejected);
    CHECK(shutdownRejected);
    REQUIRE(owner.Join(TestJoin).HasValue());
    const auto completed = owner.TakeCompleted(request);
    REQUIRE(completed.HasValue());
    REQUIRE(completed.Value());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
}

TEST_CASE("Detached expansion inputs survive caller destruction and scene replacement retires terminal results", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystem jobs;
    ScenePrefabExpansionOwner owner{jobs};
    auto current = fixture.Request();
    {
        auto source = current;
        REQUIRE(owner.Submit(std::move(source)).HasValue());
    }
    REQUIRE(owner.Join(TestJoin).HasValue());
    REQUIRE(owner.ReplaceScene(TestJoin).HasValue());
    const auto retired = owner.TakeCompleted(current);
    REQUIRE(retired.HasValue());
    CHECK_FALSE(retired.Value());
    ++current.documentSession;
    REQUIRE(owner.Submit(current).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    const auto fresh = owner.TakeCompleted(current);
    REQUIRE(fresh.HasValue());
    REQUIRE(fresh.Value());
    CHECK(fresh.Value()->Entities().size() == 3);
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
}

TEST_CASE("Expansion admission cannot inherit a host permission to block on a full queue", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystemConfig config{.workerCount = 0, .maxQueuedJobs = 1};
    config.priorityQueues[1] = {.capacity = 1,
                                .overloadPolicy = JobOverloadPolicy::Block,
                                .blockTimeout = Duration::FromMilliseconds(5'000)};
    JobSystem jobs{config};
    const auto queued = jobs.Submit({}, [](const CancellationToken &) {
    });
    REQUIRE(queued.HasValue());
    ScenePrefabExpansionOwner owner{jobs};
    {
        const JobProducerScope hostPermission{JobProducerRole::NonCritical};
        const auto rejected = owner.Submit(fixture.Request());
        CHECK(rejected.HasError());
        CHECK(jobs.AdmissionSnapshot().waitingProducers == 0);
    }
    CHECK(queued.Value().RequestCancel().HasValue());
    static_cast<void>(queued.Value().Wait(TestJoin));
    REQUIRE(owner.Submit(fixture.Request()).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    REQUIRE(owner.TakeCompleted(fixture.Request()).Value());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
}

TEST_CASE("Cache pressure after validated worker completion does not discard complete Scene output", "[native][prefab][jobs][allocation]") {
    ExpansionFixture fixture;
    JobSystem jobs{JobSystemConfig{.workerCount = 0}};
    const auto request = fixture.Request();
    SECTION("retained byte pressure") {
        ScenePrefabExpansionOwner owner{jobs, {.maximumRetainedBytes = 1}};
        REQUIRE(owner.Submit(request).HasValue());
        REQUIRE(owner.Join(TestJoin).HasValue());
        const auto result = owner.TakeCompleted(request);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value());
        CHECK(result.Value()->Entities().size() == 3);
        REQUIRE(owner.Shutdown(TestJoin).HasValue());
    }
    SECTION("each completion allocation has explicit validation or optional memoization semantics") {
        const std::size_t allocations = MeasureCompletionAllocations(jobs, request);
        REQUIRE(allocations > 0);
        REQUIRE(allocations < 4096);
        std::size_t preservedDefinitions{};
        for (std::size_t index = 0; index < allocations; ++index) {
            ScenePrefabExpansionOwner owner{jobs};
            REQUIRE(owner.Submit(request).HasValue());
            REQUIRE(owner.Join(TestJoin).HasValue());
            const auto result = [&] {
                Tests::AllocationProbe::ScopedFailure failure{index};
                return owner.TakeCompleted(request);
            }();
            if (result.HasError())
                CHECK(result.ErrorValue().code.Value() == PrefabErrors::ExpansionCacheAllocationFailed.code.Value());
            else {
                REQUIRE(result.Value());
                CHECK(result.Value()->Entities().size() == 3);
                ++preservedDefinitions;
            }
            CHECK_FALSE(owner.TakeCompleted(request).Value());
            REQUIRE(owner.Shutdown(TestJoin).HasValue());
        }
        CHECK(preservedDefinitions > 0);
    }
}

TEST_CASE("Cancellation during completion memoization cannot publish a validated definition", "[native][prefab][jobs][allocation]") {
    ExpansionFixture fixture;
    JobSystem jobs{JobSystemConfig{.workerCount = 0}};
    const auto request = fixture.Request();
    CancellationSource cancellation;
    std::size_t allocations{};
    {
        ScenePrefabExpansionOwner measured{jobs};
        REQUIRE(measured.Submit(request, cancellation.Token()).HasValue());
        REQUIRE(measured.Join(TestJoin).HasValue());
        const auto completed = [&] {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto result = measured.TakeCompleted(request);
            allocations = measurement.Snapshot().requests;
            return result;
        }();
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value());
    }
    REQUIRE(allocations > 0);
    ScenePrefabExpansionOwner owner{jobs};
    REQUIRE(owner.Submit(request, cancellation.Token()).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());

    struct CancelAtAllocation final {
        const CancellationSource *source;
        std::size_t remaining;
        bool fired{};
    } state{&cancellation, allocations};

    const auto completed = [&] {
        Tests::AllocationProbe::ScopedObserver observer{[](std::size_t, void *context) noexcept {
            auto &state = *static_cast<CancelAtAllocation *>(context);
            if (state.remaining > 0 && --state.remaining == 0) {
                state.fired = true;
                state.source->RequestCancellation();
            }
        }, &state};
        return owner.TakeCompleted(request);
    }();
    REQUIRE(state.fired);
    REQUIRE(completed.HasError());
    CHECK(completed.ErrorValue().code.Value() == PrefabErrors::ResolutionStale.code.Value());
    CHECK_FALSE(owner.TakeCompleted(request).Value());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
}

TEST_CASE("Owned expansion bounds and malformed admission reject before scheduling and allow retry", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystem jobs{JobSystemConfig{.workerCount = 0}};
    ScenePrefabExpansionOwner owner{jobs};
    auto rejected = fixture.Request();
    SECTION("invalid UTF8 fails the existing canonical codec with a typed error") {
        rejected.document.objects.front().name = std::string(1, static_cast<char>(0xff));
    }
    SECTION("placement count cannot create unbounded keys and owned candidates") {
        rejected.document.prefabInstances.resize(PrefabHardLimits::SourceObjectCount + 1);
    }
    SECTION("source component payload is bounded before hexadecimal encoding") {
        Gameplay::SerializedComponent component;
        component.payload.resize(32U * 1024U * 1024U + 1);
        rejected.document.objects.front().components.gameplayComponents.push_back(std::move(component));
    }
    const auto result = owner.Submit(rejected);
    REQUIRE(result.HasError());
    CHECK(jobs.AdmissionSnapshot().queued == std::array<std::size_t, 3>{});
    const auto valid = fixture.Request();
    REQUIRE(owner.Submit(valid).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    REQUIRE(owner.TakeCompleted(valid).Value());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
}
