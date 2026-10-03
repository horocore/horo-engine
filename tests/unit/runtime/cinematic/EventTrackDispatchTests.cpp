#include "EventTrackTestSupport.h"

namespace Horo::Cinematic {
    using namespace EventTestSupport;

    TEST_CASE("Event staging rejects null and mismatched borrowed contexts", "[unit][cinematic][event][context]") {
        HandlerProbe probe;
        const BorrowedCallbackContext context{&probe};
        CHECK(context.Get<HandlerProbe>() == &probe);
        CHECK(context.Get<CinematicEventDispatcher>() == nullptr);
        RequireCode(CinematicEventDispatcher::StageHook(context, {}), EventTrackErrors::DispatchStateInvalid);
        RequireCode(CinematicEventDispatcher::StageHook({}, {}), EventTrackErrors::DispatchStateInvalid);
    }

    TEST_CASE("Script event cook resolves exact export parameter types and qualified names", "[unit][cinematic][event][cook]") {
        using enum EventRuntimeContext;
        const auto exports = ExportSnapshot();
        const auto descriptor = Descriptor();
        auto source = Authored();
        auto cooked = CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless);
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value()->Find(Track, Key)->binding == Binding);
        CHECK_FALSE(cooked.Value()->Find(Track, {12, 1}));

        source.qualifiedName = "gameplay.quest.missing";
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless), EventTrackErrors::UnknownName);
        source = Authored();
        source.arguments = {Extensions::ScriptValue::String("wrong type")};
        const auto mismatch = CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless);
        REQUIRE(mismatch.HasError());
        CHECK(mismatch.ErrorValue().message.find("quest") != std::string::npos);
        CHECK(mismatch.ErrorValue().message.find("index 0") != std::string::npos);
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless),
                    EventTrackErrors::SchemaMismatch);
        source = Authored();
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Preview), EventTrackErrors::SchemaMismatch);
        const std::array duplicate{descriptor, descriptor};
        RequireCode(CookScriptEvents(std::span{&source, 1}, duplicate, exports, Headless), EventTrackErrors::CookInvalid);
    }

    TEST_CASE("Committed event occurrences invoke a leased gameplay callback once at the owner safe point",
              "[unit][cinematic][event][dispatch]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        const auto plan = Cooked();
        const auto evaluation = Evaluation();
        const SequencePlayerHandle player{Session, {1, 1}};
        REQUIRE(dispatcher.Activate(player, plan, evaluation, 3).HasValue());
        const auto occurrence = Occurrence();

        REQUIRE(dispatcher.BeginTick(10).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        CHECK(dispatcher.Drain(1).HasError());
        REQUIRE(dispatcher.AbortTick().HasValue());
        CHECK(dispatcher.Drain(1).Value() == 0);
        CHECK(probe->calls == 0);

        REQUIRE(dispatcher.BeginTick(11).HasValue());
        const std::size_t before = Horo::Tests::AllocationProbe::Count();
        const auto staged = dispatcher.Stage(std::span{&occurrence, 1});
        const std::size_t after = Horo::Tests::AllocationProbe::Count();
        REQUIRE(staged.HasValue());
        CHECK(after == before);
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(probe->calls == 0);
        REQUIRE(dispatcher.Drain(1).Value() == 1);
        CHECK(probe->calls == 1);
        CHECK(probe->lastQuest == 42);
        CHECK(probe->lastTick == 11);
        REQUIRE(dispatcher.Results().size() == 1);
        CHECK(dispatcher.Results()[0].outcome == EventDispatchOutcome::Accepted);

        REQUIRE(dispatcher.BeginTick(12).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(dispatcher.Drain(1).Value() == 0);
        CHECK(probe->calls == 1);

        dispatcher.ClearResults();
        REQUIRE(dispatcher.BeginTick(13).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(dispatcher.Drain(1).Value() == 0);
        CHECK(probe->calls == 1);
    }

    TEST_CASE("Cooked event lookup does not construct payload storage", "[unit][cinematic][event][allocation]") {
        const auto plan = Cooked();
        const auto *expected = plan->Find(Track, Key);
        REQUIRE(expected != nullptr);
        std::size_t matches{};
        const std::size_t before = Horo::Tests::AllocationProbe::Count();
        for (std::size_t query = 0; query < 1'024; ++query) {
            if (plan->Find(Track, Key) == expected) {
                ++matches;
            }
            if (plan->Find(Track, {12, 1}) == nullptr) {
                ++matches;
            }
            if (plan->Find({9, 1}, Key) == nullptr) {
                ++matches;
            }
            if (plan->Find({11, 1}, Key) == nullptr) {
                ++matches;
            }
        }
        const std::size_t after = Horo::Tests::AllocationProbe::Count();
        CHECK(matches == 4'096);
        CHECK(after == before);
    }

    TEST_CASE("Revocation and cancellation fence queued event callbacks", "[unit][cinematic][event][lifecycle]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        const auto plan = Cooked();
        const auto evaluation = Evaluation();
        const SequencePlayerHandle player{Session, {1, 1}};
        REQUIRE(dispatcher.Activate(player, plan, evaluation, 0).HasValue());
        const auto occurrence = Occurrence();
        REQUIRE(dispatcher.BeginTick(1).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        REQUIRE(dispatcher.Revoke(Binding, 1).HasValue());
        CHECK(dispatcher.Drain(1).Value() == 1);
        CHECK(probe->calls == 0);
        CHECK(dispatcher.Results()[0].outcome == EventDispatchOutcome::StaleBinding);
        REQUIRE(dispatcher.BeginTick(2).HasValue());
        RequireCode(dispatcher.Stage(std::span{&occurrence, 1}), EventTrackErrors::StaleBinding);
        REQUIRE(dispatcher.AbortTick().HasValue());
        REQUIRE(dispatcher.CancelPlayer(player).HasValue());
        REQUIRE(dispatcher.Close().HasValue());
    }

    TEST_CASE("Evaluation stages a complete batch without invoking gameplay during sampling", "[unit][cinematic][event][evaluation]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        const auto cooked = Cooked();
        const auto evaluation = Evaluation();
        const SequencePlayerHandle player{Session, {1, 1}};
        REQUIRE(dispatcher.Activate(player, cooked, evaluation, 0).HasValue());
        const SequencePlayerSnapshot snapshot{player, SequencePlaybackState::Playing, 0, 10, {1, 1}, 2};
        auto cursor = MakeSequenceFrameCursor(snapshot, SequenceCursorResetPolicy::EmitCurrentBoundary).Value();
        std::array<SequenceFrameEventOccurrence, 4> events{};
        const SequenceFrameScratch scratch{{}, events, {}};
        const SequenceFrameHooks hooks{BorrowedCallbackContext{&dispatcher}, CinematicEventDispatcher::StageHook, nullptr, nullptr};

        REQUIRE(dispatcher.BeginTick(3).HasValue());
        auto evaluated = evaluation.Evaluate(snapshot, 3, cursor, scratch, hooks);
        REQUIRE(evaluated.HasValue());
        CHECK(evaluated.Value().firedEvents == 1);
        CHECK(probe->calls == 0);
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(probe->calls == 0);
        REQUIRE(dispatcher.Drain(1).Value() == 1);
        CHECK(probe->calls == 1);
    }

    TEST_CASE("Event queue admission is atomic and committed delivery order ignores stage arrival order",
              "[unit][cinematic][event][order][capacity]") {
        auto created = CinematicEventDispatcher::Create(Session, EventRuntimeContext::Headless, 2, 2);
        REQUIRE(created.HasValue());
        auto dispatcher = std::move(created).Value();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        const auto cooked = Cooked();
        const auto evaluation = Evaluation();
        REQUIRE(dispatcher.Activate({Session, {8, 1}}, cooked, evaluation, 0).HasValue());
        REQUIRE(dispatcher.Activate({Session, {3, 1}}, cooked, evaluation, 5).HasValue());
        const std::array reversed{Occurrence(8), Occurrence(3)};
        REQUIRE(dispatcher.BeginTick(7).HasValue());
        REQUIRE(dispatcher.Stage(reversed).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        REQUIRE(dispatcher.Drain(2).Value() == 2);
        REQUIRE(probe->calls == 2);
        CHECK(probe->deliveredPlayers[0] == 3);
        CHECK(probe->deliveredPlayers[1] == 8);

        auto limitedResult = CinematicEventDispatcher::Create(Session, EventRuntimeContext::Headless, 1, 2);
        REQUIRE(limitedResult.HasValue());
        auto limited = std::move(limitedResult).Value();
        Register(limited, probe);
        REQUIRE(limited.Activate({Session, {8, 1}}, cooked, evaluation, 0).HasValue());
        REQUIRE(limited.Activate({Session, {3, 1}}, cooked, evaluation, 5).HasValue());
        REQUIRE(limited.BeginTick(8).HasValue());
        RequireCode(limited.Stage(reversed), EventTrackErrors::DispatchCapacityExceeded);
        RequireCode(limited.CommitTick(), EventTrackErrors::DispatchStateInvalid);
        REQUIRE(limited.AbortTick().HasValue());
        CHECK(limited.Drain(2).Value() == 0);
    }

    TEST_CASE("Optional missing handler produces a typed result without invoking gameplay", "[unit][cinematic][event][optional]") {
        auto dispatcher = Dispatcher();
        auto descriptor = Descriptor();
        descriptor.required = false;
        const auto source = Authored();
        auto cooked = CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, ExportSnapshot(), EventRuntimeContext::Headless);
        REQUIRE(cooked.HasValue());
        const auto evaluation = Evaluation();
        const SequencePlayerHandle player{Session, {1, 1}};
        REQUIRE(dispatcher.Activate(player, cooked.Value(), evaluation, 0).HasValue());
        const auto occurrence = Occurrence();
        REQUIRE(dispatcher.BeginTick(1).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(dispatcher.Drain(1).Value() == 1);
        REQUIRE(dispatcher.Results().size() == 1);
        CHECK(dispatcher.Results()[0].outcome == EventDispatchOutcome::BindingUnavailable);
    }

    TEST_CASE("Unknown runtime key identity is a typed failure and cannot advance a transaction", "[unit][cinematic][event][diagnostic]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        REQUIRE(dispatcher.Activate({Session, {1, 1}}, Cooked(), Evaluation(), 0).HasValue());
        auto unknown = Occurrence();
        unknown.key = {999, 1};
        REQUIRE(dispatcher.BeginTick(1).HasValue());
        RequireCode(dispatcher.Stage(std::span{&unknown, 1}), EventTrackErrors::UnknownName);
        CHECK(dispatcher.CommitTick().HasError());
        REQUIRE(dispatcher.AbortTick().HasValue());
        CHECK(dispatcher.Drain(4).Value() == 0);
        CHECK(probe->calls == 0);
    }

    TEST_CASE("Cook rejects duplicate keys, skewed versions, missing exports and payload bounds",
              "[unit][cinematic][event][cook][validation]") {
        using enum EventRuntimeContext;
        auto descriptor = Descriptor();
        const auto source = Authored();
        const auto exports = ExportSnapshot();
        const std::array duplicate{source, source};
        RequireCode(CookScriptEvents(duplicate, std::span{&descriptor, 1}, exports, Headless), EventTrackErrors::CookInvalid);
        descriptor.exportVersion = {2, 0, 0};
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless),
                    EventTrackErrors::SchemaMismatch);
        descriptor = Descriptor();
        descriptor.exportFunctionId = "absent";
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless), EventTrackErrors::UnknownName);
        descriptor = Descriptor();
        descriptor.maximumPayloadBytes = 1;
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, Headless),
                    EventTrackErrors::CookCapacityExceeded);
    }

    TEST_CASE("Acknowledged result storage supports long-lived players without forgetting old occurrence identity",
              "[unit][cinematic][event][history]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        REQUIRE(dispatcher.Activate({Session, {1, 1}}, Cooked(), Evaluation(), 0).HasValue());
        for (std::uint64_t traversal = 1; traversal <= 12; ++traversal) {
            const auto occurrence = Occurrence(1, traversal);
            REQUIRE(dispatcher.BeginTick(traversal).HasValue());
            REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
            REQUIRE(dispatcher.CommitTick().HasValue());
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            dispatcher.ClearResults();
        }
        CHECK(probe->calls == 12);
        const auto old = Occurrence(1, 1);
        REQUIRE(dispatcher.BeginTick(13).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&old, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(dispatcher.Drain(4).Value() == 0);
        CHECK(probe->calls == 12);
    }

    TEST_CASE("Acknowledged results recycle across more distinct keys than result capacity", "[unit][cinematic][event][history]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        std::array<AuthoredScriptEventKey, 12> sources;
        std::array<SequenceFrameEventKey, 12> events;
        for (std::size_t index = 0; index < sources.size(); ++index) {
            sources[index] = Authored({index + 11, 1});
            sources[index].time = static_cast<SequenceTime>(index + 1);
            events[index] = {Track, sources[index].key, sources[index].time, true};
        }
        const auto descriptor = Descriptor();
        auto cooked = CookScriptEvents(sources, std::span{&descriptor, 1}, ExportSnapshot(), EventRuntimeContext::Headless);
        REQUIRE(cooked.HasValue());
        auto evaluation = SequenceFrameEvaluationPlan::Create(20, SequenceLoopMode::Once, 4, {}, events, {});
        REQUIRE(evaluation.HasValue());
        REQUIRE(dispatcher.Activate({Session, {1, 1}}, cooked.Value(), evaluation.Value(), 0).HasValue());
        for (std::size_t index = 0; index < sources.size(); ++index) {
            auto occurrence = Occurrence();
            occurrence.key = sources[index].key;
            occurrence.time = sources[index].time;
            REQUIRE(dispatcher.BeginTick(index + 1).HasValue());
            REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
            REQUIRE(dispatcher.CommitTick().HasValue());
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            dispatcher.ClearResults();
        }
        CHECK(probe->calls == sources.size());
        auto old = Occurrence();
        old.time = sources.front().time;
        REQUIRE(dispatcher.BeginTick(13).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&old, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        CHECK(dispatcher.Drain(4).Value() == 0);
        CHECK(probe->calls == sources.size());
    }

    TEST_CASE("Activation compares exact cooked time and reverse semantics rather than just the key set",
              "[unit][cinematic][event][activation]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        const auto cooked = Cooked();
        for (const auto &event : std::array{SequenceFrameEventKey{Track, Key, 8, true}, SequenceFrameEventKey{Track, Key, 2, false}}) {
            auto wrong = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, std::span{&event, 1}, {});
            REQUIRE(wrong.HasValue());
            RequireCode(dispatcher.Activate({Session, {1, 1}}, cooked, wrong.Value(), 0), EventTrackErrors::CookInvalid);
        }
        REQUIRE(dispatcher.Activate({Session, {1, 1}}, cooked, Evaluation(), 0).HasValue());
        auto wrongTime = Occurrence();
        wrongTime.time = 8;
        REQUIRE(dispatcher.BeginTick(1).HasValue());
        RequireCode(dispatcher.Stage(std::span{&wrongTime, 1}), EventTrackErrors::CookInvalid);
        REQUIRE(dispatcher.AbortTick().HasValue());
        CHECK(probe->calls == 0);
    }

    TEST_CASE("Dispatcher preserves reverse and loop crossing chronology", "[unit][cinematic][event][order]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        auto first = Authored(Key);
        auto second = Authored({12, 1});
        SequenceLoopMode loop = SequenceLoopMode::Once;
        SequencePlayerSnapshot snapshot{{Session, {1, 1}}, SequencePlaybackState::Playing, 10, 10, {-1, 1}, 2};
        SequenceTime delta = 10;
        SECTION("reverse") {
            second.time = 8;
        }
        SECTION("loop") {
            first.time = 0;
            second.time = 10;
            snapshot.position = 0;
            snapshot.rate = {1, 1};
            loop = SequenceLoopMode::Loop;
            delta = 12;
        }
        const std::array sources{first, second};
        const auto descriptor = Descriptor();
        auto cooked = CookScriptEvents(sources, std::span{&descriptor, 1}, ExportSnapshot(), EventRuntimeContext::Headless);
        REQUIRE(cooked.HasValue());
        const std::array events{SequenceFrameEventKey{Track, Key, first.time, true},
                                SequenceFrameEventKey{Track, {12, 1}, second.time, true}};
        auto evaluated = SequenceFrameEvaluationPlan::Create(10, loop, 4, {}, events, {});
        REQUIRE(evaluated.HasValue());
        REQUIRE(dispatcher.Activate(snapshot.handle, cooked.Value(), evaluated.Value(), 0).HasValue());
        auto cursor = MakeSequenceFrameCursor(snapshot, SequenceCursorResetPolicy::EmitCurrentBoundary).Value();
        std::array<SequenceFrameEventOccurrence, 4> occurrences;
        REQUIRE(dispatcher.BeginTick(1).HasValue());
        REQUIRE(evaluated.Value()
                    .Evaluate(snapshot, delta, cursor, {{}, occurrences, {}},
                              {BorrowedCallbackContext{&dispatcher}, CinematicEventDispatcher::StageHook})
                    .HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        REQUIRE(dispatcher.Drain(4).HasValue());
        if (loop == SequenceLoopMode::Loop) {
            REQUIRE(probe->calls == 3);
            CHECK(probe->deliveredKeys[0] == Key.stableValue);
            CHECK(probe->deliveredKeys[1] == 12);
            CHECK(probe->deliveredKeys[2] == Key.stableValue);
        } else {
            REQUIRE(probe->calls == 2);
            CHECK(probe->deliveredKeys[0] == 12);
            CHECK(probe->deliveredKeys[1] == Key.stableValue);
        }
    }

    TEST_CASE("Stop drains admitted events and cancellation retires pending payloads", "[unit][cinematic][event][lifecycle]") {
        auto dispatcher = Dispatcher();
        auto probe = std::make_shared<HandlerProbe>();
        Register(dispatcher, probe);
        const SequencePlayerHandle player{Session, {1, 1}};
        REQUIRE(dispatcher.Activate(player, Cooked(), Evaluation(), 0).HasValue());
        const auto occurrence = Occurrence();
        REQUIRE(dispatcher.BeginTick(1).HasValue());
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        REQUIRE(dispatcher.CommitTick().HasValue());
        SECTION("orderly stop") {
            REQUIRE(dispatcher.StopPlayer(player).HasValue());
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            CHECK(probe->calls == 1);
        }
        SECTION("cancellation") {
            REQUIRE(dispatcher.CancelPlayer(player).HasValue());
            CHECK(dispatcher.Drain(4).Value() == 0);
            CHECK(probe->calls == 0);
        }
        REQUIRE(dispatcher.Close().HasValue());
        CHECK(dispatcher.BeginTick(2).HasError());
    }

    TEST_CASE("Typed handler failure stops future track effects and contains exceptions", "[unit][cinematic][event][failure]") {
        using enum EventDispatchOutcome;

        struct FailureProbe {
            EventDispatchOutcome outcome;
            std::size_t calls{};
            bool throws{};
        };

        for (const auto outcome :
             std::array{SuppressedByAuthority, CapabilityUnavailable, InvalidTarget, Backpressured, HandlerFailed, Count}) {
            auto dispatcher = Dispatcher();
            auto probe = std::make_shared<FailureProbe>(outcome);
            probe->throws = outcome == HandlerFailed;
            auto callback = +[](const BorrowedCallbackContext &context, const EventDispatchRequest &) {
                auto &state = *context.Get<FailureProbe>();
                ++state.calls;
                if (state.throws)
                    throw 1;
                return state.outcome;
            };
            REQUIRE(
                dispatcher
                    .Register({Binding, Schema, EventRuntimeContext::Headless, 1, BorrowedCallbackContext{probe.get()}, callback, probe})
                    .HasValue());
            const SequencePlayerHandle player{Session, {1, 1}};
            REQUIRE(dispatcher.Activate(player, Cooked(), Evaluation(), 0).HasValue());
            const auto occurrence = Occurrence();
            REQUIRE(dispatcher.BeginTick(1).HasValue());
            REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
            REQUIRE(dispatcher.CommitTick().HasValue());
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            CHECK(dispatcher.Results().front().outcome == (outcome == Count ? HandlerFailed : outcome));
            dispatcher.ClearResults();
            const auto next = Occurrence(1, 2);
            REQUIRE(dispatcher.BeginTick(2).HasValue());
            REQUIRE(dispatcher.Stage(std::span{&next, 1}).HasValue());
            REQUIRE(dispatcher.CommitTick().HasValue());
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            CHECK(probe->calls == 1);
        }
    }
}  // namespace Horo::Cinematic
