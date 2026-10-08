#include "Horo/Runtime/Save/SaveThumbnailArchive.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveThumbnailCapture>);

int main() {
    Horo::Runtime::SaveThumbnailCapture capture;
    capture.BeginShutdown();
    return capture.Snapshot().state == Horo::Runtime::SaveThumbnailCaptureState::Idle ? 0 : 1;
}
