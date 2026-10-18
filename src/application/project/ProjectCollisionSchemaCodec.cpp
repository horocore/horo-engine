#include "ProjectCollisionCodecInternal.h"

namespace Horo::Application::CollisionCodecInternal {
    namespace {
        /** @brief Reads explicit layer eligibility with no native masks or implicit policy. */
        Physics::CollisionLayerDefinition Layer(const Json &v) {
            Fields(v, {"id", "enabled", "static", "kinematic", "dynamic", "overlap"});
            return {Identity<Physics::CollisionLayerId>(v["id"]),
                    Boolean(v["enabled"]),
                    Boolean(v["static"]),
                    Boolean(v["kinematic"]),
                    Boolean(v["dynamic"]),
                    Boolean(v["overlap"])};
        }

        /** @brief Reads a complete reusable profile including every explicitly authored query response. */
        Physics::CollisionProfileDefinition Profile(const Json &v) {
            Fields(v, {"id", "layer", "simulation", "query", "events", "responses"});
            Array(v["responses"], 64);
            Physics::CollisionProfileDefinition p{Identity<Physics::CollisionProfileId>(v["id"]),
                                                  Identity<Physics::CollisionLayerId>(v["layer"]),
                                                  Boolean(v["simulation"]),
                                                  Boolean(v["query"]),
                                                  Boolean(v["events"]),
                                                  {}};
            p.queryResponses.reserve(v["responses"].size());
            for (const auto &r : v["responses"]) {
                Fields(r, {"channel", "response"});
                p.queryResponses.push_back({Identity<Physics::PhysicsQueryChannelId>(r["channel"]),
                                            static_cast<Physics::CollisionQueryResponse>(Unsigned(r["response"], 2))});
            }
            return p;
        }
    }  // namespace

    /** @copydoc ReadSchema */
    Physics::ProjectCollisionSchema ReadSchema(const Json &root) {
        Fields(root, {"version", "features", "defaultProfile", "layers", "pairs", "channels", "profiles"});
        Array(root["layers"], 4096);
        Array(root["pairs"], 2080);
        Array(root["channels"], 4096);
        Array(root["profiles"], 1024);
        Physics::ProjectCollisionSchema schema;
        schema.schemaVersion = static_cast<std::uint32_t>(Unsigned(root["version"], UINT32_MAX));
        schema.requiredFeatureBits = Unsigned(root["features"]);
        schema.defaultProfile = Identity<Physics::CollisionProfileId>(root["defaultProfile"]);
        for (const auto &v : root["layers"])
            schema.layers.push_back(Layer(v));
        for (const auto &v : root["pairs"]) {
            Fields(v, {"first", "second", "response"});
            schema.pairs.emplace_back(Identity<Physics::CollisionLayerId>(v["first"]), Identity<Physics::CollisionLayerId>(v["second"]),
                                      static_cast<Physics::SimulationPairResponse>(Unsigned(v["response"], 2)));
        }
        for (const auto &v : root["channels"]) {
            Fields(v, {"id", "enabled"});
            schema.queryChannels.push_back({Identity<Physics::PhysicsQueryChannelId>(v["id"]), Boolean(v["enabled"])});
        }
        for (const auto &v : root["profiles"])
            schema.profiles.push_back(Profile(v));
        return schema;
    }

    /** @copydoc WriteSchema */
    Json WriteSchema(const ProjectCollisionSettings &settings) {
        const auto &s = *settings.Schema();
        Json layers = Json::array();
        Json pairs = Json::array();
        Json channels = Json::array();
        Json profiles = Json::array();
        for (const auto &v : s.Layers())
            layers.push_back({{"id", v.id.ToString()},
                              {"enabled", v.enabled},
                              {"static", v.admitsStatic},
                              {"kinematic", v.admitsKinematic},
                              {"dynamic", v.admitsDynamic},
                              {"overlap", v.admitsOverlap}});
        for (const auto &v : s.Pairs())
            pairs.push_back(
                {{"first", v.first.ToString()}, {"second", v.second.ToString()}, {"response", static_cast<unsigned>(v.response)}});
        for (const auto &v : s.QueryChannels())
            channels.push_back({{"id", v.id.ToString()}, {"enabled", v.enabled}});
        for (const auto &v : s.Profiles()) {
            Json responses = Json::array();
            for (const auto &r : v.queryResponses)
                responses.push_back({{"channel", r.channel.ToString()}, {"response", static_cast<unsigned>(r.response)}});
            profiles.push_back({{"id", v.id.ToString()},
                                {"layer", v.layer.ToString()},
                                {"simulation", v.simulationEnabled},
                                {"query", v.queryEnabled},
                                {"events", v.lifecycleEventsEnabled},
                                {"responses", std::move(responses)}});
        }
        return {{"version", settings.Input().schema.schemaVersion},
                {"features", settings.Input().schema.requiredFeatureBits},
                {"defaultProfile", s.DefaultProfile().ToString()},
                {"layers", std::move(layers)},
                {"pairs", std::move(pairs)},
                {"channels", std::move(channels)},
                {"profiles", std::move(profiles)}};
    }
}  // namespace Horo::Application::CollisionCodecInternal
