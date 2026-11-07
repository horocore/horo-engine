#include "Horo/XR/XRHandTracking.h"
#include "Horo/XR/XRTrackingSnapshot.h"
static_assert(Horo::XR::XRHandTrackingHardLimits::MaximumJoints == 52);
static_assert(Horo::XR::XRTrackingSnapshotLimits{}.maximumHandJoints == 0);
