#include "PrefabMetricsTestSupport.h"
#include "PrefabSceneCookHostFixture.h"

namespace Horo::Application::PrefabCookTestSupport {
    TEST_CASE("Prefab cook host passes explicit process metrics through canonical source and template cooking",
              "[prefab][prefab-metrics][host]") {
        Prefab::MetricTest::Recorder recorder;
        HostFixture fixture;
        fixture.request.metrics = recorder.handles;
        fixture.request.runtimePrefabRoots = {fixture.prefabId};
        const auto cooked = fixture.Cook();
        REQUIRE(cooked.HasValue());
        recorder.Flush();
        for (const auto phase : {Prefab::PrefabMetricPhase::Parse, Prefab::PrefabMetricPhase::Graph, Prefab::PrefabMetricPhase::Resolve,
                                 Prefab::PrefabMetricPhase::Expand, Prefab::PrefabMetricPhase::Cook})
            CHECK(recorder.sink->Attempts(phase, Prefab::PrefabMetricOutcome::Success) >= 1);
    }
}  // namespace Horo::Application::PrefabCookTestSupport
