#include "AnimationGraphTestFixtures.h"
#include "Horo/Animation/AnimationGraphSource.h"

#include <algorithm>
#include <limits>
#include <nlohmann/json.hpp>
using namespace Horo;
using namespace Horo::Animation;
using namespace Horo::Animation::Test;
using namespace Horo::Animation::Test::Graph;

TEST_CASE("Graph source round trips all scalar defaults and canonical identities", "[animation][graph][codec]") {
    auto data = Blended();
    data.entry = Id<GraphDefinitionId>(std::numeric_limits<std::uint64_t>::max());
    data.definitions.front().id = data.entry;
    data.parameters.push_back({Id<GraphParameterId>(2), "enabled", GraphValueType::Boolean, true});
    data.parameters.push_back({Id<GraphParameterId>(3), "mode", GraphValueType::Integer, std::numeric_limits<std::int32_t>::min()});
    data.parameters.push_back({Id<GraphParameterId>(4), "fire", GraphValueType::Trigger, false});
    const auto encoded = SerializeAnimationGraphSource(data);
    REQUIRE(encoded.HasValue());
    CHECK(encoded.Value().back() == '\n');
    CHECK(encoded.Value().find(data.id.Asset().ToString()) == std::string::npos);
    const auto decoded = DeserializeAnimationGraphSource(encoded.Value(), data.id);
    REQUIRE(decoded.HasValue());
    const auto canonical = MigrateAnimationGraph(data);
    REQUIRE(canonical.HasValue());
    CHECK(decoded.Value() == canonical.Value());
    CHECK(CompileGraph(decoded.Value()).HasValue());
    const auto repeated = SerializeAnimationGraphSource(decoded.Value());
    REQUIRE(repeated.HasValue());
    CHECK(encoded.Value() == repeated.Value());
    std::reverse(data.parameters.begin(), data.parameters.end());
    std::reverse(data.definitions.front().nodes.begin(), data.definitions.front().nodes.end());
    std::reverse(data.definitions.front().connections.begin(), data.definitions.front().connections.end());
    for (auto &node : data.definitions.front().nodes)
        std::reverse(node.pins.begin(), node.pins.end());
    const auto shuffled = SerializeAnimationGraphSource(data);
    REQUIRE(shuffled.HasValue());
    CHECK(shuffled.Value() == encoded.Value());
}

TEST_CASE("Graph canonical source normalizes signed zero and object field order", "[animation][graph][codec]") {
    auto data = Blended();
    data.parameters.front().defaultValue = -0.0F;
    const auto negative = SerializeAnimationGraphSource(data);
    data.parameters.front().defaultValue = 0.0F;
    const auto positive = SerializeAnimationGraphSource(data);
    REQUIRE(negative.HasValue());
    REQUIRE(positive.HasValue());
    CHECK(negative.Value() == positive.Value());
    auto source = nlohmann::ordered_json::parse(positive.Value());
    source["contractVersion"] = {{"patch", 0}, {"minor", 0}, {"major", 1}};
    std::reverse(source["dependencies"].begin(), source["dependencies"].end());
    for (auto &dependency : source["dependencies"]) {
        auto reordered = nlohmann::ordered_json::object();
        reordered["assetId"] = dependency["assetId"];
        reordered["assetType"] = dependency["assetType"];
        dependency = std::move(reordered);
    }
    const auto decoded = DeserializeAnimationGraphSource(source.dump(), data.id);
    REQUIRE(decoded.HasValue());
    const auto canonical = SerializeAnimationGraphSource(decoded.Value());
    REQUIRE(canonical.HasValue());
    CHECK(canonical.Value() == positive.Value());
}

TEST_CASE("Graph source rejects duplicate keys invalid encoding and trailing documents", "[animation][graph][codec]") {
    const auto data = Blended();
    const auto encoded = SerializeAnimationGraphSource(data);
    REQUIRE(encoded.HasValue());
    std::string raw;
    SECTION("duplicate envelope key") {
        raw = "{\"schemaVersion\":2," + encoded.Value().substr(1);
    }
    SECTION("duplicate nested key") {
        raw = encoded.Value();
        const auto at = raw.find("\"entry\":1");
        REQUIRE(at != std::string::npos);
        raw.replace(at, 9, "\"entry\":1,\"entry\":1");
    }
    SECTION("embedded NUL before trailing data") {
        raw = encoded.Value() + std::string(1, '\0') + "{}";
    }
    SECTION("trailing document") {
        raw = encoded.Value() + "{}";
    }
    SECTION("truncation") {
        raw = encoded.Value().substr(0, encoded.Value().size() - 2);
    }
    SECTION("invalid UTF8") {
        raw = encoded.Value();
        raw.insert(raw.find("weight"), 1, static_cast<char>(255));
    }
    SECTION("BOM") {
        raw = std::string("\xEF\xBB\xBF") + encoded.Value();
    }
    const auto failed = DeserializeAnimationGraphSource(raw, data.id);
    REQUIRE(failed.HasError());
}

TEST_CASE("Graph source rejects unknown fields malformed identities and node kinds", "[animation][graph][codec]") {
    const auto data = Blended();
    const auto encoded = SerializeAnimationGraphSource(data);
    REQUIRE(encoded.HasValue());
    auto source = nlohmann::ordered_json::parse(encoded.Value());
    SECTION("unknown root key") {
        source["unrecognized"] = 1;
    }
    SECTION("forged primary identity") {
        source["payload"]["id"] = data.id.Asset().ToString();
    }
    SECTION("missing field") {
        source["payload"].erase("entry");
    }
    SECTION("zero node id") {
        source["payload"]["definitions"][0]["nodes"][0]["id"] = 0;
    }
    SECTION("floating node id") {
        source["payload"]["definitions"][0]["nodes"][0]["id"] = 2.0;
    }
    SECTION("negative node id") {
        source["payload"]["definitions"][0]["nodes"][0]["id"] = -2;
    }
    SECTION("unknown kind") {
        source["payload"]["definitions"][0]["nodes"][0]["kind"] = "stateMachine";
    }
    SECTION("noncanonical UUID") {
        source["payload"]["skeleton"] = "{00000000-0000-0000-0000-000000000002}";
    }
    const std::string raw = source.dump();
    const auto failed = DeserializeAnimationGraphSource(raw, data.id);
    REQUIRE(failed.HasError());
}

TEST_CASE("Graph source rejects invalid defaults and dependency projections", "[animation][graph][codec]") {
    const auto data = Blended();
    const auto encoded = SerializeAnimationGraphSource(data);
    REQUIRE(encoded.HasValue());
    auto source = nlohmann::ordered_json::parse(encoded.Value());
    SECTION("default overflow") {
        source["payload"]["parameters"][0]["default"] = 1e100;
    }
    SECTION("type coercion") {
        source["payload"]["parameters"][0]["default"] = "0.5";
    }
    SECTION("duplicate parameter id") {
        source["payload"]["parameters"].push_back(source["payload"]["parameters"][0]);
    }
    SECTION("missing dependency") {
        source["dependencies"].erase(source["dependencies"].begin());
    }
    SECTION("duplicate dependency") {
        source["dependencies"].push_back(source["dependencies"][0]);
    }
    SECTION("wrong dependency type") {
        source["dependencies"][0]["assetType"] = "core.mesh";
    }
    const std::string raw = source.dump();
    const auto failed = DeserializeAnimationGraphSource(raw, data.id);
    REQUIRE(failed.HasError());
}

TEST_CASE("Graph source parser and encoder enforce exact byte count depth token and typed container budgets", "[animation][graph][codec]") {
    const auto data = Blended();
    const auto encoded = SerializeAnimationGraphSource(data);
    REQUIRE(encoded.HasValue());
    AnimationGraphSourceLimits limits;
    limits.bytes = encoded.Value().size();
    CHECK(DeserializeAnimationGraphSource(encoded.Value(), data.id, {}, limits).HasValue());
    CHECK(SerializeAnimationGraphSource(data, {}, limits).HasValue());
    SECTION("byte capacity") {
        --limits.bytes;
    }
    SECTION("depth capacity") {
        limits.depth = 1;
    }
    SECTION("parser values") {
        limits.jsonValues = 1;
    }
    SECTION("hard source ceiling") {
        limits.bytes = AnimationGraphSourceHardLimits::Bytes + 1;
    }
    SECTION("typed node ceiling") {
        AnimationGraphCompileContext context;
        context.limits.nodes = 4;
        const auto failed = DeserializeAnimationGraphSource(encoded.Value(), data.id, context);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().code.Value() == AnimationErrors::GraphLimitExceeded.code.Value());
        return;
    }
    const auto read = DeserializeAnimationGraphSource(encoded.Value(), data.id, {}, limits);
    const auto write = SerializeAnimationGraphSource(data, {}, limits);
    REQUIRE(read.HasError());
    REQUIRE(write.HasError());
    CHECK(read.ErrorValue().code.Value() == AnimationErrors::GraphLimitExceeded.code.Value());
    CHECK(write.ErrorValue().code.Value() == AnimationErrors::GraphLimitExceeded.code.Value());
}

TEST_CASE("Graph source version migration is explicit canonical and transactional", "[animation][graph][codec]") {
    const auto data = Blended();
    const auto current = SerializeAnimationGraphSource(data);
    REQUIRE(current.HasValue());
    auto legacy = nlohmann::ordered_json::parse(current.Value());
    legacy["schemaVersion"] = 1;
    for (auto &edge : legacy["payload"]["definitions"][0]["connections"])
        edge.erase("type");
    CHECK(DeserializeAnimationGraphSource(legacy.dump(), data.id).HasError());
    const auto migrated = DeserializeAnimationGraphSource(legacy.dump(), data.id, {}, {}, AnimationGraphSourceMigration::MigrateVersion1);
    REQUIRE(migrated.HasValue());
    const auto rewritten = SerializeAnimationGraphSource(migrated.Value());
    REQUIRE(rewritten.HasValue());
    CHECK(rewritten.Value() == current.Value());
    legacy["schemaVersion"] = 99;
    CHECK(DeserializeAnimationGraphSource(legacy.dump(), data.id, {}, {}, AnimationGraphSourceMigration::MigrateVersion1).HasError());
    legacy = nlohmann::ordered_json::parse(current.Value());
    legacy["contractVersion"]["major"] = 2;
    CHECK(DeserializeAnimationGraphSource(legacy.dump(), data.id).HasError());
    AnimationGraphCompileContext context;
    context.replacing = data.id;
    const auto wrongIdentity = DeserializeAnimationGraphSource(current.Value(), Asset<AnimationGraphId>(9), context);
    REQUIRE(wrongIdentity.HasError());
    CHECK(wrongIdentity.ErrorValue().code.Value() == AnimationErrors::GraphReloadMismatch.code.Value());
    CancellationSource cancellation;
    context.cancellation = cancellation.Token();
    cancellation.RequestCancellation();
    const auto cancelled = DeserializeAnimationGraphSource(current.Value(), data.id, context);
    REQUIRE(cancelled.HasError());
    CHECK(cancelled.ErrorValue().code.Value() == AnimationErrors::GraphOperationCancelled.code.Value());
    CHECK(SerializeAnimationGraphSource(data, context).HasError());
    context.accepting = false;
    const auto closed = DeserializeAnimationGraphSource(current.Value(), data.id, context);
    REQUIRE(closed.HasError());
    CHECK(closed.ErrorValue().code.Value() == AnimationErrors::GraphAdmissionRejected.code.Value());
    CHECK(CompileGraph(data).HasValue());
}

namespace {
    /** @brief Exercises each public ingress with matching typed dependencies and colliding UUID roles. */
    void RejectAssetRoleCollision(const AnimationGraphData &data, const std::string &source) {
        const auto compiled = CompileGraph(data);
        const auto migrated = MigrateAnimationGraph(data);
        const auto serialized = SerializeAnimationGraphSource(data);
        const auto deserialized = DeserializeAnimationGraphSource(source, data.id);
        REQUIRE(compiled.HasError());
        REQUIRE(migrated.HasError());
        REQUIRE(serialized.HasError());
        REQUIRE(deserialized.HasError());
        CHECK(compiled.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        CHECK(migrated.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        CHECK(serialized.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
        CHECK(deserialized.ErrorValue().code.Value() == AnimationErrors::GraphMalformed.code.Value());
    }
}  // namespace

TEST_CASE("Graph source and compiler reject a primary identity reused as the skeleton", "[animation][graph][codec]") {
    auto data = Simple();
    const auto valid = SerializeAnimationGraphSource(data);
    REQUIRE(valid.HasValue());
    data.id = Asset<AnimationGraphId>(2);
    RejectAssetRoleCollision(data, valid.Value());
}

TEST_CASE("Graph source and compiler reject a primary identity reused as a clip", "[animation][graph][codec]") {
    auto data = Simple();
    const auto valid = SerializeAnimationGraphSource(data);
    REQUIRE(valid.HasValue());
    data.id = Asset<AnimationGraphId>(3);
    RejectAssetRoleCollision(data, valid.Value());
}

TEST_CASE("Graph source and compiler reject a skeleton identity reused as a clip", "[animation][graph][codec]") {
    auto data = Simple();
    const auto valid = SerializeAnimationGraphSource(data);
    REQUIRE(valid.HasValue());
    auto source = nlohmann::ordered_json::parse(valid.Value());
    data.skeleton = Asset<SkeletonId>(3);
    source["payload"]["skeleton"] = data.skeleton.Asset().ToString();
    for (auto &dependency : source["dependencies"])
        if (dependency["assetType"] == "core.animation.skeleton")
            dependency["assetId"] = data.skeleton.Asset().ToString();
    RejectAssetRoleCollision(data, source.dump());
}
