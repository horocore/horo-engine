#include "Horo/Application/PlatformServicesDefinitionCodec.h"

#include "PlatformServicesAuthoringJson.h"

#include <limits>

namespace Horo::Application {
    using namespace PlatformServices;

    namespace {
        using namespace PlatformServicesAuthoringJson;
        constexpr std::size_t MaximumDocumentBytes = 4U * 1024U * 1024U;
        constexpr const char *Source = "platform-services/definitions";

        /** @brief Requires an exact boolean without numeric or string coercion. */
        bool Boolean(const Json &value) {
            if (!value.is_boolean())
                throw std::invalid_argument("boolean required");
            return value.get<bool>();
        }

        /** @brief Reads signed 64-bit endpoints without unsigned wraparound or floating conversion. */
        std::int64_t Signed(const Json &value) {
            if (!value.is_number_integer() ||
                (value.is_number_unsigned() &&
                 value.get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
                throw std::invalid_argument("signed integer required");
            return value.get<std::int64_t>();
        }

        /** @brief Requires canonical stable-ID text before projecting into a typed candidate. */
        std::uint64_t Id(const Json &value) {
            const auto parsed = ParsePlatformServiceStableId(Text(value, 21));
            if (parsed.HasError())
                throw std::invalid_argument("canonical identity required");
            return parsed.Value().value;
        }

        /** @brief Reads complete inclusive endpoints; the semantic validator checks range compatibility. */
        ProgressionNumericRange Range(const Json &value) {
            if (!Fields(value, {"minimum", "maximum"}))
                throw std::invalid_argument("complete range required");
            return {Signed(value["minimum"]), Signed(value["maximum"])};
        }

        /** @brief Projects one complete achievement without implementing semantic validation a second time. */
        AchievementDefinition Achievement(const Json &value) {
            if (!Fields(value, {"id", "authority", "progress", "presentation"}) || !Fields(value["progress"], {"kind", "total"}) ||
                !Fields(value["presentation"], {"titleLocalizationKey", "descriptionLocalizationKey", "hidden"}))
                throw std::invalid_argument("complete achievement required");
            return {.id = {Id(value["id"])},
                    .authority = static_cast<ProgressionAuthorityMode>(Number(value["authority"], 1)),
                    .progress = {.kind = static_cast<AchievementProgressKind>(Number(value["progress"]["kind"], 1)),
                                 .total = Number(value["progress"]["total"], UINT32_MAX)},
                    .presentation = {.titleLocalizationKey = Text(value["presentation"]["titleLocalizationKey"], MaximumDocumentBytes),
                                     .descriptionLocalizationKey =
                                         Text(value["presentation"]["descriptionLocalizationKey"], MaximumDocumentBytes),
                                     .hidden = Boolean(value["presentation"]["hidden"])}};
        }

        /** @brief Projects a stat with exact integer, enum and boolean representations. */
        StatDefinition Stat(const Json &value) {
            if (!Fields(value, {"id", "authority", "valueKind", "range", "mutation", "localizationKey", "hidden"}))
                throw std::invalid_argument("complete stat required");
            return {.id = {Id(value["id"])},
                    .authority = static_cast<ProgressionAuthorityMode>(Number(value["authority"], 1)),
                    .valueKind = static_cast<ProgressionValueKind>(Number(value["valueKind"], 1)),
                    .range = Range(value["range"]),
                    .mutation = static_cast<StatMutationPolicy>(Number(value["mutation"], 3)),
                    .localizationKey = Text(value["localizationKey"], MaximumDocumentBytes),
                    .hidden = Boolean(value["hidden"])};
        }

        /** @brief Projects a leaderboard, retaining its optional typed stat reference for shared validation. */
        LeaderboardDefinition Leaderboard(const Json &value) {
            if (!Fields(value, {"id", "authority", "valueKind", "range", "ordering", "sourceStat", "localizationKey", "hidden"}))
                throw std::invalid_argument("complete leaderboard required");
            std::optional<StatId> source;
            if (!value["sourceStat"].is_null())
                source = StatId{Id(value["sourceStat"])};
            return {.id = {Id(value["id"])},
                    .authority = static_cast<ProgressionAuthorityMode>(Number(value["authority"], 1)),
                    .valueKind = static_cast<ProgressionValueKind>(Number(value["valueKind"], 1)),
                    .range = Range(value["range"]),
                    .ordering = static_cast<LeaderboardOrdering>(Number(value["ordering"], 1)),
                    .sourceStat = source,
                    .localizationKey = Text(value["localizationKey"], MaximumDocumentBytes),
                    .hidden = Boolean(value["hidden"])};
        }

        /** @brief Projects bounded presence policy without accepting free-form provider fields. */
        PresenceDefinition Presence(const Json &value) {
            if (!Fields(value, {"id", "detailPolicy", "maximumDetailUtf8Bytes", "localizationKey", "hidden"}))
                throw std::invalid_argument("complete presence required");
            return {.id = {Id(value["id"])},
                    .detailPolicy = static_cast<PresenceDetailPolicy>(Number(value["detailPolicy"], 1)),
                    .maximumDetailUtf8Bytes = Number(value["maximumDetailUtf8Bytes"], UINT32_MAX),
                    .localizationKey = Text(value["localizationKey"], MaximumDocumentBytes),
                    .hidden = Boolean(value["hidden"])};
        }

        /** @brief Shares bounded document framing while leaving every semantic rule to the owning typed builder. */
        template <typename Candidate, typename Parse, typename Build>
        auto Decode(const std::string_view source, const PlatformStableIdRegistry &ids, const ErrorCodeDescriptor &invalid,
                    const ErrorCodeDescriptor &stale, Parse parse, Build build) {
            using Output = decltype(build(Candidate{}));
            const auto failure = [&] {
                return Output::Failure(
                    AuthoringError(invalid, Source, "", "Use a complete bounded definition document with exact field types."));
            };
            if (source.empty() || source.size() > MaximumDocumentBytes)
                return failure();
            try {
                const auto root = Document(source, 65536);
                if (!Fields(root, {"schemaVersion", "projectId", "stableIdRegistryFingerprint", "definitions"}) ||
                    !root["definitions"].is_array() || root["definitions"].size() > 4096)
                    return failure();
                if (Text(root["projectId"], PlatformServicesMaximumProjectIdBytes) != ids.ProjectId() || ids.ProjectId().empty())
                    return Output::Failure(AuthoringError(stale, Source, "projectId", "Capture definitions from the same project ledger."));
                const auto fingerprint = ParseSha256(Text(root["stableIdRegistryFingerprint"], 71));
                if (fingerprint.HasError())
                    return failure();
                Candidate candidate{.schemaVersion = Number(root["schemaVersion"], UINT32_MAX),
                                    .stableIdRegistryFingerprint = fingerprint.Value()};
                candidate.definitions.reserve(root["definitions"].size());
                for (const auto &value : root["definitions"])
                    candidate.definitions.push_back(parse(value));
                return build(candidate);
            } catch (const Json::exception &) {
                return failure();
            } catch (const std::invalid_argument &) {
                return failure();
            }
        }

        /** @brief Encodes complete typed achievement fields in fixed member order. */
        Json Entry(const AchievementDefinition &value) {
            return {{"id", FormatPlatformServiceStableId({value.id.value})},
                    {"authority", value.authority},
                    {"progress", {{"kind", value.progress.kind}, {"total", value.progress.total}}},
                    {"presentation",
                     {{"titleLocalizationKey", value.presentation.titleLocalizationKey},
                      {"descriptionLocalizationKey", value.presentation.descriptionLocalizationKey},
                      {"hidden", value.presentation.hidden}}}};
        }

        /** @brief Encodes complete typed stat fields with lossless signed endpoints. */
        Json Entry(const StatDefinition &value) {
            return {{"id", FormatPlatformServiceStableId({value.id.value})},
                    {"authority", value.authority},
                    {"valueKind", value.valueKind},
                    {"range", {{"minimum", value.range.minimum}, {"maximum", value.range.maximum}}},
                    {"mutation", value.mutation},
                    {"localizationKey", value.localizationKey},
                    {"hidden", value.hidden}};
        }

        /** @brief Encodes a missing stat reference as explicit JSON null. */
        Json Entry(const LeaderboardDefinition &value) {
            return {{"id", FormatPlatformServiceStableId({value.id.value})},
                    {"authority", value.authority},
                    {"valueKind", value.valueKind},
                    {"range", {{"minimum", value.range.minimum}, {"maximum", value.range.maximum}}},
                    {"ordering", value.ordering},
                    {"sourceStat", value.sourceStat ? Json(FormatPlatformServiceStableId({value.sourceStat->value})) : Json(nullptr)},
                    {"localizationKey", value.localizationKey},
                    {"hidden", value.hidden}};
        }

        /** @brief Encodes complete typed presence fields in fixed member order. */
        Json Entry(const PresenceDefinition &value) {
            return {{"id", FormatPlatformServiceStableId({value.id.value})},
                    {"detailPolicy", value.detailPolicy},
                    {"maximumDetailUtf8Bytes", value.maximumDetailUtf8Bytes},
                    {"localizationKey", value.localizationKey},
                    {"hidden", value.hidden}};
        }

        /** @brief Uses immutable ID-sorted storage as the sole canonical definition order. */
        template <typename Registry> Result<std::string> Encode(const Registry &registry, const ErrorCodeDescriptor &invalid) {
            const auto failure = [&] {
                return Result<std::string>::Failure(
                    AuthoringError(invalid, Source, "", "Encode an initialized bounded definition snapshot."));
            };
            if (registry.StableIdProjectId().empty() || registry.Definitions().size() > 4096)
                return failure();
            Json root{{"schemaVersion", 1},
                      {"projectId", registry.StableIdProjectId()},
                      {"stableIdRegistryFingerprint", FormatSha256(registry.StableIdRegistryFingerprint())},
                      {"definitions", Json::array()}};
            for (const auto &value : registry.Definitions())
                root["definitions"].push_back(Entry(value));
            auto bytes = root.dump(2) + "\n";
            if (bytes.size() > MaximumDocumentBytes)
                return failure();
            return Result<std::string>::Success(std::move(bytes));
        }
    }  // namespace

    /** @copydoc DecodePlatformAchievementDefinitions */
    Result<AchievementDefinitionRegistry> DecodePlatformAchievementDefinitions(const std::string_view source,
                                                                               const PlatformStableIdRegistry &stableIds,
                                                                               const AchievementDefinitionRegistryLimits &limits) {
        return Decode<AchievementDefinitionRegistryCandidate>(source, stableIds, AchievementDefinitionErrors::InvalidDefinition,
                                                              AchievementDefinitionErrors::StaleIdentityRegistry, Achievement,
                                                              [&](const auto &candidate) {
            return BuildAchievementDefinitionRegistry(stableIds, candidate, limits);
        });
    }

    /** @copydoc DecodePlatformStatDefinitions */
    Result<StatDefinitionRegistry> DecodePlatformStatDefinitions(const std::string_view source, const PlatformStableIdRegistry &stableIds,
                                                                 const PlatformDefinitionRegistryLimits &limits) {
        return Decode<StatDefinitionRegistryCandidate>(source, stableIds, PlatformDefinitionErrors::InvalidDefinition,
                                                       PlatformDefinitionErrors::StaleIdentityRegistry, Stat, [&](const auto &candidate) {
            return BuildStatDefinitionRegistry(stableIds, candidate, limits);
        });
    }

    /** @copydoc DecodePlatformLeaderboardDefinitions */
    Result<LeaderboardDefinitionRegistry> DecodePlatformLeaderboardDefinitions(const std::string_view source,
                                                                               const PlatformStableIdRegistry &stableIds,
                                                                               const StatDefinitionRegistry &stats,
                                                                               const PlatformDefinitionRegistryLimits &limits) {
        return Decode<LeaderboardDefinitionRegistryCandidate>(source, stableIds, PlatformDefinitionErrors::InvalidDefinition,
                                                              PlatformDefinitionErrors::StaleIdentityRegistry, Leaderboard,
                                                              [&](const auto &candidate) {
            return BuildLeaderboardDefinitionRegistry(stableIds, stats, candidate, limits);
        });
    }

    /** @copydoc DecodePlatformPresenceDefinitions */
    Result<PresenceDefinitionRegistry> DecodePlatformPresenceDefinitions(const std::string_view source,
                                                                         const PlatformStableIdRegistry &stableIds,
                                                                         const PlatformDefinitionRegistryLimits &limits) {
        return Decode<PresenceDefinitionRegistryCandidate>(source, stableIds, PlatformDefinitionErrors::InvalidDefinition,
                                                           PlatformDefinitionErrors::StaleIdentityRegistry, Presence,
                                                           [&](const auto &candidate) {
            return BuildPresenceDefinitionRegistry(stableIds, candidate, limits);
        });
    }

    /** @copydoc EncodePlatformAchievementDefinitions */
    Result<std::string> EncodePlatformAchievementDefinitions(const AchievementDefinitionRegistry &registry) {
        return Encode(registry, AchievementDefinitionErrors::InvalidDefinition);
    }

    /** @copydoc EncodePlatformStatDefinitions */
    Result<std::string> EncodePlatformStatDefinitions(const StatDefinitionRegistry &registry) {
        return Encode(registry, PlatformDefinitionErrors::InvalidDefinition);
    }

    /** @copydoc EncodePlatformLeaderboardDefinitions */
    Result<std::string> EncodePlatformLeaderboardDefinitions(const LeaderboardDefinitionRegistry &registry) {
        return Encode(registry, PlatformDefinitionErrors::InvalidDefinition);
    }

    /** @copydoc EncodePlatformPresenceDefinitions */
    Result<std::string> EncodePlatformPresenceDefinitions(const PresenceDefinitionRegistry &registry) {
        return Encode(registry, PlatformDefinitionErrors::InvalidDefinition);
    }
}  // namespace Horo::Application
