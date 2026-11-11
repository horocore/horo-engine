#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Runtime/Camera/CameraErrors.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::CameraService>);
static_assert(!std::is_copy_constructible_v<Horo::Cinematic::CinematicCameraPlayback>);
static_assert(Horo::Cinematic::SequenceFrameCameraCutKey{}.transitionTier == Horo::Cinematic::CameraTransitionTier::HardCut);
static_assert(Horo::Cinematic::SequenceFrameCameraCutKey{}.fallback == Horo::Cinematic::CameraTransitionFallback::Fail);
static_assert(Horo::Cinematic::SequenceFrameCameraCutRequest{}.transitionTier == Horo::Cinematic::CameraTransitionTier::HardCut);
static_assert(
    std::is_same_v<Horo::Cinematic::SequenceCameraCutHook,
                   void (*)(const Horo::BorrowedCallbackContext &, const Horo::Cinematic::SequenceFrameCameraCutRequest &) noexcept>);

int main() {
    const auto camera = Horo::Runtime::CameraService::Create({{1, 1}, {1}, Horo::Runtime::CameraViewDomain::Runtime});
    const auto baseline = Horo::Runtime::CameraService::Create({{2, 1}, {1}, Horo::Runtime::CameraViewDomain::Runtime},
                                                               Horo::Runtime::CameraTransitionSupport::HardCutOnly);
    return camera.HasValue() && baseline.HasValue() &&
                   baseline.Value().TransitionSupport() == Horo::Runtime::CameraTransitionSupport::HardCutOnly &&
                   !Horo::Runtime::CameraErrors::CameraUnavailable.code.Value().empty()
               ? 0
               : 1;
}
