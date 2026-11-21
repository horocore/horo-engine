#pragma once

#include "Horo/Runtime/Input.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace Horo::Input::Tests {
    /** @brief Real collector/router composition for capture lifecycle and persistence regressions. */
    struct CaptureFixture {
        InputService input;
        InputContextId gameplayId{"gameplay"};
        InputContextId pauseId{"pause.rebind"};
        InputContextToken gameplay{input.Router().PushContext(gameplayId, InputContextKind::Gameplay)};
        InputContextToken owner;
        std::optional<InputBindingCapture> capture;
        GamepadDeviceId pad;
        GamepadDeviceId raw;
        FrameNumber frame{1};

        explicit CaptureFixture(const InputContextKind ownerKind = InputContextKind::ModalRoot, const float rawRest = -1.0F,
                                const ActionValueType shape = ActionValueType::Digital,
                                const RawAxisCaptureRange range = RawAxisCaptureRange::Centered)
            : owner(input.Router().PushContext(pauseId, ownerKind)) {
            REQUIRE(input.Router()
                        .SetActionMap({{ActionId{"jump"}, shape, gameplayId, true, {{.kind = BindingControlKind::Key, .key = Key::Space}}},
                                       {ActionId{"pause.submit"},
                                        ActionValueType::Digital,
                                        pauseId,
                                        false,
                                        {{.kind = BindingControlKind::Key, .key = Key::J}}}})
                        .HasValue());
            input.BeginFrame(frame);
            pad = input.Collector().ConnectGamepad("canonical");
            raw = input.Collector().ConnectGamepad("unknown", false, 1, 1);
            REQUIRE(input.Collector().SetRawGamepadAxis(raw, 0, rawRest));
            (void)input.CommitFrame();
            auto created = InputBindingCapture::Create(input.Router(), owner, ActionId{"jump"}, range);
            REQUIRE(created.HasValue());
            capture.emplace(std::move(created).Value());
        }

        RawInputCollector &Begin() {
            input.BeginFrame(++frame);
            return input.Collector();
        }

        BindingCaptureResult Poll() {
            (void)input.CommitFrame();
            return capture->Poll(input.Router(), owner);
        }
    };

}  // namespace Horo::Input::Tests
