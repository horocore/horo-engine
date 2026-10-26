#include "Horo/Runtime/InputCollection.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>

namespace Horo::Input {
    namespace {
        struct Fixture final {
            InputRouter router;
            RawInputCollector collector;
            ActionDescriptor action{ActionId{"play.jump"},
                                    ActionValueType::Digital,
                                    InputContextId{"play"},
                                    false,
                                    {{.kind = BindingControlKind::Key, .key = Key::Space}}};
            InputContextToken context = router.PushContext(InputContextId{"play"}, InputContextKind::Gameplay);

            InputActionCollection Port() {
                auto result = router.ActionCollection(context);
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            Fixture() {
                REQUIRE(router.SetActionMap({action}).HasValue());
            }

            InputActionSourceToken Source(const InputActionCollection &port) {
                auto result = port.RegisterSource(std::span{&action, 1});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            InputCollectionFrame Frame(const InputActionCollection &port, const FrameNumber number) {
                collector.BeginFrame(number);
                auto result = port.Begin(collector.Commit());
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }
        };

        constexpr std::array pressed{CollectedActionValue{0, 1.0F, 0.0F, true}};
    }  // namespace

    TEST_CASE("Input reserved source validation retains exact lifetime thread and configuration fences", "[unit][input][collection]") {
        Fixture fixture;
        auto source = fixture.Source(fixture.Port());
        const auto before = Tests::AllocationProbe::Count();
        const auto valid = source.Validate();
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(valid.HasValue());
        CHECK(before == after);
        bool rejected{};
        std::thread foreign{[&] {
            rejected = source.Validate().HasError();
        }};
        foreign.join();
        CHECK(rejected);
        auto moved = std::move(source);
        CHECK(source.Validate().HasError());
        REQUIRE(moved.Validate().HasValue());
        REQUIRE(fixture.router.SetActionMap({fixture.action}).HasValue());
        CHECK(moved.Validate().HasError());
        REQUIRE(moved.Reset().HasValue());
        CHECK(moved.Validate().HasError());
    }

    TEST_CASE("Input source validation rejects expired router without dereferencing retired storage", "[unit][input][collection]") {
        InputActionSourceToken retained;
        {
            Fixture fixture;
            retained = fixture.Source(fixture.Port());
            REQUIRE(retained.Validate().HasValue());
        }
        CHECK(retained.Validate().HasError());
    }

    TEST_CASE("Input collection publishes only at commit and consumes an admitted edge once", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto frame = fixture.Frame(port, 1);
        REQUIRE(frame.Append(source, pressed).HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        REQUIRE(frame.Commit().HasValue());
        auto first = fixture.router.ReadAction(fixture.context, fixture.action.id);
        CHECK(first.down);
        CHECK(first.pressed);
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
        CHECK(frame.Append(source, pressed).HasError());
        CHECK(frame.Commit().HasError());
        auto missing = fixture.Frame(port, 2);
        REQUIRE(missing.Commit().HasValue());
        const auto released = fixture.router.ReadAction(fixture.context, fixture.action.id);
        CHECK_FALSE(released.down);
        CHECK(released.released);
    }

    TEST_CASE("Input collection validation is atomic and cancellation cannot publish or revive values", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto frame = fixture.Frame(port, 1);
        const std::array invalid{pressed[0], CollectedActionValue{1, 1.0F, 0.0F, true}};
        CHECK(frame.Append(source, invalid).HasError());
        REQUIRE(frame.Append(source, pressed).HasValue());
        REQUIRE(frame.Cancel().HasValue());
        CHECK(frame.Commit().HasError());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        auto next = fixture.Frame(port, 2);
        REQUIRE(next.Commit().HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
    }

    TEST_CASE("Input collection rejects duplicate and malformed payloads without partial admission", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto frame = fixture.Frame(port, 1);
        const std::array duplicate{pressed[0], pressed[0]};
        CHECK(frame.Append(source, duplicate).HasError());
        for (const auto value :
             {CollectedActionValue{0, std::numeric_limits<float>::quiet_NaN(), 0, true}, CollectedActionValue{0, 0.5F, 0, true},
              CollectedActionValue{0, 1, 1, true}, CollectedActionValue{0, 1, 0, false}}) {
            CHECK(frame.Append(source, std::span{&value, 1}).HasError());
        }
        REQUIRE(frame.Append(source, pressed).HasValue());
        CHECK(frame.Append(source, pressed).HasError());
        REQUIRE(frame.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
    }

    TEST_CASE("Input collection missing XR contribution preserves a held keyboard action", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto first = fixture.Frame(port, 1);
        REQUIRE(first.Append(source, pressed).HasValue());
        REQUIRE(first.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        fixture.collector.BeginFrame(2);
        fixture.collector.SetKey(Key::Space, true);
        auto raw = port.Begin(fixture.collector.Commit());
        REQUIRE(raw.HasValue());
        auto second = std::move(raw).Value();
        REQUIRE(second.Commit().HasValue());
        const auto keyboard = fixture.router.ReadAction(fixture.context, fixture.action.id);
        CHECK(keyboard.down);
        CHECK(keyboard.released);
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
        CHECK(keyboard.pressed);
    }

    TEST_CASE("Input collection replacement requires a real neutral commit and preserves release evidence", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto first = fixture.Frame(port, 1);
        REQUIRE(first.Append(source, pressed).HasValue());
        REQUIRE(first.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        REQUIRE(source.Reset().HasValue());
        CHECK(port.RegisterSource(std::span{&fixture.action, 1}).HasError());
        auto neutral = fixture.Frame(port, 2);
        REQUIRE(neutral.Commit().HasValue());
        auto invalid = fixture.action;
        invalid.context = InputContextId{"foreign"};
        CHECK(port.RegisterSource(std::span{&invalid, 1}).HasError());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).released);
        auto replacement = fixture.Source(port);
        auto next = fixture.Frame(port, 3);
        CHECK(next.Append(source, pressed).HasError());
        REQUIRE(next.Append(replacement, pressed).HasValue());
        REQUIRE(next.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
    }

    TEST_CASE("Input collection map replacement invalidates borrows and cannot publish an old action index", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto frame = fixture.Frame(port, 1);
        REQUIRE(frame.Append(source, pressed).HasValue());
        REQUIRE(fixture.router.SetActionMap({fixture.action}).HasValue());
        CHECK(frame.Commit().HasError());
        CHECK(port.Begin(fixture.collector.Commit()).HasError());
        auto current = fixture.Port();
        CHECK(current.RegisterSource(std::span{&fixture.action, 1}).HasError());
        auto neutral = fixture.Frame(current, 2);
        REQUIRE(neutral.Commit().HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        CHECK(current.RegisterSource(std::span{&fixture.action, 1}).HasValue());
    }

    TEST_CASE("Input collection weak borrows reject retired routers and foreign owner threads", "[unit][input][collection]") {
        std::optional<InputActionCollection> retained;
        InputActionSourceToken source;
        {
            Fixture fixture;
            retained = fixture.Port();
            source = fixture.Source(*retained);
            bool rejected = false;
            const auto &raw = fixture.collector.Commit();
            std::thread worker([&] {
                rejected = retained->Begin(raw).HasError();
            });
            worker.join();
            CHECK(rejected);
        }
        RawInputSnapshot raw;
        CHECK(retained->Begin(raw).HasError());
        CHECK(source.Reset().HasError());
    }

    TEST_CASE("Input collection focus and legacy empty-source commits neutralize without frame allocations", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        const auto before = Tests::AllocationProbe::Count();
        bool accepted = true;
        for (FrameNumber number = 1; number != 8; ++number) {
            fixture.collector.BeginFrame(number);
            auto candidate = port.Begin(fixture.collector.Commit());
            if (candidate.HasError()) {
                accepted = false;
                break;
            }
            auto frame = std::move(candidate).Value();
            accepted = frame.Append(source, pressed).HasValue() && frame.Commit().HasValue();
            static_cast<void>(fixture.router.ReadAction(fixture.context, fixture.action.id));
        }
        const auto after = Tests::AllocationProbe::Count();
        CHECK(accepted);
        CHECK(after == before);
        fixture.collector.BeginFrame(8);
        fixture.router.BeginFrame(fixture.collector.Commit());
        CHECK(fixture.router.ReadAction(fixture.context, fixture.action.id).released);
    }

    TEST_CASE("Input collection enforces aggregate source capacity and exact canonical contexts", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        std::array<InputActionSourceToken, MaximumCollectedInputSources> sources;
        for (auto &source : sources)
            source = fixture.Source(port);
        CHECK(port.RegisterSource(std::span{&fixture.action, 1}).HasError());
        auto forged = fixture.action;
        forged.context = InputContextId{"foreign"};
        CHECK(port.RegisterSource(std::span{&forged, 1}).HasError());
        REQUIRE(sources[0].Reset().HasValue());
        auto neutral = fixture.Frame(port, 1);
        REQUIRE(neutral.Commit().HasValue());
        CHECK(port.RegisterSource(std::span{&forged, 1}).HasError());
        forged = fixture.action;
        forged.valueType = ActionValueType::Axis1D;
        CHECK(port.RegisterSource(std::span{&forged, 1}).HasError());
        CHECK(port.RegisterSource(std::span{&fixture.action, 1}).HasValue());
    }

    TEST_CASE("Input collection focus and modal gates preserve canonical routing eligibility", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        fixture.collector.BeginFrame(1);
        fixture.collector.SetWindowState({.focused = false});
        auto opened = port.Begin(fixture.collector.Commit());
        REQUIRE(opened.HasValue());
        auto frame = std::move(opened).Value();
        REQUIRE(frame.Append(source, pressed).HasValue());
        REQUIRE(frame.Commit().HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        fixture.collector.BeginFrame(2);
        fixture.collector.SetWindowState({.focused = true});
        auto next = port.Begin(fixture.collector.Commit());
        REQUIRE(next.HasValue());
        auto focused = std::move(next).Value();
        REQUIRE(focused.Append(source, pressed).HasValue());
        REQUIRE(focused.Commit().HasValue());
        auto modal = fixture.router.PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
    }

    TEST_CASE("Input collection total action storage rejects overflow without consuming a source slot", "[unit][input][collection]") {
        Fixture fixture;
        std::vector<ActionDescriptor> actions;
        for (std::size_t index = 0; index <= MaximumCollectedInputActions; ++index)
            actions.push_back({ActionId{"action." + std::to_string(index)}, ActionValueType::Digital, fixture.action.context});
        REQUIRE(fixture.router.SetActionMap(actions).HasValue());
        auto port = fixture.Port();
        CHECK(port.RegisterSource(actions).HasError());
        auto registered = port.RegisterSource(std::span{actions}.first(MaximumCollectedInputActions));
        REQUIRE(registered.HasValue());
        auto source = std::move(registered).Value();
        CHECK(port.RegisterSource(std::span{actions}.last(1)).HasError());
        auto frame = fixture.Frame(port, 1);
        const std::array last{CollectedActionValue{255, 1, 0, true}};
        REQUIRE(frame.Append(source, last).HasValue());
        REQUIRE(frame.Commit().HasValue());
        CHECK(fixture.router.ReadAction(fixture.context, actions[255].id).pressed);
    }

    TEST_CASE("Input collection copies only an existing player assignment and rejects its replacement", "[unit][input][collection]") {
        Fixture fixture;
        fixture.collector.BeginFrame(1);
        const auto device = fixture.collector.ConnectGamepad("host-assigned");
        fixture.router.BeginFrame(fixture.collector.Commit());
        CHECK(fixture.router.ActionCollection(fixture.context, device).HasError());
        REQUIRE(fixture.router.AssignGamepad(2, device));
        auto borrowed = fixture.router.ActionCollection(fixture.context, device);
        REQUIRE(borrowed.HasValue());
        auto port = std::move(borrowed).Value();
        auto source = fixture.Source(port);
        auto frame = fixture.Frame(port, 2);
        REQUIRE(frame.Append(source, pressed).HasValue());
        REQUIRE(frame.Commit().HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id, 1).down);
        const auto unfiltered = fixture.router.ReadAction(fixture.context, fixture.action.id);
        CHECK(unfiltered.down);
        CHECK(unfiltered.pressed);
        const auto assigned = fixture.router.ReadAction(fixture.context, fixture.action.id, 2);
        CHECK(assigned.down);
        CHECK_FALSE(assigned.pressed);
        fixture.router.UnassignGamepad(device);
        CHECK(port.Begin(fixture.collector.Commit()).HasError());
        auto current = fixture.Port();
        auto neutral = fixture.Frame(current, 3);
        CHECK(neutral.Append(source, pressed).HasError());
        REQUIRE(neutral.Commit().HasValue());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id, 2).down);
    }

    TEST_CASE("Input collection destructor cleanup does not allocate under failure injection", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        std::optional<InputActionSourceToken> source{fixture.Source(port)};
        std::optional<InputCollectionFrame> frame{fixture.Frame(port, 1)};
        REQUIRE(frame->Append(*source, pressed).HasValue());
        {
            Tests::AllocationProbe::ScopedFailure failNextAllocation;
            frame.reset();
            source.reset();
        }
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        auto neutral = fixture.Frame(port, 2);
        REQUIRE(neutral.Commit().HasValue());
        CHECK(port.RegisterSource(std::span{&fixture.action, 1}).HasValue());
    }

    TEST_CASE("Input collection foreign-thread destruction cannot mutate or revive the owner cutoff", "[unit][input][collection]") {
        Fixture fixture;
        auto port = fixture.Port();
        auto source = fixture.Source(port);
        auto frame = fixture.Frame(port, 1);
        REQUIRE(frame.Append(source, pressed).HasValue());
        std::thread worker([movedSource = std::move(source), movedFrame = std::move(frame)]() mutable {
            // Both destructors must be non-allocating even when typed explicit calls would fail.
            Tests::AllocationProbe::ScopedFailure failNextAllocation;
            movedSource = InputActionSourceToken{};
            movedFrame = InputCollectionFrame{};
        });
        worker.join();
        CHECK(port.Begin(fixture.collector.Commit()).HasError());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
        fixture.collector.BeginFrame(2);
        fixture.router.BeginFrame(fixture.collector.Commit());
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).down);
    }

    TEST_CASE("Input collection leaves physical-only independent action edges unchanged", "[unit][input][collection]") {
        Fixture fixture;
        fixture.action.defaultBindings.push_back({.kind = BindingControlKind::Key, .key = Key::Enter});
        REQUIRE(fixture.router.SetActionMap({fixture.action}).HasValue());
        fixture.collector.BeginFrame(1);
        fixture.collector.SetKey(Key::Space, true);
        fixture.collector.SetKey(Key::Enter, true);
        fixture.router.BeginFrame(fixture.collector.Commit());
        const auto pressedBoth = fixture.router.ReadAction(fixture.context, fixture.action.id);
        CHECK(pressedBoth.down);
        CHECK(pressedBoth.pressed);
        CHECK_FALSE(pressedBoth.released);
        CHECK_FALSE(fixture.router.ReadAction(fixture.context, fixture.action.id).pressed);
        fixture.collector.BeginFrame(2);
        fixture.collector.SetKey(Key::Space, false);
        fixture.router.BeginFrame(fixture.collector.Commit());
        const auto oneReleased = fixture.router.ReadAction(fixture.context, fixture.action.id);
        CHECK(oneReleased.down);
        CHECK_FALSE(oneReleased.pressed);
        CHECK(oneReleased.released);
    }

}  // namespace Horo::Input
