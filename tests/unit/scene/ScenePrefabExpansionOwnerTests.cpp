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

TEST_CASE("Owned expansion accepts only complete current document source and settings evidence", "[native][prefab][jobs]") {
    ExpansionFixture fixture;
    JobSystem jobs;
    ScenePrefabExpansionOwner owner{jobs};
    auto request = fixture.Request();
    REQUIRE(owner.Submit(request).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    bool stale = true;
    SECTION("same evidence publishes canonical complete hierarchy") {
        stale = false;
    }
    SECTION("document revision advances") {
        ++request.revision.value;
    }
    SECTION("document session closes and another opens") {
        ++request.documentSession;
    }
    SECTION("same revision cannot hide authored object mutation") {
        request.document.objects.front().name = "Edited";
    }
    SECTION("placement transform is current input not cached state") {
        request.document.prefabInstances.front().rootTransform.translation.x = 9;
    }
    SECTION("source changes under same registry revision") {
        request.resolver = fixture.Resolver("Edited");
    }
    SECTION("all settings must match") {
        auto policy = fixture.limits.Policy();
        --policy.maximumRuntimeSpawnDepth;
        request.limits = PrefabLimitProfile::Create(policy).Value();
    }
    const auto result = owner.TakeCompleted(request);
    if (stale)
        CHECK(result.HasError());
    else {
        REQUIRE(result.HasValue());
        REQUIRE(result.Value());
        CHECK(result.Value()->Entities().size() == 3);
        CHECK(result.Value()->Entities()[1].localTransform.translation == Math::Vec3{3, 0, 0});
    }
    CHECK(owner.TakeCompleted(request).Value() == std::nullopt);
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
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
    REQUIRE(owner.Submit(request).HasValue());
    REQUIRE(owner.Join(TestJoin).HasValue());
    REQUIRE(owner.TakeCompleted(request).Value());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
    REQUIRE(owner.Shutdown(TestJoin).HasValue());
    REQUIRE(owner.ReplaceScene(TestJoin).HasValue());
    CHECK(owner.Submit(request).HasError());
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
        std::size_t allocations{};
        {
            ScenePrefabExpansionOwner owner{jobs};
            REQUIRE(owner.Submit(request).HasValue());
            REQUIRE(owner.Join(TestJoin).HasValue());
            Tests::AllocationProbe::ScopedMeasurement measurement;
            const auto result = owner.TakeCompleted(request);
            allocations = measurement.Snapshot().requests;
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
        }
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
        Tests::AllocationProbe::ScopedMeasurement measurement;
        const auto completed = measured.TakeCompleted(request);
        allocations = measurement.Snapshot().requests;
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
