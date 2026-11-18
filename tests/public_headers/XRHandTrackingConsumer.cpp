#include "Horo/XR/XRHandTracking.h"
#include "Horo/XR/XRInteraction.h"
#include "Horo/XR/XRPlaneTracking.h"
#include "Horo/XR/XRSpatialHitTest.h"
#include "Horo/XR/XRTrackingSnapshot.h"
static_assert(Horo::XR::XRHandTrackingHardLimits::MaximumJoints == 52);
static_assert(Horo::XR::XRTrackingSnapshotLimits{}.maximumHandJoints == 0);
static_assert(Horo::XR::XRInteractionCandidates::MaximumProbes > 0);
