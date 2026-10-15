#include <Horo/WorldStreaming/StreamingSourcePlanner.h>
#include <type_traits>

using namespace Horo::WorldStreaming;
static_assert(!std::is_copy_constructible_v<StreamingSourceRegistration>);
static_assert(std::is_nothrow_move_constructible_v<StreamingSourceRegistration>);
static_assert(!std::is_copy_constructible_v<StreamingSourcePlanner>);

namespace Horo::WorldStreaming::ConsumerCoverage {
    /** @brief Compiles actual caller operations through only WorldStreaming's staged public headers. */
    [[maybe_unused]] Result<StreamingTravelReadiness> PollDestination(StreamingSourcePlanner &planner, StreamingSourceRegistration &source,
                                                                      const StreamingSourceRevision revision,
                                                                      const StreamingTravelResidencyEvidence &evidence,
                                                                      const std::uint64_t time) {
        const auto progress = planner.Service(time);
        if (progress.HasError())
            return Result<StreamingTravelReadiness>::Failure(progress.ErrorValue());
        return planner.PollTravel(source, revision, evidence, time, 100);
    }
}  // namespace Horo::WorldStreaming::ConsumerCoverage
