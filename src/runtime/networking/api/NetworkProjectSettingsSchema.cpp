#include "Horo/Network/NetworkProjectSettings.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace Horo::Network {
    namespace {
        using Json = nlohmann::json;

        /** @brief Accepts only an exact closed object shape. */
        bool HasFields(const Json &object, const std::initializer_list<std::string_view> fields) {
            if (!object.is_object() || object.size() != fields.size())
                return false;
            return std::ranges::all_of(fields, [&](const std::string_view field) {
                return object.contains(field);
            });
        }

        /** @brief Reads a non-negative integer without narrowing or floating-point conversion. */
        bool ReadUnsigned(const Json &object, const char *key, std::uint64_t &out, const std::uint64_t maximum) {
            if (const Json &value = object.at(key); value.is_number_unsigned()) {
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

        /** @brief Parses the complete fixed profile with an explicit field list. */
        bool ReadProfile(const Json &value, NetworkProjectProfileV1 &profile) {
            if (!HasFields(value, {"id",
                                   "revision",
                                   "networkTickRate",
                                   "maxActiveConnections",
                                   "maxNetworkObjects",
                                   "maxCandidatesPerConnectionPerTick",
                                   "maxRelevantObjectsPerConnection",
                                   "maxInterestTransitionsPerConnectionPerTick",
                                   "maxCapturedObjectsPerTick",
                                   "maxSerializedFieldsPerConnectionPerTick",
                                   "maxGlobalInterestWorkPerTick",
                                   "maxGlobalCaptureWorkPerTick",
                                   "maxGlobalSchedulingWorkPerTick",
                                   "targetBytesPerConnection",
                                   "burstBytesPerConnection",
                                   "maxMessagesPerConnectionPerTick",
                                   "maxReliableQueuedBytesPerConnection",
                                   "maxUnreliableQueuedBytesPerConnection",
                                   "enterDwellTicks",
                                   "exitDwellTicks",
                                   "maxEligibleStarvationTicks",
                                   "saturationGraceTicks"}))
                return false;
            std::uint64_t number{};
#define READ_PROFILE_ID(field, type)                                                                                                       \
    if (!ReadUnsigned(value, #field, number, std::numeric_limits<std::uint64_t>::max()) || number == 0)                                    \
        return false;                                                                                                                      \
    profile.field = type::Create(number).Value();
#define READ_PROFILE32(field)                                                                                                              \
    if (!ReadUnsigned(value, #field, number, std::numeric_limits<std::uint32_t>::max()))                                                   \
        return false;                                                                                                                      \
    profile.field.value = static_cast<std::uint32_t>(number);
#define READ_PROFILE64(field)                                                                                                              \
    if (!ReadUnsigned(value, #field, number, std::numeric_limits<std::uint64_t>::max()))                                                   \
        return false;                                                                                                                      \
    profile.field.value = number;
            READ_PROFILE_ID(id, NetworkProjectProfileId)
            READ_PROFILE_ID(revision, NetworkProjectProfileRevision)
            READ_PROFILE32(networkTickRate)
            READ_PROFILE32(maxActiveConnections)
            READ_PROFILE32(maxNetworkObjects)
            READ_PROFILE32(maxCandidatesPerConnectionPerTick)
            READ_PROFILE32(maxRelevantObjectsPerConnection)
            READ_PROFILE32(maxInterestTransitionsPerConnectionPerTick)
            READ_PROFILE32(maxCapturedObjectsPerTick)
            READ_PROFILE32(maxSerializedFieldsPerConnectionPerTick)
            READ_PROFILE64(maxGlobalInterestWorkPerTick)
            READ_PROFILE64(maxGlobalCaptureWorkPerTick)
            READ_PROFILE64(maxGlobalSchedulingWorkPerTick)
            READ_PROFILE64(targetBytesPerConnection)
            READ_PROFILE64(burstBytesPerConnection)
            READ_PROFILE32(maxMessagesPerConnectionPerTick)
            READ_PROFILE64(maxReliableQueuedBytesPerConnection)
            READ_PROFILE64(maxUnreliableQueuedBytesPerConnection)
            READ_PROFILE32(enterDwellTicks)
            READ_PROFILE32(exitDwellTicks)
            READ_PROFILE32(maxEligibleStarvationTicks)
            READ_PROFILE32(saturationGraceTicks)
#undef READ_PROFILE_ID
#undef READ_PROFILE32
#undef READ_PROFILE64
            return true;
        }

        /** @brief Emits every profile member as a named exact integer. */
        Json WriteProfile(const NetworkProjectProfileV1 &p) {
            return {{"id", p.id.Value()},
                    {"revision", p.revision.Value()},
                    {"networkTickRate", p.networkTickRate.value},
                    {"maxActiveConnections", p.maxActiveConnections.value},
                    {"maxNetworkObjects", p.maxNetworkObjects.value},
                    {"maxCandidatesPerConnectionPerTick", p.maxCandidatesPerConnectionPerTick.value},
                    {"maxRelevantObjectsPerConnection", p.maxRelevantObjectsPerConnection.value},
                    {"maxInterestTransitionsPerConnectionPerTick", p.maxInterestTransitionsPerConnectionPerTick.value},
                    {"maxCapturedObjectsPerTick", p.maxCapturedObjectsPerTick.value},
                    {"maxSerializedFieldsPerConnectionPerTick", p.maxSerializedFieldsPerConnectionPerTick.value},
                    {"maxGlobalInterestWorkPerTick", p.maxGlobalInterestWorkPerTick.value},
                    {"maxGlobalCaptureWorkPerTick", p.maxGlobalCaptureWorkPerTick.value},
                    {"maxGlobalSchedulingWorkPerTick", p.maxGlobalSchedulingWorkPerTick.value},
                    {"targetBytesPerConnection", p.targetBytesPerConnection.value},
                    {"burstBytesPerConnection", p.burstBytesPerConnection.value},
                    {"maxMessagesPerConnectionPerTick", p.maxMessagesPerConnectionPerTick.value},
                    {"maxReliableQueuedBytesPerConnection", p.maxReliableQueuedBytesPerConnection.value},
                    {"maxUnreliableQueuedBytesPerConnection", p.maxUnreliableQueuedBytesPerConnection.value},
                    {"enterDwellTicks", p.enterDwellTicks.value},
                    {"exitDwellTicks", p.exitDwellTicks.value},
                    {"maxEligibleStarvationTicks", p.maxEligibleStarvationTicks.value},
                    {"saturationGraceTicks", p.saturationGraceTicks.value}};
        }

        /** @brief Formats typed endpoint identity without using diagnostic projections. */
        std::string WriteEndpoint(const NetworkAddress &endpoint) {
            if (!endpoint.IsValid())
                return {};
            std::string result;
            if (endpoint.Kind() == NetworkAddressKind::DnsHostname) {
                result.assign(endpoint.Hostname());
            } else if (endpoint.Kind() == NetworkAddressKind::Ipv4) {
                const auto bytes = endpoint.AddressBytes();
                for (std::size_t index = 0; index < bytes.size(); ++index) {
                    if (index != 0)
                        result.push_back('.');
                    result += std::to_string(bytes[index]);
                }
            } else {
                const auto bytes = endpoint.AddressBytes();
                result.push_back('[');
                for (std::size_t index = 0; index < bytes.size(); index += 2) {
                    if (index != 0)
                        result.push_back(':');
                    const auto group = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[index]) << 8U) | bytes[index + 1]);
                    std::array<char, 4> digits{};
                    const auto [end, error] = std::to_chars(digits.data(), digits.data() + digits.size(), group, 16);
                    if (error != std::errc{})
                        return {};
                    result.append(digits.data(), end);
                }
                result.push_back(']');
            }
            result.push_back(':');
            result += std::to_string(endpoint.Port());
            return result;
        }

        /** @brief Rejects duplicate keys and excessive nesting during parsing. */
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

        /** @brief Decodes the exact protocol identity and version interval. */
        bool ReadProtocol(const Json &protocol, NetworkProjectProtocolPolicy &output) {
            if (!HasFields(protocol, {"protocol", "minimumMajor", "minimumMinor", "maximumMajor", "maximumMinor", "schemaFingerprint"}))
                return false;
            std::uint64_t number{};
            if (!ReadUnsigned(protocol, "protocol", number, std::numeric_limits<std::uint16_t>::max()) || number == 0)
                return false;
            output.protocol = ProtocolId::Create(static_cast<std::uint16_t>(number)).Value();
#define READ_VERSION(field, member)                                                                                                        \
    if (!ReadUnsigned(protocol, field, number, std::numeric_limits<std::uint16_t>::max()))                                                 \
        return false;                                                                                                                      \
    output.supportedVersions.member = static_cast<std::uint16_t>(number);
            READ_VERSION("minimumMajor", minimum.major)
            READ_VERSION("minimumMinor", minimum.minor)
            READ_VERSION("maximumMajor", maximum.major)
            READ_VERSION("maximumMinor", maximum.minor)
#undef READ_VERSION
            return ReadUnsigned(protocol, "schemaFingerprint", output.schemaFingerprint, std::numeric_limits<std::uint64_t>::max());
        }

        /** @brief Decodes exact required delivery and finite transport limits. */
        bool ReadTransport(const Json &transport, NetworkProjectTransportPolicy &output) {
            if (!HasFields(transport, {"requirement", "requiredDelivery", "requiredChannels", "requiredMaximumMessageBytes", "deadline",
                                       "requiredMaximumDeadlineMilliseconds"}))
                return false;
            std::uint64_t number{};
            if (!ReadUnsigned(transport, "requirement", number, std::numeric_limits<std::uint8_t>::max()))
                return false;
            output.requirement = static_cast<NetworkProjectTransportRequirement>(number);
            const auto &delivery = transport.at("requiredDelivery");
            if (!delivery.is_array() || delivery.size() != output.capabilities.requiredDelivery.size())
                return false;
            for (std::size_t index = 0; index < delivery.size(); ++index) {
                if (!delivery[index].is_boolean())
                    return false;
                output.capabilities.requiredDelivery[index] = delivery[index].get<bool>();
            }
            if (!ReadUnsigned(transport, "requiredChannels", number, std::numeric_limits<std::uint32_t>::max()))
                return false;
            output.capabilities.requiredChannels = static_cast<std::uint32_t>(number);
            if (!ReadUnsigned(transport, "requiredMaximumMessageBytes", output.capabilities.requiredMaximumMessageBytes,
                              std::numeric_limits<std::uint64_t>::max()))
                return false;
            if (!ReadUnsigned(transport, "deadline", number, std::numeric_limits<std::uint8_t>::max()))
                return false;
            output.capabilities.deadline = static_cast<DeadlineRequirement>(number);
            if (!ReadUnsigned(transport, "requiredMaximumDeadlineMilliseconds", number, std::numeric_limits<std::uint32_t>::max()))
                return false;
            output.capabilities.requiredMaximumDeadlineMilliseconds = static_cast<std::uint32_t>(number);
            return true;
        }

        /** @brief Decodes fields introduced in version two; version one keeps safe absent defaults. */
        bool ReadVersionTwo(const Json &root, NetworkProjectSettingsInput &input) {
            if (!root.at("defaultEndpoint").is_string())
                return false;
            if (const auto endpoint = root.at("defaultEndpoint").get<std::string>(); !endpoint.empty()) {
                if (const auto parsed = NetworkAddress::Parse(endpoint); parsed.HasValue())
                    input.defaultEndpoint = parsed.Value();
                else
                    return false;
            }
            std::uint64_t number{};
            if (!ReadUnsigned(root, "credentialRequirementId", number, std::numeric_limits<std::uint32_t>::max()))
                return false;
            input.credentialRequirementId = static_cast<std::uint32_t>(number);
            return true;
        }

        /** @brief Decodes the strictly closed portable representation. */
        bool Decode(const Json &root, NetworkProjectSettingsInput &input) {
            if (!root.is_object() || !root.contains("contractVersion"))
                return false;
            std::uint64_t number{};
            if (!ReadUnsigned(root, "contractVersion", number, NetworkProjectSettingsInput::CurrentContractVersion) || number == 0)
                return false;
            const bool legacy = number == 1;
            if (legacy ? !HasFields(root, {"contractVersion", "settings", "revision", "supportedRoles", "defaultRole", "profile",
                                           "protocol", "transport"})
                       : !HasFields(root, {"contractVersion", "settings", "revision", "supportedRoles", "defaultRole", "profile",
                                           "protocol", "transport", "defaultEndpoint", "credentialRequirementId"}))
                return false;
            input.contractVersion = NetworkProjectSettingsInput::CurrentContractVersion;
            if (!ReadUnsigned(root, "settings", number, std::numeric_limits<std::uint64_t>::max()) || number == 0)
                return false;
            input.settings = NetworkProjectSettingsId::Create(number).Value();
            if (!ReadUnsigned(root, "revision", number, std::numeric_limits<std::uint64_t>::max()) || number == 0)
                return false;
            input.revision = NetworkProjectSettingsRevision::Create(number).Value();
            if (!ReadUnsigned(root, "supportedRoles", number, std::numeric_limits<std::uint32_t>::max()))
                return false;
            input.supportedRoles = static_cast<NetworkProjectRoleSet>(number);
            if (!ReadUnsigned(root, "defaultRole", number, std::numeric_limits<std::uint8_t>::max()))
                return false;
            input.defaultRole = static_cast<NetworkProjectRole>(number);
            return ReadProfile(root.at("profile"), input.profile) && ReadProtocol(root.at("protocol"), input.protocol) &&
                   ReadTransport(root.at("transport"), input.transport) && (legacy || ReadVersionTwo(root, input));
        }
    }  // namespace

    /** @copydoc DefaultNetworkProjectSettings */
    Result<NetworkProjectSettingsInput> DefaultNetworkProjectSettings(const NetworkProjectSettingsId settings) {
        if (!settings.IsValid())
            return Result<NetworkProjectSettingsInput>::Failure(MakeError(NetworkErrors::IdentityInvalid));
        NetworkProjectSettingsInput input;
        input.settings = settings;
        input.revision = NetworkProjectSettingsRevision::Create(1).Value();
        input.supportedRoles = NetworkProjectRoleSet::Standalone;
        input.defaultRole = NetworkProjectRole::Standalone;
        auto &profile = input.profile;
        profile.id = NetworkProjectProfileId::Create(1).Value();
        profile.revision = NetworkProjectProfileRevision::Create(1).Value();
        profile.networkTickRate.value = 60;
        profile.maxActiveConnections.value = 16;
        profile.maxNetworkObjects.value = 1024;
        profile.maxCandidatesPerConnectionPerTick.value = 256;
        profile.maxRelevantObjectsPerConnection.value = 128;
        profile.maxInterestTransitionsPerConnectionPerTick.value = 64;
        profile.maxCapturedObjectsPerTick.value = 256;
        profile.maxSerializedFieldsPerConnectionPerTick.value = 1024;
        profile.maxGlobalInterestWorkPerTick.value = 1000;
        profile.maxGlobalCaptureWorkPerTick.value = 1000;
        profile.maxGlobalSchedulingWorkPerTick.value = 1000;
        profile.targetBytesPerConnection.value = 64'000;
        profile.burstBytesPerConnection.value = 128'000;
        profile.maxMessagesPerConnectionPerTick.value = 128;
        profile.maxReliableQueuedBytesPerConnection.value = 128'000;
        profile.maxUnreliableQueuedBytesPerConnection.value = 128'000;
        profile.enterDwellTicks.value = 2;
        profile.exitDwellTicks.value = 2;
        profile.maxEligibleStarvationTicks.value = 120;
        profile.saturationGraceTicks.value = 30;
        input.protocol.protocol = ProtocolId::Create(1).Value();
        input.protocol.supportedVersions = {.minimum = {.major = 1, .minor = 0}, .maximum = {.major = 1, .minor = 0}};
        input.protocol.schemaFingerprint = 1;
        if (const auto validated = NetworkProjectSettings::Create(input); validated.HasError())
            return Result<NetworkProjectSettingsInput>::Failure(validated.ErrorValue());
        return Result<NetworkProjectSettingsInput>::Success(input);
    }

    /** @copydoc SerializeNetworkProjectSettings */
    std::string SerializeNetworkProjectSettings(const NetworkProjectSettings &settings) {
        const auto &protocol = settings.Protocol();
        const auto &transport = settings.Transport();
        const Json document = {{"contractVersion", settings.ContractVersion()},
                               {"settings", settings.Settings().Value()},
                               {"revision", settings.Revision().Value()},
                               {"supportedRoles", static_cast<std::uint32_t>(settings.SupportedRoles())},
                               {"defaultRole", static_cast<std::uint8_t>(settings.DefaultRole())},
                               {"profile", WriteProfile(settings.Profile())},
                               {"protocol",
                                {{"protocol", protocol.protocol.Value()},
                                 {"minimumMajor", protocol.supportedVersions.minimum.major},
                                 {"minimumMinor", protocol.supportedVersions.minimum.minor},
                                 {"maximumMajor", protocol.supportedVersions.maximum.major},
                                 {"maximumMinor", protocol.supportedVersions.maximum.minor},
                                 {"schemaFingerprint", protocol.schemaFingerprint}}},
                               {"transport",
                                {{"requirement", static_cast<std::uint8_t>(transport.requirement)},
                                 {"requiredDelivery", transport.capabilities.requiredDelivery},
                                 {"requiredChannels", transport.capabilities.requiredChannels},
                                 {"requiredMaximumMessageBytes", transport.capabilities.requiredMaximumMessageBytes},
                                 {"deadline", static_cast<std::uint8_t>(transport.capabilities.deadline)},
                                 {"requiredMaximumDeadlineMilliseconds", transport.capabilities.requiredMaximumDeadlineMilliseconds}}},
                               {"defaultEndpoint", WriteEndpoint(settings.DefaultEndpoint())},
                               {"credentialRequirementId", settings.CredentialRequirementId()}};
        return document.dump();
    }

    /** @copydoc ParseNetworkProjectSettings */
    Result<NetworkProjectSettingsInput> ParseNetworkProjectSettings(const std::string_view document) {
        if (document.size() > 64 * 1024)
            return Result<NetworkProjectSettingsInput>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsCapacityExceeded));
        ParseGuard guard;
        const Json parsed = Json::parse(document, std::ref(guard), false, true);
        NetworkProjectSettingsInput input;
        if (guard.rejected || parsed.is_discarded() || !Decode(parsed, input))
            return Result<NetworkProjectSettingsInput>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));
        if (const auto validated = NetworkProjectSettings::Create(input); validated.HasError())
            return Result<NetworkProjectSettingsInput>::Failure(validated.ErrorValue());
        return Result<NetworkProjectSettingsInput>::Success(input);
    }

    /** @copydoc PreflightNetworkProjectSettings */
    Result<void> PreflightNetworkProjectSettings(const NetworkProjectSettings &settings, const NetworkProjectRole role,
                                                 const TransportCapabilities &capabilities) {
        if (!ContainsNetworkProjectRole(settings.SupportedRoles(), role))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));
        if (role == NetworkProjectRole::Standalone)
            return Result<void>::Success();
        if (settings.Transport().requirement != NetworkProjectTransportRequirement::Required)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));
        if (const auto selected = ResolveTransportCapabilities(capabilities, capabilities.revision, settings.Transport().capabilities);
            selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        return Result<void>::Success();
    }
}  // namespace Horo::Network
