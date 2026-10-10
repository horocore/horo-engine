#include "Horo/Runtime/Ui/UiPointerInput.h"
#include "UiAnimationOwnerIntegrationFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Runtime::Ui::AnimationTests {
    namespace {
        class InputObserver final : public UiEventHandler {
        public:
            Result<UiEventResponse> Handle(UiElementHandle, const UiEventPhase phase, const UiRoutedEvent &event) override {
                if (phase != UiEventPhase::Target)
                    return Result<UiEventResponse>::Success({});
                ++counts[static_cast<std::size_t>(event.gesture->kind)];
                if (event.gesture->kind == UiGestureKind::Press) {
                    if (revokeCapture)
                        router->CancelCapture(Input::CaptureCancellationReason::FocusLost);
                    if (reconfigure)
                        REQUIRE(router->SetActionMap(DefaultUiPointerActions(Input::InputContextId{"ui.pointer"})).HasValue());
                    if (stopAdapter)
                        adapter->Shutdown();
                    if (stopHost)
                        host->Shutdown();
                    if (replaceInputFrame) {
                        input->BeginFrame(99);
                        (void)input->CommitFrame();
                    }
                    if (rebindDuringCallback) {
                        const auto rebound = adapter->Rebind(*pointerHost, *sequence, descriptor, targets);
                        rebindRejected =
                            rebound.HasError() && rebound.ErrorValue().code.Value() == UiErrors::EventDispatchReentrant.code.Value();
                    }
                }
                return Result<UiEventResponse>::Success({false, false, preventPress && event.gesture->kind == UiGestureKind::Press});
            }

            Result<void> ApplyDefault(UiElementHandle, const UiRoutedEvent &) override {
                ++defaults;
                return Result<void>::Success();
            }

            Input::InputRouter *router{};
            UiPointerInput *adapter{};
            RuntimeHost *host{};
            Input::InputService *input{};
            UiPointerInteractionHost *pointerHost{};
            std::uint64_t *sequence{};
            UiPointerInteractionDescriptor descriptor;
            std::array<UiPointerTargetPolicy, 1> targets{};
            std::array<std::uint32_t, static_cast<std::size_t>(UiGestureKind::Count)> counts{};
            std::uint32_t defaults{};
            bool revokeCapture{};
            bool reconfigure{};
            bool stopAdapter{};
            bool stopHost{};
            bool preventPress{};
            bool replaceInputFrame{};
            bool rebindDuringCallback{};
            bool rebindRejected{};
        };

        struct PointerHostFixture final {
            UiElementSlotAllocator issuer{std::move(UiElementSlotAllocator::Create(ReloadTests::Owner())).Value()};
            HostFixture runtime{{.allocator = &issuer, .focusAccess = true}};
            Input::InputService input;
            Input::InputContextToken context;
            UiAnimationFrameLease frame;
            std::optional<UiHitTestStore> hitOwner;
            std::optional<UiHitTestSnapshot> hit;
            UiResolvedScreenCanvas canvas{{100, 100}, {100, 100}, {64, 1}};
            InputObserver observer;
            std::unique_ptr<UiPointerInput> adapter;
            std::uint64_t sequence{1};

            PointerHostFixture() {
                REQUIRE(runtime.host->RunFrame().HasValue());
                frame = Frame(*runtime.participant);
                const auto receipt = Receipt(frame, 1);
                REQUIRE(runtime.participant->ApplyPresentation(receipt).HasValue());
                const auto &layout = frame.Layout().Descriptor();
                auto created = UiHitTestStore::Create(
                    {layout.instance, layout.canvas, layout.document, UiRenderMode::ScreenSpaceOverlay, {100, 100}, 8, 3});
                REQUIRE(created.HasValue());
                hitOwner.emplace(std::move(created).Value());
                std::vector<UiHitTestElement> projection;
                for (const auto &record : frame.Layout().Records())
                    projection.push_back(
                        {record.element, {}, {}, {}, record.element == frame.Controls().front().source.element ? 1 : 0, false, true, true});
                auto published = hitOwner->Publish(frame.Layout(), projection);
                REQUIRE(published.HasValue());
                hit.emplace(std::move(published).Value());
                auto actions = DefaultUiPointerActions(Input::InputContextId{"ui.pointer"});
                std::array<Input::ActionId, UiPointerActionCount> ids;
                for (std::size_t index = 0; index < ids.size(); ++index)
                    ids[index] = actions[index].id;
                REQUIRE(input.Router().SetActionMap(std::move(actions)).HasValue());
                context = input.Router().PushContext(Input::InputContextId{"ui.pointer"}, Input::InputContextKind::FocusedGuiWidget);
                const UiPointerCaptureContext owner{{ReloadTests::Owner(), 11, 1},
                                                    receipt.view,
                                                    layout.instance,
                                                    layout.canvas,
                                                    layout.document,
                                                    layout.sources.tree,
                                                    layout.interaction};
                const std::array policies{UiPointerTargetPolicy{frame.Controls().front().source.element, true, true}};
                auto admitted = UiPointerInput::Create(input.Router(), context, {owner}, policies, ids);
                REQUIRE(admitted.HasValue());
                adapter = std::move(admitted).Value();
                observer.router = &input.Router();
                observer.adapter = adapter.get();
                observer.host = runtime.host.get();
                observer.input = &input;
                observer.pointerHost = runtime.participant;
                observer.sequence = &sequence;
                observer.descriptor = {owner};
                observer.targets = policies;
            }

            UiPointerCaptureContext PointerOwner(const UiAnimationFrameLease &source) const {
                const auto &layout = source.Layout().Descriptor();
                return {{ReloadTests::Owner(), 11, 1}, Receipt(source, 2).view, layout.instance, layout.canvas, layout.document,
                        layout.sources.tree,           layout.interaction};
            }

            void Rebind(const UiAnimationFrameLease &source) {
                std::vector<UiHitTestElement> projection;
                for (const auto &record : source.Layout().Records())
                    projection.push_back({record.element,
                                          {},
                                          {},
                                          {},
                                          record.element == source.Controls().front().source.element ? 1 : 0,
                                          false,
                                          true,
                                          true});
                auto published = hitOwner->Publish(source.Layout(), projection);
                REQUIRE(published.HasValue());
                hit.emplace(std::move(published).Value());
                observer.descriptor = {PointerOwner(source)};
                observer.targets = {UiPointerTargetPolicy{source.Controls().front().source.element, true, true}};
                REQUIRE(adapter->Rebind(*runtime.participant, sequence, observer.descriptor, observer.targets).HasValue());
            }

            void SetContact(const bool touch, const bool down, const Input::TouchContactId contact = {1, 1}) {
                if (touch)
                    REQUIRE(input.Collector().SetTouchContact(contact, 10, 10, down) == Input::TouchCollectionStatus::Accepted);
                else
                    input.Collector().SetPointerButton(Input::PointerButton::Primary, down);
            }

            Result<UiPointerInputFrame> Pump(const std::uint64_t time) {
                (void)input.CommitFrame();
                const UiPointerInputSurface surface{*runtime.participant, *hit, canvas, {0, 0, 100, 100}, observer, sequence};
                return adapter->Pump(input.Router(), context, surface, time);
            }

            void Begin(const Input::FrameNumber number) {
                input.BeginFrame(number);
                input.Collector().SetPointerPosition(10, 10);
            }
        };

        /** @brief Ends the disarmed held contact and proves only a fresh source generation can press again. */
        void RequireFreshContact(PointerHostFixture &fixture, const bool touch, const bool releaseFrameStarted = false) {
            if (!releaseFrameStarted)
                fixture.Begin(4);
            fixture.SetContact(touch, false);
            REQUIRE(fixture.Pump(4).HasValue());
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 0);
            fixture.Begin(5);
            fixture.SetContact(touch, true, {1, 2});
            const auto fresh = fixture.Pump(5);
            REQUIRE(fresh.HasValue());
            CHECK(fresh.Value().interaction.activePointers == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Press)] == 2);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Drop)] == 0);
        }

        TEST_CASE("Committed mouse and touch route through the real runtime-owned presented canvas", "[runtime_ui][pointer][integration]") {
            const bool touch = GENERATE(false, true);
            PointerHostFixture fixture;
            fixture.Begin(1);
            if (touch)
                REQUIRE(fixture.input.Collector().SetTouchContact({1, 1}, 10, 10, true) == Input::TouchCollectionStatus::Accepted);
            else
                fixture.input.Collector().SetPointerButton(Input::PointerButton::Primary, true);
            auto press = fixture.Pump(1);
            REQUIRE(press.HasValue());
            CHECK(press.Value().interaction.activePointers == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Press)] == 1);
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto pressed = Frame(*fixture.runtime.participant);
            CHECK(std::get<UiTextInputControlState>(pressed.Controls().front().state).editing);
            fixture.Begin(2);
            if (touch)
                REQUIRE(fixture.input.Collector().SetTouchContact({1, 1}, 10, 10, false) == Input::TouchCollectionStatus::Accepted);
            else
                fixture.input.Collector().SetPointerButton(Input::PointerButton::Primary, false);
            auto released = fixture.Pump(2);
            REQUIRE(released.HasValue());
            CHECK(released.Value().interaction.activePointers == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Release)] == 1);
            CHECK_FALSE(fixture.input.Router().HasCapture());
            pressed = {};
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto tapped = Frame(*fixture.runtime.participant);
            CHECK(std::get<UiTextInputControlState>(tapped.Controls().front().state).editing);
        }

        TEST_CASE("Real pointer callback revocation cannot authorize an aggregate default",
                  "[runtime_ui][pointer][integration][lifetime]") {
            const auto mode = GENERATE(0, 1, 2, 3, 4);
            PointerHostFixture fixture;
            fixture.observer.revokeCapture = mode == 0;
            fixture.observer.reconfigure = mode == 1;
            fixture.observer.stopAdapter = mode == 2;
            fixture.observer.stopHost = mode == 3;
            fixture.observer.replaceInputFrame = mode == 4;
            fixture.Begin(1);
            fixture.input.Collector().SetPointerButton(Input::PointerButton::Primary, true);
            REQUIRE(fixture.Pump(1).HasError());
            CHECK(fixture.observer.defaults == 0);
            CHECK(fixture.adapter->Defaults().empty());
            CHECK_FALSE(fixture.input.Router().HasCapture());
            if (mode != 3) {
                REQUIRE(fixture.runtime.host->RunFrame().HasValue());
                auto current = Frame(*fixture.runtime.participant);
                CHECK_FALSE(std::get<UiTextInputControlState>(current.Controls().front().state).editing);
            }
        }

        TEST_CASE("Prevented real pointer press neither changes control state nor retains a UI contact",
                  "[runtime_ui][pointer][integration][prevention]") {
            PointerHostFixture fixture;
            fixture.observer.preventPress = true;
            fixture.Begin(1);
            fixture.input.Collector().SetPointerButton(Input::PointerButton::Primary, true);
            auto pumped = fixture.Pump(1);
            REQUIRE(pumped.HasValue());
            CHECK(pumped.Value().interaction.defaultPrevented);
            CHECK(pumped.Value().interaction.activePointers == 0);
            CHECK(fixture.observer.defaults == 0);
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto current = Frame(*fixture.runtime.participant);
            CHECK_FALSE(std::get<UiTextInputControlState>(current.Controls().front().state).editing);
        }

        TEST_CASE("Callback rebind rejects without replacing the actual held pointer generation",
                  "[runtime_ui][pointer][integration][lifetime][rebind]") {
            PointerHostFixture fixture;
            fixture.observer.rebindDuringCallback = true;
            fixture.Begin(1);
            fixture.SetContact(false, true);
            const auto pressed = fixture.Pump(1);
            REQUIRE(pressed.HasValue());
            CHECK(fixture.observer.rebindRejected);
            CHECK(pressed.Value().interaction.activePointers == 1);
            CHECK(fixture.input.Router().HasCapture());
            fixture.Begin(2);
            fixture.SetContact(false, false);
            const auto released = fixture.Pump(2);
            REQUIRE(released.HasValue());
            CHECK(released.Value().interaction.activePointers == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 1);
            CHECK_FALSE(fixture.input.Router().HasCapture());
        }

        TEST_CASE("Keyboard pick drop and cancellation use the same actual presented route",
                  "[runtime_ui][pointer][integration][accessibility]") {
            PointerHostFixture fixture;
            fixture.Begin(1);
            fixture.input.Collector().SetKey(Input::Key::Space, true);
            REQUIRE(fixture.Pump(1).HasValue());
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::DragBegin)] == 1);
            fixture.Begin(2);
            fixture.input.Collector().SetKey(Input::Key::Space, false);
            REQUIRE(fixture.Pump(2).HasValue());
            fixture.Begin(3);
            fixture.input.Collector().SetKey(Input::Key::Enter, true);
            REQUIRE(fixture.Pump(3).HasValue());
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Drop)] == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 0);
            fixture.Begin(4);
            fixture.input.Collector().SetKey(Input::Key::Enter, false);
            REQUIRE(fixture.Pump(4).HasValue());
            fixture.Begin(5);
            fixture.input.Collector().SetKey(Input::Key::Enter, true);
            fixture.input.Collector().SetKey(Input::Key::Escape, true);
            REQUIRE(fixture.Pump(5).HasValue());
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Cancel)] == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 0);
            CHECK(fixture.adapter->Defaults().empty());
        }

        TEST_CASE("Published geometry replacement disarms old contacts instead of replaying held input",
                  "[runtime_ui][pointer][integration][generation]") {
            const bool touch = GENERATE(false, true);
            PointerHostFixture fixture;
            fixture.Begin(1);
            fixture.SetContact(touch, true);
            REQUIRE(fixture.Pump(1).HasValue());
            REQUIRE(fixture.runtime.participant->Start(Stable<UiAnimationId>(1)).HasValue());
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            fixture.runtime.clock.Advance(Duration::FromMilliseconds(10));
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto current = Frame(*fixture.runtime.participant);
            REQUIRE(current.Layout().Descriptor().interaction != fixture.frame.Layout().Descriptor().interaction);
            REQUIRE(fixture.runtime.participant->ApplyPresentation(Receipt(current, 2)).HasValue());
            CHECK_FALSE(fixture.runtime.participant->PointerInputEligible(fixture.observer.descriptor.owner));
            CHECK(fixture.runtime.participant->PointerInputEligible(fixture.PointerOwner(current)));
            fixture.Begin(2);
            fixture.input.Collector().SetKey(Input::Key::Space, true);
            auto stale = fixture.Pump(2);
            REQUIRE(stale.HasValue());
            CHECK(stale.Value().status == UiPointerInputStatus::NeedsRebind);
            CHECK_FALSE(fixture.input.Router().HasCapture());
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Press)] == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::DragUpdate)] == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Drop)] == 0);
            CHECK(stale.Value().interaction.activePointers == 0);
            CHECK(fixture.adapter->Defaults().empty());
            CHECK(fixture.input.Router().ReadAction(fixture.context, Input::ActionId{"ui.pointer.pick"}).pressed);
            fixture.Rebind(current);
            fixture.Begin(3);
            const auto held = fixture.Pump(3);
            REQUIRE(held.HasValue());
            CHECK(held.Value().status == UiPointerInputStatus::Active);
            CHECK(held.Value().interaction.activePointers == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Press)] == 1);
            CHECK_FALSE(fixture.input.Router().HasCapture());
            fixture.Begin(4);
            fixture.input.Collector().SetKey(Input::Key::Space, false);
            RequireFreshContact(fixture, touch, true);
            current = {};
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            const auto freshControl = Frame(*fixture.runtime.participant);
            CHECK(std::get<UiTextInputControlState>(freshControl.Controls().front().state).editing);
        }

        TEST_CASE("Keyboard cancellation neutralizes the real held control after focus is cleared",
                  "[runtime_ui][pointer][integration][accessibility][cancellation]") {
            PointerHostFixture fixture;
            fixture.Begin(1);
            fixture.input.Collector().SetPointerButton(Input::PointerButton::Primary, true);
            REQUIRE(fixture.Pump(1).HasValue());
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto held = Frame(*fixture.runtime.participant);
            REQUIRE(std::get<UiTextInputControlState>(held.Controls().front().state).editing);
            REQUIRE(fixture.runtime.attachedFocus != nullptr);
            REQUIRE(fixture.runtime.attachedFocus->ClearFocus().HasValue());
            fixture.Begin(2);
            fixture.input.Collector().SetKey(Input::Key::Escape, true);
            const auto cancelled = fixture.Pump(2);
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value().interaction.activePointers == 0);
            CHECK(fixture.adapter->Defaults().empty());
            held = {};
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto neutral = Frame(*fixture.runtime.participant);
            CHECK_FALSE(std::get<UiTextInputControlState>(neutral.Controls().front().state).editing);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Drop)] == 0);
        }

        TEST_CASE("Successful real-owner pointer pumping performs no C++ allocation", "[runtime_ui][pointer][integration][allocation]") {
            PointerHostFixture fixture;
            fixture.Begin(1);
            fixture.input.Collector().SetPointerButton(Input::PointerButton::Primary, true);
            (void)fixture.input.CommitFrame();
            const UiPointerInputSurface surface{*fixture.runtime.participant,
                                                *fixture.hit,
                                                fixture.canvas,
                                                {0, 0, 100, 100},
                                                fixture.observer,
                                                fixture.sequence};
            const auto before = Horo::Tests::AllocationProbe::Count();
            auto pumped = fixture.adapter->Pump(fixture.input.Router(), fixture.context, surface, 1);
            const auto after = Horo::Tests::AllocationProbe::Count();
            REQUIRE(pumped.HasValue());
            CHECK(after == before);
        }

        TEST_CASE("Real structural reload retires held pointer authority without activating the replacement",
                  "[runtime_ui][pointer][integration][reload]") {
            PointerHostFixture fixture;
            fixture.Begin(1);
            REQUIRE(fixture.input.Collector().SetTouchContact({1, 1}, 10, 10, true) == Input::TouchCollectionStatus::Accepted);
            REQUIRE(fixture.Pump(1).HasValue());
            auto replacement = Generation(fixture.issuer, 100);
            auto resources = Resources(replacement.Canvases().front());
            REQUIRE(fixture.runtime.participant
                        ->Reload(std::move(replacement), fixture.issuer, std::move(resources.registry), std::move(resources.styles),
                                 std::move(resources.definition),
                                 {UiAnimationReloadPolicy::Cancel, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands})
                        .HasValue());
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            auto current = Frame(*fixture.runtime.participant);
            REQUIRE(current.Controls().front().source.element != fixture.frame.Controls().front().source.element);
            REQUIRE(fixture.runtime.participant->ApplyPresentation(Receipt(current, 2)).HasValue());
            CHECK_FALSE(fixture.runtime.participant->PointerInputEligible(fixture.observer.descriptor.owner));
            CHECK(fixture.runtime.participant->PointerInputEligible(fixture.PointerOwner(current)));
            fixture.Begin(2);
            auto retired = fixture.Pump(2);
            REQUIRE(retired.HasValue());
            CHECK(retired.Value().status == UiPointerInputStatus::NeedsRebind);
            CHECK(retired.Value().interaction.activePointers == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Press)] == 1);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Tap)] == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Drop)] == 0);
            CHECK(fixture.adapter->Defaults().empty());
            fixture.Rebind(current);
            fixture.Begin(3);
            const auto held = fixture.Pump(3);
            REQUIRE(held.HasValue());
            CHECK(held.Value().interaction.activePointers == 0);
            CHECK(fixture.observer.counts[static_cast<std::size_t>(UiGestureKind::Press)] == 1);
            RequireFreshContact(fixture, true);
            current = {};
            REQUIRE(fixture.runtime.host->RunFrame().HasValue());
            const auto freshControl = Frame(*fixture.runtime.participant);
            CHECK(std::get<UiTextInputControlState>(freshControl.Controls().front().state).editing);
            fixture.adapter->Shutdown();
            fixture.Begin(6);
            const auto stopped = fixture.Pump(6);
            REQUIRE(stopped.HasValue());
            CHECK(stopped.Value().status == UiPointerInputStatus::Stopped);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui::AnimationTests
