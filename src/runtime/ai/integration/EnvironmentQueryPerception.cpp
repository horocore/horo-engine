#include "Horo/AI/EnvironmentQueryPerception.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::AI {
    namespace {
        [[nodiscard]] bool ValidTest(const QueryPerceptionTest &test) noexcept {
            return test.stage.IsValid() && test.agent.IsValid() && test.revision != 0 &&
                   test.match <= QueryPerceptionMatch::CurrentlySensed && (!test.sense || test.sense->IsValid());
        }

        [[nodiscard]] bool Matches(const PerceivedStimulus &entry, const PerceptionSourceRef &candidate,
                                   const QueryPerceptionTest &test) noexcept {
            return entry.key.source == candidate && (!test.sense || entry.key.sense == *test.sense) &&
                   (test.match == QueryPerceptionMatch::Remembered || entry.isCurrentlySensed);
        }
    }  // namespace

    /** @copydoc EvaluateQueryPerception */
    Result<QueryTestSample> EvaluateQueryPerception(const QueryPerceivedEntitySource &source, const QueryPerceptionTest &test,
                                                    const Runtime::EntityRef candidate) {
        using TestResult = Result<QueryTestSample>;
        if (!ValidTest(test))
            return TestResult::Failure(MakeError(AIErrors::EnvironmentQuerySchemaInvalid));
        auto current = source.publication.Current(source.scene, source.filtering, source.blackboard);
        if (current.HasError())
            return TestResult::Failure(std::move(current).ErrorValue());
        const auto &publication = current.Value();
        if (publication.agent != test.agent || publication.revision != test.revision || source.scene.Get(candidate).HasError())
            return TestResult::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto projected = ProjectPerceptionSource(candidate);
        QueryTestSample sample{.stage = test.stage, .filter = false, .metric = 0.0};
        for (const auto &entry : std::span{publication.memory.entries}.first(publication.memory.count)) {
            if (!Matches(entry, projected, test))
                continue;
            sample.filter = true;
            sample.metric = std::max(*sample.metric, entry.confidence);
        }
        return TestResult::Success(sample);
    }
}  // namespace Horo::AI
