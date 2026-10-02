#include "Horo/Runtime/Save/SaveCaptureBarrier.h"

#include <type_traits>

static_assert(!std::is_copy_constructible_v<Horo::Runtime::SaveCaptureBarrier>);

int main() {
    const Horo::Runtime::SaveCaptureBarrierSnapshot snapshot{};
    return snapshot.state == Horo::Runtime::SaveBarrierState::Idle ? 0 : 1;
}
