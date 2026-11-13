#include "Horo/Runtime/Input.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

    TEST_CASE("Explicit inspection propagation retains gameplay and exclusive consumption without bypassing other contexts",
              "[unit][runtime][input]") {
        InputService input;
        input.BeginFrame(1);
        input.Collector().SetKey(Key::A, true);
        input.Collector().SetKey(Key::B, true);
        input.Collector().SetPointerButton(PointerButton::Primary, true);
        static_cast<void>(input.CommitFrame());
        auto &router = input.Router();
        auto gameplay = router.PushContext(InputContextId{"gameplay"}, InputContextKind::Gameplay);
        CaptureOwner owner;
        auto captured = router.CapturePointer(gameplay, PointerButton::Primary, owner);
        REQUIRE(captured.HasValue());
        auto capture = std::move(captured).Value();
        auto inspection = router.PushContext(InputContextId{"inspection"}, InputContextKind::FocusedGuiWidget,
                                             InputContextPropagation::GameplayPassThrough);
        REQUIRE(inspection.IsActive());
        REQUIRE(router.IsContextActive(inspection));
        REQUIRE(router.IsContextActive(gameplay));
        REQUIRE(capture.IsActive());
        REQUIRE(owner.reasons.empty());
        REQUIRE(router.FocusText(inspection));
        CHECK_FALSE(router.FocusText(gameplay));
        CHECK(router.ConsumeKey(inspection, Key::A));
        CHECK_FALSE(router.ConsumeKey(gameplay, Key::A));
        CHECK(router.ConsumeKey(gameplay, Key::B));

        SECTION("Modal and native dialog barriers override passing inspection") {
            const auto kind = GENERATE(InputContextKind::ModalRoot, InputContextKind::NativeDialog);
            auto barrier = router.PushContext(InputContextId{"barrier"}, kind);
            CHECK_FALSE(router.IsContextActive(inspection));
            CHECK_FALSE(router.IsContextActive(gameplay));
            CHECK_FALSE(capture.IsActive());
            CHECK(owner.reasons == std::vector{CaptureCancellationReason::ModalOpened});
        }
        SECTION("Ordinary focused widgets retain exclusion and capture preemption") {
            auto ordinary = router.PushContext(InputContextId{"ordinary"}, InputContextKind::FocusedGuiWidget);
            CHECK(router.IsContextActive(ordinary));
            CHECK_FALSE(router.IsContextActive(inspection));
            CHECK_FALSE(router.IsContextActive(gameplay));
            CHECK_FALSE(capture.IsActive());
            CHECK(owner.reasons == std::vector{CaptureCancellationReason::ContextPreempted});
        }
        SECTION("An editor context still blocks gameplay without gaining eligibility from inspection") {
            auto workspace = router.PushContext(InputContextId{"workspace"}, InputContextKind::EditorWorkspace);
            CHECK_FALSE(router.IsContextActive(workspace));
            CHECK_FALSE(router.IsContextActive(gameplay));
            CHECK(router.IsContextActive(inspection));
        }
        SECTION("Window focus loss neutralizes both contexts and capture") {
            input.BeginFrame(2);
            input.Collector().SetWindowState({.focused = false});
            static_cast<void>(input.CommitFrame());
            CHECK_FALSE(router.IsContextActive(inspection));
            CHECK_FALSE(router.IsContextActive(gameplay));
            CHECK_FALSE(capture.IsActive());
        }
        SECTION("Invalid propagation cannot install a modal barrier or revoke an existing capture") {
            auto invalidModal = router.PushContext(InputContextId{"invalid.modal"}, InputContextKind::ModalRoot,
                                                   InputContextPropagation::GameplayPassThrough);
            auto invalidPolicy = router.PushContext(InputContextId{"invalid.policy"}, InputContextKind::FocusedGuiWidget,
                                                    static_cast<InputContextPropagation>(255));
            auto invalidKind =
                router.PushContext(InputContextId{"invalid.kind"}, static_cast<InputContextKind>(255), InputContextPropagation::Exclusive);
            CHECK_FALSE(invalidModal.IsActive());
            CHECK_FALSE(invalidPolicy.IsActive());
            CHECK_FALSE(invalidKind.IsActive());
            CHECK(router.IsContextActive(inspection));
            CHECK(router.IsContextActive(gameplay));
            CHECK(capture.IsActive());
            CHECK(owner.reasons.empty());
        }
    }

    TEST_CASE("Passing inspection respects exclusive mouse capture for raw consumption and mapped controls", "[unit][runtime][input]") {
        const bool gameplayOwnsCapture = GENERATE(false, true);
        InputService input;
        auto &router = input.Router();
        const InputContextId gameplayId{"gameplay"}, inspectionId{"inspection"};
        const InputBinding mouse{.kind = BindingControlKind::PointerButton, .pointerButton = PointerButton::Primary};
        const InputBinding wheelX{.kind = BindingControlKind::PointerWheelX};
        const InputBinding wheelY{.kind = BindingControlKind::PointerWheelY};
        const InputBinding key{.kind = BindingControlKind::Key, .key = Key::C};
        REQUIRE(router
                    .SetActionMap({{ActionId{"gameplay.mouse"}, ActionValueType::Axis1D, gameplayId, false, {mouse, wheelX, wheelY}},
                                   {ActionId{"inspection.mouse"}, ActionValueType::Axis1D, inspectionId, false, {mouse, wheelX, wheelY}},
                                   {ActionId{"gameplay.key"}, ActionValueType::Digital, gameplayId, false, {key}},
                                   {ActionId{"inspection.key"}, ActionValueType::Digital, inspectionId, false, {key}}})
                    .HasValue());
        input.BeginFrame(1);
        input.Collector().SetPointerButton(PointerButton::Primary, true);
        input.Collector().AddPointerWheel(1, 2);
        input.Collector().SetKey(Key::C, true);
        static_cast<void>(input.CommitFrame());
        auto gameplay = router.PushContext(gameplayId, InputContextKind::Gameplay);
        auto inspection =
            router.PushContext(inspectionId, InputContextKind::FocusedGuiWidget, InputContextPropagation::GameplayPassThrough);
        const auto &ownerContext = gameplayOwnsCapture ? gameplay : inspection;
        const auto &peerContext = gameplayOwnsCapture ? inspection : gameplay;
        const ActionId ownerAction{gameplayOwnsCapture ? "gameplay.mouse" : "inspection.mouse"};
        const ActionId peerAction{gameplayOwnsCapture ? "inspection.mouse" : "gameplay.mouse"};
        CaptureOwner owner;
        auto captured = router.CapturePointer(ownerContext, PointerButton::Primary, owner);
        REQUIRE(captured.HasValue());
        auto capture = std::move(captured).Value();
        REQUIRE(router.IsContextActive(ownerContext));
        REQUIRE(router.IsContextActive(peerContext));
        CHECK_FALSE(router.ConsumePointerButton(peerContext, PointerButton::Primary));
        CHECK_FALSE(router.ConsumePointerMotion(peerContext));
        const auto blocked = router.ReadAction(peerContext, peerAction);
        CHECK(blocked.x == 0);
        CHECK_FALSE(blocked.down);
        CHECK_FALSE(blocked.pressed);
        CHECK_FALSE(blocked.released);
        CHECK(router.ReadAction(peerContext, ActionId{gameplayOwnsCapture ? "inspection.key" : "gameplay.key"}).pressed);
        CHECK(router.ConsumePointerButton(ownerContext, PointerButton::Primary));
        CHECK(router.ConsumePointerMotion(ownerContext));
        CHECK(router.ReadAction(ownerContext, ownerAction).x > 0);
        CHECK(capture.IsActive());
        CHECK(owner.reasons.empty());

        capture.Release();
        input.BeginFrame(2);
        input.Collector().AddPointerWheel(1, 1);
        static_cast<void>(input.CommitFrame());
        CHECK(router.ConsumePointerMotion(peerContext));
        CHECK(router.ReadAction(peerContext, peerAction).x > 0);
    }
}  // namespace
