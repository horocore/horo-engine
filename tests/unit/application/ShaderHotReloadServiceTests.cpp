#include "ShaderHotReloadTestFixture.h"

using namespace Horo;
using namespace Horo::Application;
using namespace Horo::Application::ShaderHotReloadTests;

TEST_CASE("Shader reload adopts the owned complete program set on its owner", "[shader][hot_reload]") {
    Fixture fixture;
    fixture.adapter->hold = true;
    fixture.Create();
    auto request = Request(7);
    const auto admitted = fixture.service->Request(request);
    REQUIRE(admitted.HasValue());
    fixture.AwaitCompiler();
    request.programs.clear();
    REQUIRE(fixture.service->Poll().HasValue());
    CHECK(fixture.target.adoptions == 0);
    fixture.adapter->hold = false;
    fixture.Drain();
    CHECK(fixture.target.live == std::vector<std::uint8_t>{7, 8});
    CHECK(fixture.target.adoptions == 1);
    CHECK_FALSE(fixture.target.wrongThread);
    CHECK_FALSE(fixture.adapter->wrongThread);
    const auto snapshot = fixture.Snapshot();
    REQUIRE(snapshot.lastGood);
    CHECK(snapshot.lastGood->operation == admitted.Value().operation);
    CHECK(snapshot.lastGood->outcome == ShaderHotReloadOutcome::Adopted);
    REQUIRE(fixture.service->SnapshotIfChanged(snapshot.revision).HasValue());
    CHECK_FALSE(fixture.service->SnapshotIfChanged(snapshot.revision).Value());
}

TEST_CASE("Latest shader request replaces pending work and discards obsolete compilation", "[shader][hot_reload]") {
    Fixture fixture;
    fixture.adapter->hold = true;
    fixture.Create();
    REQUIRE(fixture.service->Request(Request(1)).HasValue());
    fixture.AwaitCompiler();
    const auto superseded = fixture.service->Request(Request(3));
    REQUIRE(superseded.HasValue());
    const auto latest = fixture.service->Request(Request(5));
    REQUIRE(latest.HasValue());
    const auto queued = fixture.Snapshot();
    CHECK(queued.pendingOperation == latest.Value().operation);
    REQUIRE(queued.lastResult);
    CHECK(queued.lastResult->operation == superseded.Value().operation);
    CHECK(queued.lastResult->outcome == ShaderHotReloadOutcome::Stale);
    fixture.adapter->hold = false;
    fixture.Drain();
    CHECK(fixture.target.live == std::vector<std::uint8_t>{5, 6});
    CHECK(fixture.target.adoptions == 1);
    CHECK(fixture.Snapshot().lastGood->operation == latest.Value().operation);
}

TEST_CASE("Shader compile and target failures preserve the last good complete generation", "[shader][hot_reload]") {
    Fixture fixture;
    fixture.Create();
    REQUIRE(fixture.service->Request(Request(2)).HasValue());
    fixture.Drain();
    const auto lastGood = fixture.Snapshot().lastGood->operation;
    const auto failure = GENERATE(0, 1, 2);
    fixture.adapter->failureMarker = failure == 0 ? 10 : 0;
    fixture.target.failPreparation = failure == 1;
    fixture.target.failAdoption = failure == 2;
    REQUIRE(fixture.service->Request(Request(9)).HasValue());
    fixture.Drain();
    CHECK(fixture.target.live == std::vector<std::uint8_t>{2, 3});
    CHECK(fixture.target.adoptions == 1);
    CHECK(fixture.Snapshot().lastGood->operation == lastGood);
    CHECK(fixture.Snapshot().lastResult->outcome == ShaderHotReloadOutcome::Failed);
}

TEST_CASE("Shader context and device fences reject obsolete results before preparation", "[shader][hot_reload]") {
    Fixture fixture;
    fixture.adapter->hold = true;
    fixture.Create();
    REQUIRE(fixture.service->Request(Request()).HasValue());
    fixture.AwaitCompiler();
    const bool deviceChanged = GENERATE(false, true);
    if (deviceChanged)
        ++fixture.target.state.deviceIncarnation;
    else {
        auto context = fixture.configuration.context;
        ++context.sourceGeneration;
        REQUIRE(fixture.service->SetContext(context).HasValue());
    }
    fixture.adapter->hold = false;
    fixture.Drain();
    CHECK(fixture.target.preparations == 0);
    CHECK(fixture.target.adoptions == 0);
    CHECK(fixture.Snapshot().lastResult->outcome == ShaderHotReloadOutcome::Stale);
}

TEST_CASE("Shader cancellation and shutdown join workers and preserve target state", "[shader][hot_reload]") {
    Fixture fixture;
    fixture.adapter->hold = true;
    fixture.Create();
    const auto active = fixture.service->Request(Request());
    REQUIRE(active.HasValue());
    fixture.AwaitCompiler();
    REQUIRE(fixture.service->Cancel(active.Value().operation).HasValue());
    fixture.Drain();
    CHECK(fixture.Snapshot().lastResult->outcome == ShaderHotReloadOutcome::Cancelled);
    REQUIRE(fixture.service->Request(Request()).HasValue());
    REQUIRE(fixture.service->Request(Request(4)).HasValue());
    REQUIRE(fixture.service->Shutdown().HasValue());
    CHECK(fixture.Snapshot().closed);
    CHECK_FALSE(fixture.Snapshot().activeOperation);
    CHECK_FALSE(fixture.Snapshot().pendingOperation);
    CHECK(fixture.target.adoptions == 0);
    CHECK(fixture.service->Request(Request()).HasError());
    CHECK(fixture.service->Shutdown().HasValue());
}

TEST_CASE("Shader reload rejects invalid roles and aggregate budgets before admission", "[shader][hot_reload]") {
    Fixture fixture;
    const auto malformed = GENERATE(0, 1, 2, 3);
    if (malformed == 3)
        fixture.configuration.maximumInputBytes = 4096;
    fixture.Create();
    auto request = Request();
    if (malformed == 0)
        request.programs[1].id = request.programs[0].id;
    if (malformed == 1)
        request.programs = Request(1, 9).programs;
    if (malformed == 2)
        request.programs[0].build.limits.maximumPayloadBytes = 128U * 1024U * 1024U;
    if (malformed == 3)
        request.programs[0].build.sources.push_back({"unused", std::filesystem::path(std::string(4096, 'a'))});
    CHECK(fixture.service->Request(request).HasError());
    CHECK_FALSE(fixture.Snapshot().activeOperation);
    CHECK(fixture.adapter->entered == 0);
}

TEST_CASE("Shader reload enforces development policy hard envelopes and owner affinity", "[shader][hot_reload]") {
    Fixture fixture;
    for (const auto policy : {ShaderHotReloadPolicy::Disabled, ShaderHotReloadPolicy::Packaged}) {
        fixture.configuration.policy = policy;
        CHECK(ShaderHotReloadService::Create(fixture.compiler, fixture.jobs, fixture.operations, fixture.output, fixture.target,
                                             fixture.configuration)
                  .HasError());
    }
    fixture.configuration.policy = ShaderHotReloadPolicy::Development;
    fixture.configuration.maximumInputBytes = 64U * 1024U * 1024U + 1;
    CHECK(ShaderHotReloadService::Create(fixture.compiler, fixture.jobs, fixture.operations, fixture.output, fixture.target,
                                         fixture.configuration)
              .HasError());
    fixture.configuration.maximumInputBytes = 64U * 1024U * 1024U;
    fixture.Create();
    auto wrongThread = std::async(std::launch::async, [&] {
        return fixture.service->Request(Request());
    });
    CHECK(wrongThread.get().HasError());
    CHECK(fixture.adapter->entered == 0);
}
