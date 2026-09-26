#include "Horo/Runtime/Input.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>
#include <vector>

namespace {
    using namespace Horo::Input;

    struct CaptureOwner final : IInputCaptureOwner {
        void OnInputCaptureCancelled(const CaptureCancellationReason reason) noexcept override {
            reasons.push_back(reason);
        }

        std::vector<CaptureCancellationReason> reasons;
    };

    TEST_CASE("Pointer capture ends on its initiating release without cancelling another gesture", "[unit][runtime][input]") {
        InputService input;
        input.BeginFrame(1);
        input.Collector().SetPointerButton(PointerButton::Primary, true);
        static_cast<void>(input.CommitFrame());
        auto context = input.Router().PushContext(InputContextId{"workspace"}, InputContextKind::EditorWorkspace);
        CaptureOwner owner;
        auto captured = input.Router().CapturePointer(context, PointerButton::Primary, owner);
        REQUIRE(captured.HasValue());
        auto token = std::move(captured).Value();
        REQUIRE(token.IsActive());

        input.BeginFrame(2);
        input.Collector().SetPointerButton(PointerButton::Secondary, true);
        static_cast<void>(input.CommitFrame());
        REQUIRE(token.IsActive());

        input.BeginFrame(3);
        input.Collector().SetPointerButton(PointerButton::Primary, false);
        static_cast<void>(input.CommitFrame());
        REQUIRE(token.IsActive());
        input.Router().EndFrame();
        REQUIRE_FALSE(token.IsActive());
        REQUIRE(owner.reasons == std::vector{CaptureCancellationReason::Released});
        token.Release();
        REQUIRE(owner.reasons.size() == 1);
    }

    TEST_CASE("Capture cannot outlive its owner or be stolen by a peer context", "[unit][runtime][input]") {
        InputRouter router;
        auto first = router.PushContext(InputContextId{"first"}, InputContextKind::EditorToolCapture);
        auto owner = std::make_unique<CaptureOwner>();
        auto captured = router.CapturePointer(first, PointerButton::Primary, *owner);
        REQUIRE(captured.HasValue());
        auto token = std::move(captured).Value();
        REQUIRE(router.HasCapture());

        CaptureOwner competitor;
        REQUIRE(router.CapturePointer(first, PointerButton::Secondary, competitor).HasError());
        auto second = router.PushContext(InputContextId{"second"}, InputContextKind::EditorToolCapture);
        REQUIRE(owner->reasons == std::vector{CaptureCancellationReason::ContextPreempted});
        REQUIRE_FALSE(token.IsActive());
        REQUIRE_FALSE(router.IsContextActive(first));
        REQUIRE(router.IsContextActive(second));

        auto secondCapture = router.CapturePointer(second, PointerButton::Primary, *owner);
        REQUIRE(secondCapture.HasValue());
        auto secondToken = std::move(secondCapture).Value();
        owner.reset();
        REQUIRE_FALSE(router.HasCapture());
        REQUIRE_FALSE(secondToken.IsActive());
        secondToken.Release();
        second.Reset();
        REQUIRE(router.IsContextActive(first));
    }

    TEST_CASE("Modal and native dialog barriers discard blocked transitions before lower contexts resume", "[unit][runtime][input]") {
        InputService input;
        auto workspace = input.Router().PushContext(InputContextId{"workspace"}, InputContextKind::EditorWorkspace);
        input.BeginFrame(1);
        input.Collector().SetKey(Key::A, true);
        static_cast<void>(input.CommitFrame());
        auto modal = input.Router().PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        REQUIRE_FALSE(input.Router().ConsumeKey(workspace, Key::A));
        modal.Reset();
        REQUIRE_FALSE(input.Router().ConsumeKey(workspace, Key::A));

        input.BeginFrame(2);
        input.Collector().SetKey(Key::A, false);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().IsContextActive(workspace));

        auto dialog = input.Router().PushContext(InputContextId{"dialog"}, InputContextKind::NativeDialog);
        dialog.Reset();
        input.BeginFrame(3);
        input.Collector().SetKey(Key::A, true);
        static_cast<void>(input.CommitFrame());
        REQUIRE_FALSE(input.Router().ConsumeKey(workspace, Key::A));
        input.BeginFrame(4);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().IsContextActive(workspace));
    }

    TEST_CASE("Focus and pointer-device loss neutralize held controls and composition", "[unit][runtime][input]") {
        InputService input;
        auto workspace = input.Router().PushContext(InputContextId{"workspace"}, InputContextKind::EditorWorkspace);
        input.BeginFrame(1);
        input.Collector().SetKey(Key::A, true);
        input.Collector().SetPointerButton(PointerButton::Primary, true);
        input.Collector().AppendText("a");
        input.Collector().SetTextComposition("preedit", 0, 7);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().IsContextActive(workspace));

        input.BeginFrame(2);
        input.Collector().SetWindowState({.focused = false});
        const RawInputSnapshot &lostFocus = input.CommitFrame();
        REQUIRE_FALSE(lostFocus.State(Key::A).down);
        REQUIRE(lostFocus.State(Key::A).released);
        REQUIRE_FALSE(lostFocus.State(PointerButton::Primary).down);
        REQUIRE(lostFocus.text.empty());
        REQUIRE_FALSE(lostFocus.composition.active);
        REQUIRE_FALSE(input.Router().IsContextActive(workspace));

        input.BeginFrame(3);
        input.Collector().SetWindowState({.focused = true, .pointerDeviceAvailable = true});
        input.Collector().SetPointerButton(PointerButton::Primary, true);
        static_cast<void>(input.CommitFrame());
        REQUIRE(input.Router().IsContextActive(workspace));
        input.BeginFrame(4);
        input.Collector().SetWindowState({.focused = true, .pointerDeviceAvailable = false});
        const RawInputSnapshot &lostDevice = input.CommitFrame();
        REQUIRE_FALSE(lostDevice.State(PointerButton::Primary).down);
        REQUIRE(lostDevice.State(PointerButton::Primary).released);
    }
}  // namespace
