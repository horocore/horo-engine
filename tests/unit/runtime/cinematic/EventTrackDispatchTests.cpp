#include "GameplayModuleTestSupport.h"
#include "Horo/Cinematic/EventDispatcher.h"
#include "Horo/Cinematic/EventSession.h"
#include "Horo/Cinematic/EventTrackErrors.h"
#include "Horo/Cinematic/GameplayEventAdapter.h"
#include "Horo/Cinematic/ScriptEventCook.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        template <class T>
        concept UncheckedCook = requires(std::vector<CookedEventKey> keys) { T::Create(std::move(keys)); };
        static_assert(!UncheckedCook<CookedEventPlan>);
        template <class T>
        concept UncheckedConstruction = requires(std::vector<CookedEventKey> keys) { T({}, std::move(keys)); };
        static_assert(!UncheckedConstruction<CookedEventPlan>);
        constexpr CinematicRuntimeSessionId Session{70, 1};
        constexpr EventBindingId Binding{90, 1};
        constexpr EventPayloadSchemaId Schema{91, 1};
        constexpr TrackId Track{10, 1};
        constexpr KeyframeId Key{11, 1};

        [[nodiscard]] Extensions::ScriptExportDescriptorSnapshotPtr ExportSnapshot() {
            using namespace Extensions;
            ScriptExportDescriptor api{
                .moduleId = "gameplay.quest",
                .id = "gameplay.quest.events",
                .nameSpace = "gameplay.quest",
                .version = {1, 0, 0},
                .compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 0, 0}},
                .service = {.capability = {"gameplay.quest"},
                            .serviceId = "gameplay.quest.service",
                            .contractId = "gameplay.quest.contract",
                            .minimumVersion = {1, 0, 0}},
            };
            api.functions.push_back(
                {.id = "complete",
                 .introducedVersion = {1, 0, 0},
                 .invocation = ScriptExportInvocationMode::Synchronous,
                 .parameters = {
                     {.id = "quest", .type = {.primitive = ScriptExportPrimitiveKind::SignedInteger}, .introducedVersion = {1, 0, 0}}}});
            auto snapshot = BuildScriptExportDescriptorSnapshot(std::array{api});
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        }

        [[nodiscard]] ScriptEventDescriptor Descriptor() {
            return {.qualifiedName = "gameplay.quest.complete",
                    .binding = Binding,
                    .schema = Schema,
                    .exportApiId = "gameplay.quest.events",
                    .exportFunctionId = "complete",
                    .exportVersion = {1, 0, 0},
                    .allowedContexts =
                        static_cast<std::byte>(EventRuntimeContext::Runtime) | static_cast<std::byte>(EventRuntimeContext::Headless)};
        }

        [[nodiscard]] AuthoredScriptEventKey Authored(const KeyframeId key = Key, const std::int64_t quest = 42) {
            return {Track, key, "gameplay.quest.complete", {Extensions::ScriptValue(quest)}, 2, true};
        }

        [[nodiscard]] std::shared_ptr<const CookedEventPlan> Cooked() {
            const auto source = Authored();
            const auto descriptor = Descriptor();
            auto cooked =
                CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, ExportSnapshot(), EventRuntimeContext::Headless);
            REQUIRE(cooked.HasValue());
            return cooked.Value();
        }

        [[nodiscard]] SequenceFrameEvaluationPlan Evaluation() {
            const std::array events{SequenceFrameEventKey{Track, Key, 2, true}};
            auto evaluation = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, events, {});
            REQUIRE(evaluation.HasValue());
            return std::move(evaluation).Value();
        }

        struct HandlerProbe final {
            std::size_t calls{};
            std::int64_t lastQuest{};
            std::uint64_t lastTick{};
            std::array<std::uint64_t, 64> deliveredPlayers{};
            std::array<std::uint64_t, 64> deliveredKeys{};
        };

        struct PublicationProbe {
            float value{};
            std::size_t writes{};
            std::size_t finished{};
        };

        Result<float> SamplePublication(const void *, const SequenceTime time) {
            return Result<float>::Success(static_cast<float>(time));
        }

        void ApplyPublication(void *context, const float value) noexcept {
            auto &probe = *static_cast<PublicationProbe *>(context);
            probe.value = value;
            ++probe.writes;
        }

        void FinishPublication(void *context, const SequencePlayerHandle &) noexcept {
            ++static_cast<PublicationProbe *>(context)->finished;
        }

        EventDispatchOutcome GameplayCallback(const BorrowedCallbackContext &context, const EventDispatchRequest &request) {
            auto *handler = context.Get<HandlerProbe>();
            if (handler == nullptr)
                return EventDispatchOutcome::HandlerFailed;
            auto &probe = *handler;
            const auto decoded = Extensions::DecodeScriptValue(request.payload);
            if (decoded.HasError() || decoded.Value().AsArray().size() != 1)
                return EventDispatchOutcome::HandlerFailed;
            const std::int64_t *quest = decoded.Value().AsArray().front().AsSignedInteger();
            if (quest == nullptr)
                return EventDispatchOutcome::HandlerFailed;
            probe.lastQuest = *quest;
            probe.lastTick = request.committedTick;
            probe.deliveredPlayers[probe.calls] = request.occurrence.player.player.stableValue;
            probe.deliveredKeys[probe.calls] = request.occurrence.key.stableValue;
            ++probe.calls;
            return EventDispatchOutcome::Accepted;
        }

        [[nodiscard]] CinematicEventDispatcher Dispatcher() {
            auto dispatcher = CinematicEventDispatcher::Create(Session, EventRuntimeContext::Headless, 4, 4);
            REQUIRE(dispatcher.HasValue());
            return std::move(dispatcher).Value();
        }

        void Register(CinematicEventDispatcher &dispatcher, const std::shared_ptr<HandlerProbe> &probe,
                      const std::uint64_t generation = 1) {
            REQUIRE(dispatcher.Register({Binding, Schema, EventRuntimeContext::Headless, generation, probe.get(), GameplayCallback, probe})
                        .HasValue());
        }

        [[nodiscard]] SequenceFrameEventOccurrence Occurrence(const std::uint64_t player = 1, const std::uint64_t traversal = 1) {
            return {{Session, {player, 1}}, Track, Key, 2, traversal, SequenceTraversalDirection::Forward};
        }

        template <typename T> void RequireCode(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code.code.Value());
        }
    }  // namespace

    TEST_CASE("Event staging rejects null and mismatched borrowed contexts", "[unit][cinematic][event][context]") {
        HandlerProbe probe;
        const BorrowedCallbackContext context{&probe};
        CHECK(context.Get<HandlerProbe>() == &probe);
        CHECK(context.Get<CinematicEventDispatcher>() == nullptr);
        RequireCode(CinematicEventDispatcher::StageHook(context, {}), EventTrackErrors::DispatchStateInvalid);
        RequireCode(CinematicEventDispatcher::StageHook({}, {}), EventTrackErrors::DispatchStateInvalid);
    }

    TEST_CASE("Script event cook resolves exact export parameter types and qualified names", "[unit][cinematic][event][cook]") {
        const auto exports = ExportSnapshot();
        const auto descriptor = Descriptor();
        auto source = Authored();
        auto cooked = CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless);
        REQUIRE(cooked.HasValue());
        CHECK(cooked.Value()->Find(Track, Key)->binding == Binding);
        CHECK_FALSE(cooked.Value()->Find(Track, {12, 1}));

        source.qualifiedName = "gameplay.quest.missing";
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless),
                    EventTrackErrors::UnknownName);
        source = Authored();
        source.arguments = {Extensions::ScriptValue::String("wrong type")};
        const auto mismatch = CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless);
        REQUIRE(mismatch.HasError());
        CHECK(mismatch.ErrorValue().message.find("quest") != std::string::npos);
        CHECK(mismatch.ErrorValue().message.find("index 0") != std::string::npos);
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless),
                    EventTrackErrors::SchemaMismatch);
        source = Authored();
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Preview),
                    EventTrackErrors::SchemaMismatch);
        const std::array duplicate{descriptor, descriptor};
        RequireCode(CookScriptEvents(std::span{&source, 1}, duplicate, exports, EventRuntimeContext::Headless),
                    EventTrackErrors::CookInvalid);
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
        REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
        CHECK(Horo::Tests::AllocationProbe::Count() == before);
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
        const SequenceFrameHooks hooks{&dispatcher, CinematicEventDispatcher::StageHook, nullptr, nullptr};

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

    TEST_CASE("Native gameplay modules receive cooked events only after successful aggregate tick publication",
              "[unit][cinematic][event][native][rollback]") {
        Gameplay::GameModuleHost host;
        auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                             Horo::Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)});
        REQUIRE(loaded.HasValue());
        auto gameModule = std::move(loaded).Value();
        auto binding = BindGameplayEvent(*gameModule, Binding, Schema, EventRuntimeContext::Headless, 1);
        REQUIRE(binding.HasValue());
        CHECK(BindGameplayEvent(*gameModule, Binding, Schema, EventRuntimeContext::Preview, 1).HasError());
        CHECK(BindGameplayEvent(*gameModule, {999, 1}, Schema, EventRuntimeContext::Headless, 1).HasError());
        auto dispatcher = Dispatcher();
        REQUIRE(dispatcher.Register(std::move(binding).Value()).HasValue());
        auto created = CinematicRuntimeService::Create({Session, SequenceCookTier::Standard});
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        const SequencePlayerHandle player{Session, {1, 1}};
        const auto cooked = Cooked();
        CinematicEventSession session(runtime, dispatcher);
        PublicationProbe published;
        const std::array tracks{
            SequenceFrameTrackDescriptor{{13, 1}, SequenceApplyStage::Property, &published, SamplePublication, ApplyPublication}};
        const std::array keys{SequenceFrameEventKey{Track, Key, 2, true}};
        auto plan = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, tracks, keys, {});
        REQUIRE(plan.HasValue());
        REQUIRE(session.Activate({{player, 10}, std::move(plan).Value()}, cooked, 0).HasValue());
        REQUIRE(runtime.Play(player).HasValue());
        std::array<SequenceFrameEventOccurrence, 4> events;
        std::array<SequenceSampledValue, 1> values;
        const SequenceFrameScratch scratch{values, events, {}};
        const SequenceFrameHooks hooks{.finishedContext = &published, .finishedHook = FinishPublication};
        const auto before = runtime.Snapshot(player).Value();
        REQUIRE(session.BeginTick(1).HasValue());
        REQUIRE(session.Evaluate(player, 10, scratch, hooks).HasValue());
        CHECK(runtime.Snapshot(player).Value() == before);
        CHECK(published.writes == 0);
        CHECK(published.finished == 0);
        CHECK(dispatcher.Results().empty());
        REQUIRE(session.AbortTick().HasValue());
        CHECK(runtime.Snapshot(player).Value() == before);
        CHECK(dispatcher.Drain(4).Value() == 0);
        REQUIRE(session.BeginTick(1).HasValue());
        REQUIRE(session.Evaluate(player, 10, scratch, hooks).HasValue());
        REQUIRE(session.CommitTick().HasValue());
        CHECK(published.writes == 1);
        CHECK(published.value == 10.0F);
        CHECK(published.finished == 1);
        CHECK(runtime.Snapshot(player).Value().state == SequencePlaybackState::Stopped);
        CHECK(dispatcher.Results().empty());
        REQUIRE(dispatcher.Drain(4).Value() == 1);
        CHECK(dispatcher.Results().front().outcome == EventDispatchOutcome::Accepted);

        const SequencePlayerHandle nextPlayer{Session, {2, 1}};
        REQUIRE(session.Activate({{nextPlayer, 10}, Evaluation()}, cooked, 0).HasValue());
        REQUIRE(runtime.Play(nextPlayer).HasValue());
        REQUIRE(session.BeginTick(2).HasValue());
        REQUIRE(session.Evaluate(nextPlayer, 3, scratch).HasValue());
        REQUIRE(session.CommitTick().HasValue());

        // Retirement closes admission while the callback lease still pins the library.
        CHECK(gameModule->PrepareReload().HasError());
        CHECK(BindGameplayEvent(*gameModule, Binding, Schema, EventRuntimeContext::Headless, 2).HasError());
        gameModule.reset();
        REQUIRE(dispatcher.Drain(4).Value() == 1);
        CHECK(dispatcher.Results().back().outcome == EventDispatchOutcome::CapabilityUnavailable);
        REQUIRE(dispatcher.Revoke(Binding, 1).HasValue());
        REQUIRE(dispatcher.CancelPlayer(player).HasValue());
        REQUIRE(session.Close().HasValue());
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
        auto descriptor = Descriptor();
        const auto source = Authored();
        const auto exports = ExportSnapshot();
        const std::array duplicate{source, source};
        RequireCode(CookScriptEvents(duplicate, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless),
                    EventTrackErrors::CookInvalid);
        descriptor.exportVersion = {2, 0, 0};
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless),
                    EventTrackErrors::SchemaMismatch);
        descriptor = Descriptor();
        descriptor.exportFunctionId = "absent";
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless),
                    EventTrackErrors::UnknownName);
        descriptor = Descriptor();
        descriptor.maximumPayloadBytes = 1;
        RequireCode(CookScriptEvents(std::span{&source, 1}, std::span{&descriptor, 1}, exports, EventRuntimeContext::Headless),
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
                    .Evaluate(snapshot, delta, cursor, {{}, occurrences, {}}, {&dispatcher, CinematicEventDispatcher::StageHook})
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
        struct FailureProbe {
            EventDispatchOutcome outcome;
            std::size_t calls{};
            bool throws{};
        };

        for (const auto outcome : std::array{EventDispatchOutcome::SuppressedByAuthority, EventDispatchOutcome::CapabilityUnavailable,
                                             EventDispatchOutcome::InvalidTarget, EventDispatchOutcome::Backpressured,
                                             EventDispatchOutcome::HandlerFailed, EventDispatchOutcome::Count}) {
            auto dispatcher = Dispatcher();
            auto probe = std::make_shared<FailureProbe>(outcome);
            probe->throws = outcome == EventDispatchOutcome::HandlerFailed;
            auto callback = +[](const BorrowedCallbackContext &context, const EventDispatchRequest &) {
                auto &state = *context.Get<FailureProbe>();
                ++state.calls;
                if (state.throws)
                    throw 1;
                return state.outcome;
            };
            REQUIRE(dispatcher.Register({Binding, Schema, EventRuntimeContext::Headless, 1, probe.get(), callback, probe}).HasValue());
            const SequencePlayerHandle player{Session, {1, 1}};
            REQUIRE(dispatcher.Activate(player, Cooked(), Evaluation(), 0).HasValue());
            const auto occurrence = Occurrence();
            REQUIRE(dispatcher.BeginTick(1).HasValue());
            REQUIRE(dispatcher.Stage(std::span{&occurrence, 1}).HasValue());
            REQUIRE(dispatcher.CommitTick().HasValue());
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            CHECK(dispatcher.Results().front().outcome ==
                  (outcome == EventDispatchOutcome::Count ? EventDispatchOutcome::HandlerFailed : outcome));
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
