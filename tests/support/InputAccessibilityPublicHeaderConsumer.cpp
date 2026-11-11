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
    return binding.accessibility.IsEnabled() &&
                   Horo::Input::InputBindingProfile{}.schemaVersion == Horo::Input::CurrentInputBindingProfileSchema &&
                   raw.sampleTime == Horo::Input::InputSampleTime{1000} && !state.Cancel().button.down
               ? 0
               : 1;
}
