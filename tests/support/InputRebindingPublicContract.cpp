#include "Horo/Runtime/Input.h"

namespace {
    /** @brief Compile-only consumer for explicit unknown-controller trigger capture. */
    [[maybe_unused]] Horo::Result<Horo::Input::InputBindingCapture> BeginTriggerCapture(const Horo::Input::InputRouter &router,
                                                                                        const Horo::Input::InputContextToken &context,
                                                                                        const Horo::Input::ActionId &action) {
        return Horo::Input::InputBindingCapture::Create(router, context, action, Horo::Input::RawAxisCaptureRange::RestingNegativeOne);
    }
}  // namespace
