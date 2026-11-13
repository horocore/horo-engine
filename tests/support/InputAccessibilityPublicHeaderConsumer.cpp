#include "Horo/Runtime/InputAccessibility.h"

#include <utility>

int main() {
    auto processor = Horo::Input::DigitalAffordance::Create({});
    if (processor.HasError())
        return 1;
    auto state = std::move(processor).Value();
    Horo::Input::InputService input;
    input.BeginFrame(1, Horo::Input::InputSampleTime{1000});
    const auto &raw = input.CommitFrame();
    Horo::Input::InputBinding binding;
    binding.accessibility.digital.toggle = true;
    const Horo::Input::GyroscopeBindingPolicy gyroscope{Horo::Input::SensorAxis::Y, 2.0F};
    Horo::Input::RetiredInputActionRelease release;
    const bool unexpectedRelease = input.Router().TryDequeueRetiredActionRelease(release);
    const Horo::Input::ActionValue neutral;
    const Horo::Input::GameplayInputFrameBuilder frames{{}, {}, {}, {}};
    return binding.accessibility.IsEnabled() && gyroscope.IsValid() &&
                   Horo::Input::InputBindingProfile{}.schemaVersion == Horo::Input::CurrentInputBindingProfileSchema &&
                   raw.sampleTime == Horo::Input::InputSampleTime{1000} && !state.Cancel().button.down && !unexpectedRelease &&
                   !neutral.repeated && frames.LastCaptureStatus() == Horo::Input::ActionReadStatus::Unavailable
               ? 0
               : 1;
}
