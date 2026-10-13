#include "AnimationStateGraphTestFixtures.h"

#include <nlohmann/json.hpp>

namespace Horo::Animation::Test {
    TEST_CASE("Animation state graph source canonicalizes states and transitions and projects exact dependencies",
              "[animation][state_graph][source]") {
        auto graph = StateGraph::Data();
        auto &machine = StateGraph::Machine(graph);
        std::ranges::reverse(machine.states);
        std::ranges::reverse(machine.transitions);
        const auto encoded = SerializeAnimationGraphSource(graph);
        REQUIRE(encoded.HasValue());
        const auto decoded = DeserializeAnimationGraphSource(encoded.Value(), graph.id);
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().schemaVersion == 4);
        CHECK(decoded.Value().contractVersion == (AnimationGraphContractVersion{1, 2, 0}));
        const auto reencoded = SerializeAnimationGraphSource(decoded.Value());
        REQUIRE(reencoded.HasValue());
        CHECK(reencoded.Value() == encoded.Value());
        const auto bytes = nlohmann::json::parse(encoded.Value());
        CHECK(bytes["dependencies"].size() == 3);
        Instance::Prepared fixture(decoded.Value());
        CHECK(fixture.program->Memory().stateMachines == 1);
        CHECK(fixture.program->Memory().conditionLatches == 2);
        CHECK(fixture.program->Memory().stateEvents == 262);
        const auto occurrences = fixture.program->Occurrences();
        std::uint64_t actualEntries = 0;
        for (const auto &occurrence : occurrences)
            actualEntries += occurrence.callPath.size() + occurrence.guards.size();
        CHECK(fixture.program->Definitions()[fixture.program->EntryIndex()].evaluationInstructions == occurrences.size());
        CHECK(fixture.program->Definitions()[fixture.program->EntryIndex()].sourceMapCallEntries == actualEntries);
    }

    TEST_CASE("Animation state graph source explicitly migrates earlier contracts without admitting state payloads",
              "[animation][state_graph][migration]") {
        const auto graph = Graph::Simple();
        const auto encoded = SerializeAnimationGraphSource(graph);
        REQUIRE(encoded.HasValue());
        const auto canonical = MigrateAnimationGraph(graph);
        REQUIRE(canonical.HasValue());
        for (const std::uint32_t schema : {1U, 2U, 3U}) {
            auto old = nlohmann::json::parse(encoded.Value());
            old["schemaVersion"] = schema;
            old["contractVersion"] = {{"major", 1}, {"minor", schema == 3 ? 1 : 0}, {"patch", 0}};
            if (schema < 3)
                for (auto &definition : old["payload"]["definitions"])
                    for (auto &node : definition["nodes"])
                        node.erase("settings");
            if (schema == 1)
                for (auto &definition : old["payload"]["definitions"])
                    for (auto &edge : definition["connections"])
                        edge.erase("type");
            CHECK(DeserializeAnimationGraphSource(old.dump(), graph.id).HasError());
            const auto migrated =
                DeserializeAnimationGraphSource(old.dump(), graph.id, {}, {}, AnimationGraphSourceMigration::MigrateSupported);
            REQUIRE(migrated.HasValue());
            CHECK(migrated.Value() == canonical.Value());
        }
        auto invalid = StateGraph::Data();
        invalid.schemaVersion = 3;
        invalid.contractVersion = {1, 1, 0};
        CHECK(MigrateAnimationGraph(invalid).HasError());
    }

    TEST_CASE("Animation state graph compiler rejects recursive state definitions and malformed typed rules", "[animation][state_graph]") {
        auto graph = StateGraph::Data();
        auto &machine = StateGraph::Machine(graph);
        SECTION("recursion") {
            machine.states[0].source = graph.entry;
        }
        SECTION("duplicate states") {
            machine.states[1].id = machine.states[0].id;
        }
        SECTION("missing initial") {
            machine.initial = Id<GraphStateId>(99);
        }
        SECTION("missing transition state") {
            machine.transitions[0].destination = Id<GraphStateId>(99);
        }
        SECTION("missing parameter") {
            machine.transitions[0].conditions[0].parameter = Id<GraphParameterId>(99);
        }
        SECTION("predicate type") {
            machine.transitions[0].conditions[0].threshold = true;
        }
        SECTION("invalid hysteresis") {
            machine.transitions[0].conditions[0].releaseThreshold = 2.0F;
        }
        SECTION("clip clock competing truth") {
            machine.states[0].loopPeriod = AnimationDeltaTime{100};
        }
        CHECK(SerializeAnimationGraphSource(graph).HasError());
    }

    TEST_CASE("Animation state graph hostile source and caller-lowered storage bounds reject before publication",
              "[animation][state_graph][source]") {
        auto graph = StateGraph::Data();
        const auto encoded = SerializeAnimationGraphSource(graph);
        REQUIRE(encoded.HasValue());
        auto source = nlohmann::json::parse(encoded.Value());
        auto &machine = source["payload"]["definitions"][0]["nodes"][1]["reference"];
        SECTION("unknown state field") {
            machine["states"][0]["extra"] = 1;
        }
        SECTION("fractional duration") {
            machine["transitions"][0]["duration"] = 0.5;
        }
        SECTION("unknown curve") {
            machine["transitions"][0]["curve"] = "unknown";
        }
        SECTION("duplicate identity") {
            machine["states"][1]["id"] = machine["states"][0]["id"];
        }
        SECTION("state budget") {
            AnimationGraphCompileContext context;
            context.limits.states = 2;
            CHECK(DeserializeAnimationGraphSource(source.dump(), graph.id, context).HasError());
            return;
        }
        CHECK(DeserializeAnimationGraphSource(source.dump(), graph.id).HasError());
    }
}  // namespace Horo::Animation::Test
