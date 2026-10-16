#include "Horo/Application/NavigationProjectSettings.h"

#include <algorithm>
#include <array>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <vector>

namespace Horo::Application {
    using namespace Navigation;

    namespace {
        using Json = nlohmann::json;

        /** @brief Requires the entire closed object shape before field access. */
        bool Fields(const Json &value, std::initializer_list<std::string_view> fields) {
            return value.is_object() && value.size() == fields.size() && std::ranges::all_of(fields, [&](auto key) {
                return value.contains(key);
            });
        }

        /** @brief Rejects negative, fractional, overflowing or stringly numeric input. */
        std::uint64_t Unsigned(const Json &value, const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max()) {
            if (!value.is_number_unsigned() && (!value.is_number_integer() || value.get<std::int64_t>() < 0))
                throw std::invalid_argument("unsigned integer required");
            const auto number = value.get<std::uint64_t>();
            if (number > maximum)
                throw std::invalid_argument("integer capacity exceeded");
            return number;
        }

        /** @brief Emits the complete profile facts; no preview or generated provider state enters the wire. */
        Json Profile(const NavigationProjectProfileInput &p) {
            const auto &c = p.capacities;
            return {{"id", p.id.Value()},
                    {"revision", p.revision.Value()},
                    {"capacities",
                     {c.maximumAgents, c.maximumSurfaces, c.maximumResidentTiles, c.maximumConcurrentQueries, c.maximumBytesPerResidentTile,
                      c.maximumResidentMemoryBytes, c.maximumWorkUnitsPerTick}},
                    {"query",
                     {static_cast<unsigned>(p.maximumQuery.query), static_cast<unsigned>(p.maximumQuery.quality),
                      p.maximumQuery.limits.maximumNodeExpansions, p.maximumQuery.limits.maximumResultPoints,
                      p.maximumQuery.limits.maximumSearchDistanceMeters}},
                    {"requirements", p.capabilities}};
        }

        /** @brief Reads all finite profile fields before the owning profile validator runs. */
        NavigationProjectProfileInput ReadProfile(const Json &v) {
            if (!Fields(v, {"id", "revision", "capacities", "query", "requirements"}) || !v["capacities"].is_array() ||
                v["capacities"].size() != 7 || !v["query"].is_array() || v["query"].size() != 5 || !v["requirements"].is_array() ||
                v["requirements"].size() != static_cast<std::size_t>(NavigationCapability::Count))
                throw std::invalid_argument("complete profile required");
            const auto &c = v["capacities"];
            const auto &q = v["query"];
            NavigationProjectProfileInput p;
            const auto id = NavigationProjectProfileId::Create(Unsigned(v["id"]));
            const auto revision = NavigationProjectProfileRevision::Create(Unsigned(v["revision"]));
            if (id.HasError() || revision.HasError())
                throw std::invalid_argument("invalid profile identity");
            p.id = id.Value();
            p.revision = revision.Value();
            p.capacities = {static_cast<std::uint32_t>(Unsigned(c[0], UINT32_MAX)),
                            static_cast<std::uint32_t>(Unsigned(c[1], UINT32_MAX)),
                            static_cast<std::uint32_t>(Unsigned(c[2], UINT32_MAX)),
                            static_cast<std::uint32_t>(Unsigned(c[3], UINT32_MAX)),
                            Unsigned(c[4]),
                            Unsigned(c[5]),
                            Unsigned(c[6])};
            p.maximumQuery = {static_cast<NavigationQueryKind>(Unsigned(q[0], static_cast<unsigned>(NavigationQueryKind::Count) - 1)),
                              static_cast<NavigationQualityLevel>(Unsigned(q[1], static_cast<unsigned>(NavigationQualityLevel::Count) - 1)),
                              {static_cast<std::uint32_t>(Unsigned(q[2], UINT32_MAX)),
                               static_cast<std::uint32_t>(Unsigned(q[3], UINT32_MAX)), q[4].get<float>()}};
            for (std::size_t i = 0; i < p.capabilities.size(); ++i)
                p.capabilities[i] = static_cast<NavigationCapabilityRequirement>(Unsigned(v["requirements"][i], 1));
            return p;
        }
    }  // namespace

    /** @copydoc EncodeNavigationProjectSettings */
    std::string EncodeNavigationProjectSettings(const NavigationProjectSettings &settings) {
        const auto &v = settings.Input();
        return Json{{"schema", 2},
                    {"revision", v.revision},
                    {"definition", v.definition.ToString()},
                    {"defaultProvider", v.defaultProvider},
                    {"runtimeProfile", Profile(v.runtimeProfile)}}
                   .dump() +
               "\n";
    }

    /** @copydoc DecodeNavigationProjectSettings */
    Result<NavigationProjectSettings> DecodeNavigationProjectSettings(const std::string_view source) {
        const auto failure = [](auto reason) {
            return Result<NavigationProjectSettings>::Failure(MakeError(NavigationSettingsError(reason)));
        };
        if (source.empty() || source.size() > 64 * 1024)
            return failure(NavigationSettingsFailure::Capacity);
        try {
            std::vector<std::set<std::string>> keys;
            const auto callback = [&](int depth, Json::parse_event_t event, Json &value) {
                if (depth > 12)
                    throw std::invalid_argument("depth exceeded");
                if (event == Json::parse_event_t::object_start)
                    keys.emplace_back();
                if (event == Json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
                    throw std::invalid_argument("duplicate key");
                if (event == Json::parse_event_t::object_end)
                    keys.pop_back();
                return true;
            };
            const auto root = Json::parse(source, callback);
            if (!root.is_object() || !root.contains("schema"))
                return failure(NavigationSettingsFailure::Invalid);
            const auto schema = Unsigned(root["schema"]);
            if (schema != 1 && schema != 2)
                return failure(NavigationSettingsFailure::UnsupportedVersion);
            const auto providerKey = schema == 1 ? "provider" : "defaultProvider";
            if (!Fields(root, {"schema", "revision", "definition", providerKey, "runtimeProfile"}))
                return failure(NavigationSettingsFailure::Invalid);
            const auto definition = Assets::AssetId::Parse(root["definition"].get<std::string>());
            if (definition.HasError())
                return Result<NavigationProjectSettings>::Failure(definition.ErrorValue());
            return NavigationProjectSettings::Create({Unsigned(root["revision"]), definition.Value(), root[providerKey].get<std::string>(),
                                                      ReadProfile(root["runtimeProfile"])});
        } catch (const std::bad_alloc &) {
            return failure(NavigationSettingsFailure::Capacity);
        } catch (const std::exception &) {
            return failure(NavigationSettingsFailure::Invalid);
        }
    }
}  // namespace Horo::Application
