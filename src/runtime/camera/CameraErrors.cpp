#include "Horo/Runtime/Camera/CameraErrors.h"

namespace Horo::Runtime::CameraErrors {
    namespace {
        /** @brief Declares inert camera-owned error metadata without ambient registration. */
        ErrorCodeDescriptor Describe(const char *code, const char *summary) {
            return {ErrorDomainId{"horo.runtime.camera"}, ErrorCode{code}, ErrorSeverity::Error, summary,
                    "Resolve the current view, scene and camera authority before retrying."};
        }
    }  // namespace

    const ErrorCodeDescriptor InvalidContext = Describe("CameraInvalidContext", "Camera view context is invalid or unsupported.");
    const ErrorCodeDescriptor Closed = Describe("CameraOwnerClosed", "Camera owner has closed proposal admission.");
    const ErrorCodeDescriptor InvalidLease = Describe("CameraInvalidLease", "Camera override lease is stale or foreign.");
    const ErrorCodeDescriptor CapacityExceeded = Describe("CameraCapacityExceeded", "Camera evaluation exceeded its declared bound.");
    const ErrorCodeDescriptor DuplicateClaim = Describe("CameraDuplicateClaim", "Camera claim is already leased.");
    const ErrorCodeDescriptor InvalidTarget = Describe("CameraInvalidTarget", "Camera target is unavailable or invalid.");
    const ErrorCodeDescriptor CameraUnavailable = Describe("CameraUnavailable", "No eligible camera can be committed for this view.");
    const ErrorCodeDescriptor InvalidFrame = Describe("CameraInvalidFrame", "Camera frame or selection epoch cannot advance.");
}  // namespace Horo::Runtime::CameraErrors
