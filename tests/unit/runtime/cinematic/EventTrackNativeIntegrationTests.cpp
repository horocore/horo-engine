#include "EventTrackTestSupport.h"

namespace Horo::Cinematic {
    using namespace EventTestSupport;

    namespace {
        [[nodiscard]] std::unique_ptr<Gameplay::LoadedGameModule> LoadCallback(const Gameplay::GameModuleHost &host,
                                                                               CinematicEventDispatcher &dispatcher) {
            auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", Gameplay::CurrentGameplayBuildFingerprint(),
                                                                 Horo::Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)});
            REQUIRE(loaded.HasValue());
            auto gameModule = std::move(loaded).Value();
            auto binding = BindGameplayEvent(*gameModule, Binding, Schema, EventRuntimeContext::Headless, 1);
            REQUIRE(binding.HasValue());
            CHECK(BindGameplayEvent(*gameModule, Binding, Schema, EventRuntimeContext::Preview, 1).HasError());
            CHECK(BindGameplayEvent(*gameModule, {999, 1}, Schema, EventRuntimeContext::Headless, 1).HasError());
            REQUIRE(dispatcher.Register(std::move(binding).Value()).HasValue());
            return gameModule;
        }

        void ActivatePublication(CinematicEventSession &session, const SequencePlayerHandle &player, PublicationProbe &published) {
            const std::array tracks{
                SequenceFrameTrackDescriptor{{13, 1}, SequenceApplyStage::Property, &published, SamplePublication, ApplyPublication}};
            const std::array keys{SequenceFrameEventKey{Track, Key, 2, true}};
            auto plan = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, tracks, keys, {});
            REQUIRE(plan.HasValue());
            REQUIRE(session.Activate({{player, 10}, std::move(plan).Value()}, Cooked(), 0).HasValue());
        }

        void RetireModule(std::unique_ptr<Gameplay::LoadedGameModule> &gameModule, CinematicEventDispatcher &dispatcher) {
            // Retirement closes admission while the callback lease still pins the library.
            CHECK(gameModule->PrepareReload().HasError());
            CHECK(BindGameplayEvent(*gameModule, Binding, Schema, EventRuntimeContext::Headless, 2).HasError());
            gameModule.reset();
            REQUIRE(dispatcher.Drain(4).Value() == 1);
            CHECK(dispatcher.Results().back().outcome == EventDispatchOutcome::CapabilityUnavailable);
            REQUIRE(dispatcher.Revoke(Binding, 1).HasValue());
        }
    }  // namespace

    TEST_CASE("Native gameplay modules receive cooked events only after successful aggregate tick publication",
              "[unit][cinematic][event][native][rollback]") {
        Gameplay::GameModuleHost host;
        auto dispatcher = Dispatcher();
        auto gameModule = LoadCallback(host, dispatcher);
        auto created = CinematicRuntimeService::Create({Session, SequenceCookTier::Standard});
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        const SequencePlayerHandle player{Session, {1, 1}};
        const auto cooked = Cooked();
        CinematicEventSession session(runtime, dispatcher);
        PublicationProbe published;
        ActivatePublication(session, player, published);
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

        RetireModule(gameModule, dispatcher);
        REQUIRE(dispatcher.CancelPlayer(player).HasValue());
        REQUIRE(session.Close().HasValue());
    }

}  // namespace Horo::Cinematic
