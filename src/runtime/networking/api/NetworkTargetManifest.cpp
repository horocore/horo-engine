#include "Horo/Network/NetworkTargetCapabilities.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace Horo::Network {
    namespace Detail {
        bool ValidateNetworkProductCapabilityManifest(const NetworkProductCapabilityManifest &product);
    }

    namespace {
        using Json = nlohmann::json;
        constexpr std::size_t MaximumManifestBytes = 16 * 1024;

        bool HasFields(const Json &value, const std::initializer_list<std::string_view> fields) {
            if (!value.is_object() || value.size() != fields.size())
                return false;
            return std::ranges::all_of(fields, [&value](const std::string_view field) {
                return value.contains(field);
            });
        }

        bool ReadUnsigned(const Json &object, const char *key, std::uint64_t &out, const std::uint64_t maximum) {
            if (const auto &value = object.at(key); value.is_number_unsigned()) {
                out = value.get<std::uint64_t>();
            } else if (value.is_number_integer()) {
                const auto signedValue = value.get<std::int64_t>();
                if (signedValue < 0)
                    return false;
                out = static_cast<std::uint64_t>(signedValue);
            } else {
                return false;
            }
            return out <= maximum;
        }

        struct ParseGuard final {
            std::vector<std::set<std::string>> keys;
            bool rejected{};

            bool operator()(const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth < 0 || depth > 5) {
                    rejected = true;
                    return false;
                }
                const auto index =
                    event == Json::parse_event_t::key && depth > 0 ? static_cast<std::size_t>(depth - 1) : static_cast<std::size_t>(depth);
                if (keys.size() <= index)
                    keys.resize(index + 1);
                if (event == Json::parse_event_t::object_start)
                    keys[index].clear();
                if (event == Json::parse_event_t::key && !keys[index].insert(value.get<std::string>()).second)
                    rejected = true;
                if (event == Json::parse_event_t::object_end)
                    keys[index].clear();
                return !rejected;
            }
        };

        Json WriteProtocol(const NetworkProjectProtocolPolicy &p) {
            return {{"protocol", p.protocol.Value()},
                    {"minimumMajor", p.supportedVersions.minimum.major},
                    {"minimumMinor", p.supportedVersions.minimum.minor},
                    {"maximumMajor", p.supportedVersions.maximum.major},
                    {"maximumMinor", p.supportedVersions.maximum.minor},
                    {"schemaFingerprint", p.schemaFingerprint}};
        }

        bool ReadProtocol(const Json &value, NetworkProjectProtocolPolicy &out) {
            if (!HasFields(value, {"protocol", "minimumMajor", "minimumMinor", "maximumMajor", "maximumMinor", "schemaFingerprint"}))
                return false;
            std::uint64_t number{};
            if (!ReadUnsigned(value, "protocol", number, UINT16_MAX) || number == 0)
                return false;
            out.protocol = ProtocolId::Create(static_cast<std::uint16_t>(number)).Value();
            if (!ReadUnsigned(value, "minimumMajor", number, UINT16_MAX))
                return false;
            out.supportedVersions.minimum.major = static_cast<std::uint16_t>(number);
            if (!ReadUnsigned(value, "minimumMinor", number, UINT16_MAX))
                return false;
            out.supportedVersions.minimum.minor = static_cast<std::uint16_t>(number);
            if (!ReadUnsigned(value, "maximumMajor", number, UINT16_MAX))
                return false;
            out.supportedVersions.maximum.major = static_cast<std::uint16_t>(number);
            if (!ReadUnsigned(value, "maximumMinor", number, UINT16_MAX))
                return false;
            out.supportedVersions.maximum.minor = static_cast<std::uint16_t>(number);
            return ReadUnsigned(value, "schemaFingerprint", out.schemaFingerprint, UINT64_MAX) && out.supportedVersions.IsValid() &&
                   out.schemaFingerprint != 0;
        }

        Json WriteCapabilities(const TransportCapabilities &c) {
            Json delivery = Json::array();
            for (const auto support : c.delivery)
                delivery.push_back(static_cast<std::uint8_t>(support));
            return {{"contractVersion", c.contractVersion},
                    {"revision", c.revision},
                    {"delivery", delivery},
                    {"maximumChannels", c.maximumChannels},
                    {"maximumMessageBytes", c.maximumMessageBytes},
                    {"deadlines", static_cast<std::uint8_t>(c.deadlines)},
                    {"maximumDeadlineMilliseconds", c.maximumDeadlineMilliseconds}};
        }

        bool ReadCapabilities(const Json &value, TransportCapabilities &out) {
            if (!HasFields(value, {"contractVersion", "revision", "delivery", "maximumChannels", "maximumMessageBytes", "deadlines",
                                   "maximumDeadlineMilliseconds"}) ||
                !value.at("delivery").is_array() || value.at("delivery").size() != out.delivery.size())
                return false;
            std::uint64_t number{};
            if (!ReadUnsigned(value, "contractVersion", number, UINT32_MAX))
                return false;
            out.contractVersion = static_cast<std::uint32_t>(number);
            if (!ReadUnsigned(value, "revision", out.revision, UINT64_MAX))
                return false;
            for (std::size_t index = 0; index < out.delivery.size(); ++index) {
                const auto &item = value.at("delivery").at(index);
                if (!item.is_number_integer())
                    return false;
                const auto support = item.get<std::int64_t>();
                if (support < 0 || support >= static_cast<std::int64_t>(TransportSupport::Count))
                    return false;
                out.delivery[index] = static_cast<TransportSupport>(support);
            }
            if (!ReadUnsigned(value, "maximumChannels", number, UINT32_MAX))
                return false;
            out.maximumChannels = static_cast<std::uint32_t>(number);
            if (!ReadUnsigned(value, "maximumMessageBytes", out.maximumMessageBytes, UINT64_MAX) ||
                !ReadUnsigned(value, "deadlines", number, static_cast<std::uint8_t>(TransportSupport::Count) - 1))
                return false;
            out.deadlines = static_cast<TransportSupport>(number);
            if (!ReadUnsigned(value, "maximumDeadlineMilliseconds", number, UINT32_MAX))
                return false;
            out.maximumDeadlineMilliseconds = static_cast<std::uint32_t>(number);
            return ValidateTransportCapabilities(out);
        }

        bool ReadManifestIdentity(const Json &value, NetworkProductCapabilityManifest &product) {
            std::uint64_t number{};
            if (!ReadUnsigned(value, "contractVersion", number, UINT32_MAX))
                return false;
            product.contractVersion = static_cast<std::uint32_t>(number);
            if (!ReadUnsigned(value, "build", number, UINT64_MAX) || number == 0)
                return false;
            product.build = NetworkProductBuildId::Create(number).Value();
            if (!ReadUnsigned(value, "revision", number, UINT64_MAX) || number == 0)
                return false;
            product.revision = NetworkProductCapabilityRevision::Create(number).Value();
            if (!ReadUnsigned(value, "platform", number, static_cast<std::uint8_t>(NetworkTargetPlatform::Count) - 1))
                return false;
            product.platform = static_cast<NetworkTargetPlatform>(number);
            if (!ReadUnsigned(value, "supportedRoles", number, UINT32_MAX))
                return false;
            product.supportedRoles = static_cast<NetworkProjectRoleSet>(number);
            if (!value.at("includesNetworkRuntime").is_boolean())
                return false;
            product.includesNetworkRuntime = value.at("includesNetworkRuntime").get<bool>();
            if (!ReadUnsigned(value, "profile", number, UINT64_MAX) || number == 0)
                return false;
            product.profile = NetworkProjectProfileId::Create(number).Value();
            if (!ReadUnsigned(value, "profileRevision", number, UINT64_MAX) || number == 0)
                return false;
            product.profileRevision = NetworkProjectProfileRevision::Create(number).Value();
            if (product.includesNetworkRuntime)
                return ReadProtocol(value.at("protocol"), product.protocol);
            return value.at("protocol").is_object() && value.at("protocol").empty();
        }

        bool ReadManifestProviders(const Json &value, NetworkProductCapabilityManifest &product) {
            const auto &providers = value.at("providers");
            if (!providers.is_array() || providers.size() > MaximumNetworkTargetProviders)
                return false;
            product.providerCount = providers.size();
            for (std::size_t index = 0; index < product.providerCount; ++index) {
                const auto &item = providers.at(index);
                std::uint64_t number{};
                if (!HasFields(item, {"id", "allowedPlatforms", "capabilities"}) || !ReadUnsigned(item, "id", number, UINT64_MAX) ||
                    number == 0)
                    return false;
                product.providers[index].id = NetworkTransportProviderId::Create(number).Value();
                if (!ReadUnsigned(item, "allowedPlatforms", number, UINT32_MAX))
                    return false;
                product.providers[index].allowedPlatforms = static_cast<NetworkTargetPlatformMask>(number);
                if (!ReadCapabilities(item.at("capabilities"), product.providers[index].capabilities))
                    return false;
            }
            return true;
        }

        Result<NetworkProductCapabilityManifest> InvalidManifest() {
            return Result<NetworkProductCapabilityManifest>::Failure(MakeError(NetworkErrors::NetworkTargetManifestInvalid));
        }
    }  // namespace

    /** @copydoc SerializeNetworkProductCapabilityManifest */
    Result<std::string> SerializeNetworkProductCapabilityManifest(const NetworkProductCapabilityManifest &product) {
        if (!Detail::ValidateNetworkProductCapabilityManifest(product))
            return Result<std::string>::Failure(MakeError(NetworkErrors::NetworkTargetManifestInvalid));
        Json providers = Json::array();
        for (std::size_t index = 0; index < product.providerCount; ++index) {
            const auto &provider = product.providers[index];
            providers.push_back({{"id", provider.id.Value()},
                                 {"allowedPlatforms", provider.allowedPlatforms},
                                 {"capabilities", WriteCapabilities(provider.capabilities)}});
        }
        const Json value = {{"contractVersion", product.contractVersion},
                            {"build", product.build.Value()},
                            {"revision", product.revision.Value()},
                            {"platform", static_cast<std::uint8_t>(product.platform)},
                            {"supportedRoles", static_cast<std::uint32_t>(product.supportedRoles)},
                            {"includesNetworkRuntime", product.includesNetworkRuntime},
                            {"profile", product.profile.Value()},
                            {"profileRevision", product.profileRevision.Value()},
                            {"protocol", product.includesNetworkRuntime ? WriteProtocol(product.protocol) : Json::object()},
                            {"providers", providers}};
        auto document = value.dump();
        if (document.size() > MaximumManifestBytes)
            return Result<std::string>::Failure(MakeError(NetworkErrors::NetworkTargetManifestCapacityExceeded));
        return Result<std::string>::Success(std::move(document));
    }

    /** @copydoc ParseNetworkProductCapabilityManifest */
    Result<NetworkProductCapabilityManifest> ParseNetworkProductCapabilityManifest(const std::string_view document) {
        if (document.size() > MaximumManifestBytes)
            return Result<NetworkProductCapabilityManifest>::Failure(MakeError(NetworkErrors::NetworkTargetManifestCapacityExceeded));
        ParseGuard guard;
        const Json value = Json::parse(document, std::ref(guard), false, true);
        if (guard.rejected || value.is_discarded() ||
            !HasFields(value, {"contractVersion", "build", "revision", "platform", "supportedRoles", "includesNetworkRuntime", "profile",
                               "profileRevision", "protocol", "providers"}))
            return InvalidManifest();
        NetworkProductCapabilityManifest product{};
        if (!ReadManifestIdentity(value, product) || !ReadManifestProviders(value, product) ||
            !Detail::ValidateNetworkProductCapabilityManifest(product))
            return InvalidManifest();
        return Result<NetworkProductCapabilityManifest>::Success(product);
    }
}  // namespace Horo::Network
