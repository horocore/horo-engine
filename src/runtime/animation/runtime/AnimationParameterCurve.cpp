#include "Horo/Animation/AnimationParameterCurve.h"

#include <algorithm>
#include <cmath>

namespace Horo::Animation {
    /** @copydoc AnimationParameterCurve::Create */
    Result<AnimationParameterCurve> AnimationParameterCurve::Create(GraphParameterCurveData data, const CancellationToken cancellation) {
        const auto fail = [](const ErrorCodeDescriptor &code) {
            return Result<AnimationParameterCurve>::Failure(MakeError(code));
        };
        if (cancellation.IsCancellationRequested())
            return fail(AnimationErrors::GraphOperationCancelled);
        if (data.version != 1)
            return fail(AnimationErrors::ContractVersionUnsupported);
        if (!data.parameter.IsValid())
            return fail(AnimationErrors::GraphBindingMismatch);
        if (data.keys.empty() || data.keys.size() > MaximumKeys)
            return fail(AnimationErrors::GraphLimitExceeded);
        if (data.interpolation != GraphCurveInterpolation::Step && data.interpolation != GraphCurveInterpolation::Linear)
            return fail(AnimationErrors::ContractVersionUnsupported);
        for (std::size_t index = 0; index < data.keys.size(); ++index) {
            if (cancellation.IsCancellationRequested())
                return fail(AnimationErrors::GraphOperationCancelled);
            const auto &key = data.keys[index];
            if (key.time.ticks < 0 || !std::isfinite(key.value) || (index > 0 && key.time.ticks <= data.keys[index - 1].time.ticks))
                return fail(AnimationErrors::GraphMalformed);
        }
        return Result<AnimationParameterCurve>::Success(AnimationParameterCurve{std::move(data)});
    }

    /** @copydoc AnimationParameterCurve::Sample */
    float AnimationParameterCurve::Sample(const AnimationTime time) const noexcept {
        if (time.ticks <= data_.keys.front().time.ticks)
            return data_.keys.front().value;
        if (time.ticks >= data_.keys.back().time.ticks)
            return data_.keys.back().value;
        const auto next = std::ranges::lower_bound(data_.keys, time.ticks, {}, [](const GraphCurveKey &key) {
            return key.time.ticks;
        });
        if (next->time.ticks == time.ticks)
            return next->value;
        const auto &previous = *(next - 1);
        if (data_.interpolation == GraphCurveInterpolation::Step)
            return previous.value;
        const auto fraction =
            static_cast<double>(time.ticks - previous.time.ticks) / static_cast<double>(next->time.ticks - previous.time.ticks);
        return static_cast<float>((1.0 - fraction) * static_cast<double>(previous.value) + fraction * static_cast<double>(next->value));
    }
}  // namespace Horo::Animation
