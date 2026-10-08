#include <Horo/WorldStreaming/StreamingEvictionPolicy.h>

namespace Horo::WorldStreaming::ConsumerCoverage {
    /** @brief Verifies selection signatures use only declared public dependencies. */
    [[maybe_unused]] Result<std::size_t> SelectEviction(const StreamingEvictionPolicy &policy, const StreamingEvictionContext &context,
                                                        std::span<const StreamingEvictionCandidate> candidates,
                                                        std::span<StreamingEvictionVictim> output) {
        return SelectStreamingEvictionVictims(policy, context, candidates, output);
    }
}  // namespace Horo::WorldStreaming::ConsumerCoverage
