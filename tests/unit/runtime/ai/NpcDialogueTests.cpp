#include "Horo/AI/NpcDialogue.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Horo::AI {
    namespace {
        template <typename Id> Id IdOf(std::uint64_t value) {
            return Id::Create(value).Value();
        }

        struct Scenario final {
            const NpcDialogueRequestIdentity request{{IdOf<AiRuntimeIncarnation>(1), {0, 1}}, 2, 3};
            const NpcDialogueRoute route{IdOf<DecisionProviderId>(1), IdOf<NpcDialogueDestinationId>(2), 1};
            const std::array<NpcDialogueFactId, 1> fields{IdOf<NpcDialogueFactId>(2)};
            const NpcDialogueGrant grant{route, request.providerGeneration, fields};
            const NpcDialogueCueId fallback{IdOf<NpcDialogueCueId>(4)};
            const NpcDialogueCueId selected{IdOf<NpcDialogueCueId>(5)};
            const std::array<NpcDialogueCueId, 2> cues{fallback, selected};
            std::array<NpcDialogueFact, 2> facts{NpcDialogueFact{IdOf<NpcDialogueFactId>(1), IdOf<NpcDialogueValueId>(19)},
                                               NpcDialogueFact{fields.front(), IdOf<NpcDialogueValueId>(20)}};
            static constexpr std::uint64_t Deadline = 10;

            auto Capture() const {
                return NpcDialogueTurn::Capture(request, route, grant, facts, cues, fallback, Deadline);
            }
        };
    }  // namespace

    TEST_CASE("NPC dialogue transports only copied context approved for its exact route", "[ai][npc-dialogue]") {
        Scenario scenario;
        const auto expected = scenario.facts.back().value;
        auto result = scenario.Capture();
        REQUIRE(result.HasValue());
        scenario.facts.back().value = IdOf<NpcDialogueValueId>(21);
        const auto context = result.Value().Context(scenario.request, scenario.grant, 1, {});
        REQUIRE(context.has_value());
        REQUIRE(context->size() == 1);
        CHECK(context->front().field == scenario.fields.front());
        CHECK(context->front().value == expected);

        auto denied = scenario.grant;
        denied.route.destination = IdOf<NpcDialogueDestinationId>(5);
        CHECK_FALSE(result.Value().Context(scenario.request, denied, 1, {}));
        CHECK(NpcDialogueTurn::Capture(scenario.request, scenario.route, denied, scenario.facts, scenario.cues, scenario.fallback,
                                       Scenario::Deadline).HasError());
        denied = scenario.grant;
        denied.fields = {};
        CHECK_FALSE(result.Value().Context(scenario.request, denied, 1, {}));
    }

    TEST_CASE("NPC dialogue admits an authored cue exactly once", "[ai][npc-dialogue]") {
        const Scenario scenario;
        auto result = scenario.Capture();
        REQUIRE(result.HasValue());
        auto turn = std::move(result).Value();
        auto old = scenario.request;
        ++old.turn;
        const NpcDialogueReply reply{scenario.request, scenario.selected, true};
        CHECK(turn.Resolve(old, scenario.grant, 1, {}, reply).status == NpcDialogueStatus::Stale);
        CHECK(turn.Resolve(scenario.request, scenario.grant, 1, {}, NpcDialogueReply{old, scenario.selected, true}).status ==
              NpcDialogueStatus::Stale);
        const auto outcome = turn.Resolve(scenario.request, scenario.grant, 1, {}, reply);
        CHECK(outcome.status == NpcDialogueStatus::Selected);
        CHECK(outcome.cue == scenario.selected);
        CHECK(turn.Resolve(scenario.request, scenario.grant, 1, {}, reply).status == NpcDialogueStatus::Consumed);
        CHECK_FALSE(turn.Context(scenario.request, scenario.grant, 1, {}));
    }

    TEST_CASE("NPC dialogue falls back on failed, denied, unsupported or late replies", "[ai][npc-dialogue]") {
        const Scenario scenario;
        auto result = scenario.Capture();
        REQUIRE(result.HasValue());
        auto grant = scenario.grant;
        auto tick = 1ULL;
        std::optional reply{NpcDialogueReply{scenario.request, scenario.selected, true}};
        SECTION("offline") { reply.reset(); }
        SECTION("rejected by moderation") { reply->approved = false; }
        SECTION("not an authored cue") { reply->cue = IdOf<NpcDialogueCueId>(18); }
        SECTION("revoked activation") { ++grant.providerGeneration; }
        SECTION("exclusive deadline elapsed") { tick = Scenario::Deadline; }
        auto turn = std::move(result).Value();
        const auto outcome = turn.Resolve(scenario.request, grant, tick, {}, reply);
        CHECK(outcome.status == NpcDialogueStatus::Fallback);
        CHECK(outcome.cue == scenario.fallback);
    }

    TEST_CASE("NPC dialogue never publishes after cancellation", "[ai][npc-dialogue]") {
        const Scenario scenario;
        auto result = scenario.Capture();
        REQUIRE(result.HasValue());
        CancellationSource source;
        source.RequestCancellation();
        CHECK_FALSE(result.Value().Context(scenario.request, scenario.grant, 1, source.Token()));
        auto turn = std::move(result).Value();
        const auto outcome = turn.Resolve(scenario.request, scenario.grant, 1, source.Token(), std::nullopt);
        CHECK(outcome.status == NpcDialogueStatus::Cancelled);
        CHECK_FALSE(outcome.cue.IsValid());
    }

    TEST_CASE("NPC dialogue rejects oversize and ambiguous context", "[ai][npc-dialogue]") {
        Scenario scenario;
        std::array<NpcDialogueFact, NpcDialogueTurn::MaximumFacts + 1> oversize{};
        CHECK(NpcDialogueTurn::Capture(scenario.request, scenario.route, scenario.grant, oversize, scenario.cues, scenario.fallback,
                                       Scenario::Deadline).HasError());
        scenario.facts.front().field = scenario.fields.front();
        CHECK(scenario.Capture().HasError());
    }

    namespace {
        using namespace std::chrono_literals;

        class ScriptedDialogueProvider final : public INpcDialogueProvider {
        public:
            NpcDialogueCueId answer;
            bool throws{};
            mutable NpcDialogueFact observed;
            mutable std::thread::id worker;
            mutable std::size_t factCount{};

            Result<NpcDialogueCueId> RequestCue(std::span<const NpcDialogueFact> facts, std::chrono::steady_clock::time_point,
                                               const CancellationToken &) const override {
                worker = std::this_thread::get_id();
                factCount = facts.size();
                if (!facts.empty())
                    observed = facts.front();
                if (throws)
                    throw std::runtime_error("fixture provider detail");
                if (answer.IsValid())
                    return Result<NpcDialogueCueId>::Success(answer);
                return Result<NpcDialogueCueId>::Failure(MakeError(AIErrors::CapabilityUnavailable));
            }
        };

        std::optional<NpcDialogueOutcome> WaitForDialogue(NpcDialogueJob &job, const Scenario &scenario) {
            const auto stop = std::chrono::steady_clock::now() + 2s;
            do {
                if (const auto outcome = job.Poll(scenario.request, scenario.grant, 1))
                    return outcome;
                std::this_thread::sleep_for(1ms);
            } while (std::chrono::steady_clock::now() < stop);
            return std::nullopt;
        }
    }  // namespace

    TEST_CASE("NPC dialogue worker sees only approved facts and publishes a cue on the owner lane", "[ai][npc-dialogue][jobs]") {
        const Scenario scenario;
        JobSystem jobs({.ioWorkerCount = 1});
        const auto provider = std::make_shared<ScriptedDialogueProvider>();
        auto expected = NpcDialogueStatus::Selected;
        auto cue = scenario.selected;
        SECTION("provider admits the authored cue") { provider->answer = scenario.selected; }
        SECTION("provider failure selects the authored fallback") {
            expected = NpcDialogueStatus::Fallback;
            cue = scenario.fallback;
        }
        SECTION("provider exception is isolated from the job record") {
            provider->throws = true;
            expected = NpcDialogueStatus::Fallback;
            cue = scenario.fallback;
        }
        const NpcDialogueProviderBinding binding{scenario.route, scenario.request.providerGeneration, std::make_shared<int>(1), provider};
        auto captured = scenario.Capture();
        REQUIRE(captured.HasValue());
        auto queued = NpcDialogueJob::Start(jobs, std::move(captured).Value(), scenario.request, scenario.grant, binding, 1,
                                            std::chrono::steady_clock::now() + 5s, {});
        REQUIRE(queued.HasValue());
        auto job = std::move(queued).Value();
        const auto outcome = WaitForDialogue(job, scenario);
        REQUIRE(outcome.has_value());
        REQUIRE(outcome->status == expected);
        CHECK(outcome->cue == cue);
        CHECK(provider->worker != std::this_thread::get_id());
        CHECK(provider->factCount == 1);
        CHECK(provider->observed.field == scenario.fields.front());
        CHECK(job.Poll(scenario.request, scenario.grant, 1)->status == NpcDialogueStatus::Consumed);
        const auto records = jobs.SnapshotIfChanged(0);
        REQUIRE(records.has_value());
        REQUIRE(records->jobs.size() == 1);
        CHECK_FALSE(records->jobs.front().error.has_value());
    }

    TEST_CASE("Rejected NPC dialogue job admission leaves the turn available for fallback", "[ai][npc-dialogue][jobs]") {
        const Scenario scenario;
        JobSystem jobs({.ioWorkerCount = 0});
        const auto provider = std::make_shared<ScriptedDialogueProvider>();
        const NpcDialogueProviderBinding binding{scenario.route, scenario.request.providerGeneration, std::make_shared<int>(1), provider};
        auto captured = scenario.Capture();
        REQUIRE(captured.HasValue());
        auto turn = std::move(captured).Value();
        CHECK(NpcDialogueJob::Start(jobs, std::move(turn), scenario.request, scenario.grant, binding, 1,
                                    std::chrono::steady_clock::now() + 5s, {}).HasError());
        CHECK(turn.Resolve(scenario.request, scenario.grant, 1, {}, std::nullopt).cue == scenario.fallback);
    }

    TEST_CASE("Queued NPC dialogue cannot disclose context after the owner revokes its grant", "[ai][npc-dialogue][jobs]") {
        const Scenario scenario;
        std::promise<void> release;
        JobSystem jobs({.ioWorkerCount = 1});
        const auto blocker = jobs.Submit({.resource = JobResource::Io}, [ready = release.get_future().share()](const CancellationToken &) {
            static_cast<void>(ready.wait_for(2s));
        });
        REQUIRE(blocker.HasValue());
        const auto provider = std::make_shared<ScriptedDialogueProvider>();
        const NpcDialogueProviderBinding binding{scenario.route, scenario.request.providerGeneration, std::make_shared<int>(1), provider};
        auto captured = scenario.Capture();
        REQUIRE(captured.HasValue());
        auto queued = NpcDialogueJob::Start(jobs, std::move(captured).Value(), scenario.request, scenario.grant, binding, 1,
                                            std::chrono::steady_clock::now() + 5s, {});
        REQUIRE(queued.HasValue());
        auto job = std::move(queued).Value();
        auto revoked = scenario.grant;
        revoked.fields = {};
        const auto outcome = job.Poll(scenario.request, revoked, 1);
        release.set_value();
        jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(outcome.has_value());
        CHECK(outcome->cue == scenario.fallback);
        CHECK(provider->worker == std::thread::id{});
    }
}  // namespace Horo::AI
