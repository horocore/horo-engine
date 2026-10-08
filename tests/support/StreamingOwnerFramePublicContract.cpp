#include <Horo/WorldStreaming/StreamingCellDirection.h>
#include <Horo/WorldStreaming/StreamingOwnerFrameBudget.h>
#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::WorldStreaming::StreamingOwnerFrameBudget>);

namespace Horo::WorldStreaming::ConsumerCoverage {
    /** @brief Compiles the operational publication and retirement signatures using only the target's public boundary. */
    [[maybe_unused]] Result<StreamingCellOperation> AdvanceCell(StreamingCellDirectionOwner &owner,
                                                                StreamingCellActivationTransaction &activation,
                                                                StreamingOwnerFrameBudget &frame, const std::uint64_t elapsed) {
        if (owner.Operation().State() == StreamingCellOperationState::Activating) {
            const auto committed =
                owner.CommitActivation(activation, StreamingCellActivationCommitPoint::CommitDeferredLifecycleChanges, frame, elapsed);
            if (committed.HasError())
                return Result<StreamingCellOperation>::Failure(committed.ErrorValue());
            return Result<StreamingCellOperation>::Success(owner.Operation());
        }
        return owner.PollRetirement(frame, elapsed);
    }
}  // namespace Horo::WorldStreaming::ConsumerCoverage
