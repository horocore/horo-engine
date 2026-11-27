#include "Horo/XR/XRFrameLifecycle.h"
#include "Horo/XR/XRProjectSettings.h"
#include "Horo/XR/XRReprojection.h"
#include "Horo/XR/XRSessionLifecycle.h"
#include "Horo/XR/XRSpatialAnchors.h"

#include <type_traits>

static_assert(std::is_copy_constructible_v<Horo::XR::XRProjectSettings>);
static_assert(!std::is_default_constructible_v<Horo::XR::XRProjectSettings>);
static_assert(static_cast<std::uint8_t>(Horo::XR::XRSessionEvent::InstanceLost) == 8);
static_assert(static_cast<std::uint8_t>(Horo::XR::XRSessionEvent::Ready) == 9);
using XRLegacyRecordWait = Horo::XR::XRFrameWaitOutcome (Horo::XR::XRFrameLifecycle::*)(Horo::XR::XRRenderPredictionTime, bool) noexcept;
static_assert(std::is_same_v<decltype(&Horo::XR::XRFrameLifecycle::RecordWait), XRLegacyRecordWait>);
static_assert(std::is_same_v<decltype(Horo::XR::XRReprojectionPolicy::maximumPredictionIntervalNanoseconds), std::int64_t>);
static_assert(!std::is_copy_constructible_v<Horo::XR::XRSpatialAnchors>);
static_assert(std::is_copy_constructible_v<Horo::XR::XRSpatialAnchorId>);

Horo::Result<Horo::XR::XRAnchorSnapshot> ReadXRSpatialAnchor(const Horo::XR::XRSpatialAnchors &anchors,
                                                             const Horo::XR::XRAnchorScope &scope, const Horo::XR::XRSpatialAnchorId &id,
                                                             Horo::XR::XRRuntimeTime now) {
    return anchors.Read(scope, id, now);
}

void ConsumeXRProjectSettings(const Horo::XR::XRProjectSettings &settings) {
    [[maybe_unused]] const auto revision = settings.Revision();
    [[maybe_unused]] const auto features = settings.Features();
    [[maybe_unused]] const auto &limits = settings.Limits();
}

Horo::XR::XRReprojectionStatus ConsumeXRReprojection(const Horo::XR::XRFrameLifecycle &frames, const Horo::XR::XRFrameId &frame,
                                                     const Horo::XR::XRViewRenderPlan &views,
                                                     const Horo::XR::XRReprojectionInputs &inputs) {
    return frames.ValidateReprojection(frame, views, inputs);
}

Horo::XR::XRFrameStatus ObserveXRReprojectionReference(Horo::XR::XRFrameLifecycle &frames, const Horo::XR::XRFrameId &frame,
                                                       const Horo::XR::XRPoseSample &sample) {
    return frames.RecordReprojectionReference(frame, sample);
}

Horo::XR::XRFrameWaitOutcome ObserveXRFrameWait(Horo::XR::XRFrameLifecycle &frames, const Horo::XR::XRRenderPredictionTime prediction,
                                                const Horo::Duration period) {
    return frames.RecordWaitWithNativePeriod(prediction, true, period);
}

std::optional<Horo::XR::XRObservedReprojectionHistory> ObserveXRReprojectionHistory(const Horo::XR::XRFrameLifecycle &frames,
                                                                                    const Horo::XR::XRFrameId &frame,
                                                                                    const Horo::XR::XRViewRenderPlan &views) {
    return frames.ObserveReprojectionHistory(frame, views);
}
