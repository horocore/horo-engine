#include "Horo/AI/NpcDialogue.h"

#include <catch2/catch_test_macros.hpp>
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
}  // namespace Horo::AI
