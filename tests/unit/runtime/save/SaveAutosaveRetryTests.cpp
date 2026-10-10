#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveAutosaveTestUtils.h"

namespace Horo::Runtime {
    TEST_CASE("Interval autosave keeps one detached capture during retry and cancels it on session replacement",
              "[unit][save][autosave][retry]") {
        using namespace AutosaveTestSupport;
        Fixture fixture;
        fixture.Sample(100);
        const SaveArbiterRetryDescriptor retry{.policy = {2, 10, 20, 100},
                                               .preconditions = {.access = {fixture.Address().nameSpace, 7},
                                                                 .runtime = fixture.generation,
                                                                 .catalogRevision = 1,
                                                                 .slot = fixture.Address().slot,
                                                                 .publicationGeneration = Test::Id<SlotGenerationId>(9)}};
        const auto captured =
            fixture.scheduler->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, fixture.generation, fixture.Operation(91),
                                                 fixture.Address(), fixture.provenance, fixture.participants, {}, retry);
        REQUIRE(captured.HasValue());
        REQUIRE(captured.Value());
        REQUIRE(fixture.arbiter
                    .DeferStorageRetry(91,
                                       {.category = SaveStorageFailureCategory::TransientIo,
                                        .nativeCause = MakeError(SaveErrors::StorageTransientIo)},
                                       0)
                    .Value());
        fixture.Sample(1'000);
        CHECK_FALSE(fixture.Poll(92).Value());
        CHECK(fixture.Snapshot().pending);
        CHECK_FALSE(fixture.Snapshot().blocked);
        CHECK(fixture.captures == 1);
        REQUIRE(fixture.arbiter.ResumeStorageRetry(91, retry.preconditions, 10).Value());
        REQUIRE(fixture.arbiter
                    .DeferStorageRetry(91,
                                       {.category = SaveStorageFailureCategory::TransientIo,
                                        .nativeCause = MakeError(SaveErrors::StorageTransientIo)},
                                       10)
                    .Value());
        auto next = fixture.generation;
        ++next.runtime;
        REQUIRE(fixture.scheduler->ReplaceSession({.generation = next}).HasValue());
        CHECK(captured.Value()->operation.Snapshot()->state == SaveOperationState::Cancelled);
        CHECK_FALSE(fixture.arbiter.ResumeStorageRetry(91, retry.preconditions, 30).Value());
        CHECK(fixture.captures == 1);
    }
}  // namespace Horo::Runtime
