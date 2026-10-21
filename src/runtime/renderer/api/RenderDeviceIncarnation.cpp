#include "Horo/Runtime/Render/RenderCapabilities.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"

#include <atomic>

namespace Horo::Render {
    /** @copydoc AcquireRenderDeviceIncarnation */
    Result<std::uint64_t> AcquireRenderDeviceIncarnation() {
        // One producer spans all peers. No descriptor or shutdown path invokes/reset it.
        static std::atomic<std::uint64_t> next{1};
        auto value = next.load(std::memory_order_relaxed);
        while (value != 0 && value != std::numeric_limits<std::uint64_t>::max())
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                return Result<std::uint64_t>::Success(value);
        return Result<std::uint64_t>::Failure(MakeError(RenderGraphExecutionErrors::DeviceIncarnationExhausted));
    }
}  // namespace Horo::Render
