#include "PlayTopologyInternal.h"

#include <algorithm>
#include <array>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace Horo::Application {
    namespace {
        using Json = nlohmann::ordered_json;

        /** @brief Reject duplicate keys and nesting before accepting a closed profile document. */
        struct Guard final {
            std::array<std::set<std::string>, 6> keys;
            bool invalid{};

            bool operator()(int depth, Json::parse_event_t event, const Json &value) {
                if (depth < 0 || depth >= static_cast<int>(keys.size())) {
                    invalid = true;
                    return false;
                }
                const auto index = static_cast<std::size_t>(event == Json::parse_event_t::key ? depth - 1 : depth);
                if (index >= keys.size()) {
                    invalid = true;
                    return false;
                }
                if (event == Json::parse_event_t::object_start)
                    keys[index].clear();
                if (event == Json::parse_event_t::key && !keys[index].insert(value.get<std::string>()).second)
                    invalid = true;
                return !invalid;
            }
        };

        /** @brief Require the exact field vocabulary, including every mandatory field. */
        bool Fields(const Json &value, std::initializer_list<const char *> names) {
            return value.is_object() && value.size() == names.size() && std::ranges::all_of(names, [&](const char *name) {
                return value.contains(name);
            });
        }

        /** @brief Decode unsigned integers without narrowing hostile input. */
        bool Number(const Json &value, const char *key, std::uint64_t &output, std::uint64_t maximum) {
            const auto &field = value.at(key);
            if (!field.is_number_unsigned())
                return false;
            output = field.get<std::uint64_t>();
            return output <= maximum;
        }

        /** @brief Accept only the current schema and a nonzero unsigned revision. */
        bool VersionAndRevision(const Json &root) {
            std::uint64_t version{}, revision{};
            return Number(root, "version", version, 1) && version == 1 &&
                   Number(root, "revision", revision, std::numeric_limits<std::uint64_t>::max()) && revision != 0;
        }

        /** @brief Decode topology counts into the private candidate before semantic validation. */
        bool DecodeShape(const Json &value, PlayTopologyProfile &profile) {
            std::uint64_t kind{}, servers{}, clients{};
            if (!Number(value, "kind", kind, static_cast<std::uint64_t>(PlayTopologyKind::Count) - 1) ||
                !Number(value, "servers", servers, 1) || !Number(value, "clients", clients, 8))
                return false;
            profile.kind = static_cast<PlayTopologyKind>(kind);
            profile.serverCount = static_cast<std::uint8_t>(servers);
            profile.clientCount = static_cast<std::uint8_t>(clients);
            return true;
        }

        /** @brief Decode network-only scalars without integer narrowing or implicit provider selection. */
        bool DecodeNetwork(const Json &value, PlayTopologyProfile &profile) {
            std::uint64_t provider{}, port{}, preset{};
            if (!Number(value, "transport", provider, std::numeric_limits<std::uint64_t>::max()) || !Number(value, "port", port, 65535) ||
                !Number(value, "simulationPreset", preset, std::numeric_limits<std::uint64_t>::max()))
                return false;
            profile.transport =
                provider == 0 ? Network::NetworkTransportProviderId{} : Network::NetworkTransportProviderId::Create(provider).Value();
            profile.port = static_cast<std::uint16_t>(port);
            profile.simulationPreset = preset;
            return true;
        }

        /** @brief Decode the common bounded root without admitting unknown versions or fields. */
        Result<Json> Root(std::string_view document, const char *entries) {
            if (document.empty() || document.size() > 65536)
                return Result<Json>::Failure(MakeError(PlayTopologyErrors::Invalid));
            Guard guard;
            Json root = Json::parse(document, std::ref(guard), false);
            if (guard.invalid || root.is_discarded() || !Fields(root, {"version", "revision", entries}) || !VersionAndRevision(root) ||
                !root.at(entries).is_array() || root.at(entries).size() > 16)
                return Result<Json>::Failure(MakeError(PlayTopologyErrors::Invalid));
            return Result<Json>::Success(std::move(root));
        }

        /** @brief Decode closed profile scalars before semantic validation. */
        Result<PlayTopologyProfile> Profile(const Json &value) {
            using Parsed = Result<PlayTopologyProfile>;
            if (!Fields(value, {"id", "name", "kind", "servers", "clients", "map", "transport", "port", "simulationPreset"}) ||
                !value.at("name").is_string() || !value.at("map").is_string())
                return Parsed::Failure(MakeError(PlayTopologyErrors::Invalid));
            PlayTopologyProfile profile;
            if (!Number(value, "id", profile.id, std::numeric_limits<std::uint64_t>::max()) || !DecodeShape(value, profile) ||
                !DecodeNetwork(value, profile))
                return Parsed::Failure(MakeError(PlayTopologyErrors::Invalid));
            profile.name = value.at("name").get<std::string>();
            profile.map = value.at("map").get<std::string>();
            if (auto valid = ValidatePlayTopology(profile); valid.HasError())
                return Parsed::Failure(valid.ErrorValue());
            return Parsed::Success(std::move(profile));
        }

        /** @brief Encode only portable fields in a fixed canonical field order. */
        Json Encode(const PlayTopologyProfile &profile) {
            return Json{{"id", profile.id},
                        {"name", profile.name},
                        {"kind", profile.kind},
                        {"servers", profile.serverCount},
                        {"clients", profile.clientCount},
                        {"map", profile.map},
                        {"transport", profile.transport.Value()},
                        {"port", profile.port},
                        {"simulationPreset", profile.simulationPreset}};
        }
    }  // namespace

    /** @copydoc ParsePlayTopologies */
    Result<PlayTopologyCatalog> ParsePlayTopologies(std::string_view document) {
        const auto root = Root(document, "profiles");
        if (root.HasError())
            return Result<PlayTopologyCatalog>::Failure(root.ErrorValue());
        PlayTopologyCatalog catalog{.revision = root.Value().at("revision").get<std::uint64_t>()};
        for (const auto &value : root.Value().at("profiles")) {
            auto parsed = Profile(value);
            if (parsed.HasError())
                return Result<PlayTopologyCatalog>::Failure(parsed.ErrorValue());
            catalog.profiles.push_back(std::move(parsed).Value());
        }
        if (!Detail::ValidPlayTopologyCatalog(catalog))
            return Result<PlayTopologyCatalog>::Failure(MakeError(PlayTopologyErrors::Invalid));
        std::ranges::sort(catalog.profiles, {}, &PlayTopologyProfile::id);
        return Result<PlayTopologyCatalog>::Success(std::move(catalog));
    }

    /** @copydoc SerializePlayTopologies */
    Result<std::string> SerializePlayTopologies(const PlayTopologyCatalog &catalog) {
        if (!Detail::ValidPlayTopologyCatalog(catalog))
            return Result<std::string>::Failure(MakeError(PlayTopologyErrors::Invalid));
        auto profiles = catalog.profiles;
        std::ranges::sort(profiles, {}, &PlayTopologyProfile::id);
        Json root{{"version", 1}, {"revision", catalog.revision}, {"profiles", Json::array()}};
        for (const auto &profile : profiles)
            root["profiles"].push_back(Encode(profile));
        return Result<std::string>::Success(root.dump(2) + '\n');
    }

    /** @copydoc ParsePlayTopologyOverrides */
    Result<PlayTopologyUserSettings> ParsePlayTopologyOverrides(std::string_view document) {
        const auto root = Root(document, "overrides");
        if (root.HasError())
            return Result<PlayTopologyUserSettings>::Failure(root.ErrorValue());
        PlayTopologyUserSettings settings{.revision = root.Value().at("revision").get<std::uint64_t>()};
        for (const auto &value : root.Value().at("overrides")) {
            std::uint64_t identity{}, port{};
            if (!Fields(value, {"profile", "port"}) || !Number(value, "profile", identity, std::numeric_limits<std::uint64_t>::max()) ||
                !Number(value, "port", port, 65535))
                return Result<PlayTopologyUserSettings>::Failure(MakeError(PlayTopologyErrors::Invalid));
            settings.overrides.push_back({identity, static_cast<std::uint16_t>(port)});
        }
        if (!Detail::ValidPlayTopologyOverrides(settings))
            return Result<PlayTopologyUserSettings>::Failure(MakeError(PlayTopologyErrors::Invalid));
        std::ranges::sort(settings.overrides, {}, &PlayTopologyOverride::profile);
        return Result<PlayTopologyUserSettings>::Success(std::move(settings));
    }

    /** @copydoc SerializePlayTopologyOverrides */
    Result<std::string> SerializePlayTopologyOverrides(const PlayTopologyUserSettings &settings) {
        if (!Detail::ValidPlayTopologyOverrides(settings))
            return Result<std::string>::Failure(MakeError(PlayTopologyErrors::Invalid));
        auto overrides = settings.overrides;
        std::ranges::sort(overrides, {}, &PlayTopologyOverride::profile);
        Json root{{"version", 1}, {"revision", settings.revision}, {"overrides", Json::array()}};
        for (const auto &value : overrides)
            root["overrides"].push_back({{"profile", value.profile}, {"port", value.port}});
        return Result<std::string>::Success(root.dump(2) + '\n');
    }
}  // namespace Horo::Application
