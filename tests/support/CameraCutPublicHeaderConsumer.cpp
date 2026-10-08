#include "Horo/Cinematic/CameraCutRuntime.h"
#include "Horo/Runtime/Camera/CameraErrors.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::CameraService>);
static_assert(!std::is_copy_constructible_v<Horo::Cinematic::CinematicCameraPlayback>);
static_assert(
    std::is_same_v<Horo::Cinematic::SequenceCameraCutHook,
                   void (*)(const Horo::BorrowedCallbackContext &, const Horo::Cinematic::SequenceFrameCameraCutRequest &) noexcept>);

int main() {
    const auto camera = Horo::Runtime::CameraService::Create({{1, 1}, {1}, Horo::Runtime::CameraViewDomain::Runtime});
    return camera.HasValue() && !Horo::Runtime::CameraErrors::CameraUnavailable.code.Value().empty() ? 0 : 1;
}
