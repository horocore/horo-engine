#include "NavigationBakeDiagnosticCodec.h"

#include "Horo/Foundation/Utf8.h"

#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

namespace Horo::Application::NavigationBakeDetail {
    namespace {
        using Json = nlohmann::json;

        struct Description {
            std::string_view code;
            DiagnosticSeverity severity;
            std::string_view fix;
        };

        constexpr std::array Descriptions{
            Description{"navigation.bake.queued", DiagnosticSeverity::Note, ""},
            Description{"navigation.bake.progress", DiagnosticSeverity::Note, ""},
            Description{"navigation.bake.tile_failed", DiagnosticSeverity::Error,
                        "Inspect the contributing geometry and agent clearance; correct the source and bake again."},
            Description{"navigation.bake.stage_failed", DiagnosticSeverity::Error,
                        "Inspect the retained cause code and resource limits; correct the input or publication failure and retry."},
            Description{"navigation.bake.succeeded", DiagnosticSeverity::Note, ""},
            Description{"navigation.bake.cancelled", DiagnosticSeverity::Warning, "Submit a new bake when the project is ready."},
            Description{"navigation.bake.superseded", DiagnosticSeverity::Note,
                        "Observe the successor bake for the current source capture."},
            Description{"navigation.bake.suppressed", DiagnosticSeverity::Warning,
                        "Increase the diagnostic detail limit for a subsequent bake if more detail is needed."},
        };
        static_assert(Descriptions.size() == static_cast<std::size_t>(NavigationBakeDiagnosticEvent::Count));

        /** @brief Reads an exact nonnegative integer without narrowing hostile JSON values. */
        [[nodiscard]] std::uint64_t Unsigned(const Json &input, const char *key) {
            const auto &value = input.at(key);
            if (!value.is_number_unsigned())
                throw Json::type_error::create(302, "unsigned identity required", &input);
            return value.get<std::uint64_t>();
        }

        /** @brief Parses one nonzero domain identity before constructing retained evidence. */
        template <typename T> [[nodiscard]] T Identity(const Json &input, const char *key) {
            const auto parsed = T::Create(Unsigned(input, key));
            if (parsed.HasError())
                throw Json::type_error::create(302, "invalid identity", &input);
            return parsed.Value();
        }

        /** @brief Checks both JSON integer representations before narrowing a signed tile coordinate. */
        [[nodiscard]] std::int32_t Coordinate(const Json &input, const char *key) {
            const auto &value = input.at(key);
            if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT32_MAX))
                throw Json::type_error::create(302, "invalid tile coordinate", &input);
            const auto coordinate = value.get<std::int64_t>();
            if (coordinate < INT32_MIN || coordinate > INT32_MAX)
                throw Json::type_error::create(302, "invalid tile coordinate", &input);
            return static_cast<std::int32_t>(coordinate);
        }

        /** @brief Decodes source provenance without accepting runtime slots as authored destinations. */
        [[nodiscard]] NavigationDiagnosticSource Source(const Json &input) {
            using namespace Navigation;
            const auto kind = Unsigned(input, "kind");
            const auto digest = ParseSha256(input.at("digest").get<std::string>());
            if (kind >= static_cast<std::uint64_t>(NavigationSourceProducerKind::Count) || digest.HasError())
                throw Json::type_error::create(302, "invalid source provenance", &input);
            NavigationDiagnosticSource source{.observation = {.kind = static_cast<NavigationSourceProducerKind>(kind),
                                                              .producer = Identity<NavigationSourceProducerId>(input, "producer"),
                                                              .contribution =
                                                                  Identity<NavigationSourceContributionId>(input, "contribution"),
                                                              .revision = Identity<NavigationSourceRevision>(input, "revision"),
                                                              .contentDigest = digest.Value()},
                                              .target = {.scene = {Unsigned(input, "scene")},
                                                         .object = {Unsigned(input, "object")},
                                                         .relativePath = input.at("path").get<std::string>()}};
            if (const auto asset = input.at("asset").get<std::string>(); !asset.empty()) {
                auto id = Assets::AssetId::Parse(asset);
                if (id.HasError() || !id.Value().IsValid())
                    throw Json::type_error::create(302, "invalid asset identity", &input);
                source.target.asset = id.Value();
            }
            if (source.target.relativePath.size() > 1024 || source.target.scene.IsValid() != source.target.object.IsValid())
                throw Json::type_error::create(302, "invalid source target", &input);
            return source;
        }

        /** @brief Validates fixed checkpoint text bounds independently of optional navigation context. */
        [[nodiscard]] bool ValidText(const NavigationBakeDiagnosticRecord &record) {
            return record.stage.size() <= 64 && record.message.size() <= 1024 && record.causeCode.size() <= 160 &&
                   IsValidUtf8ScalarSequence(record.stage) && IsValidUtf8ScalarSequence(record.message) &&
                   IsValidUtf8ScalarSequence(record.causeCode);
        }

        /** @brief Decodes bounded progress without narrowing non-finite or out-of-range values. */
        [[nodiscard]] float Progress(const Json &value) {
            const auto progress = value.get<float>();
            if (!std::isfinite(progress) || progress < 0 || progress > 1)
                throw Json::type_error::create(302, "invalid progress", &value);
            return progress;
        }

        /** @brief Decodes the optional tile/source/progress evidence with checked domain identities. */
        void ReadContext(const Json &value, NavigationBakeDiagnosticRecord &record) {
            if (value.contains("progress"))
                record.progress = Progress(value.at("progress"));
            if (value.contains("tile")) {
                const auto &tile = value.at("tile");
                const auto layer = Unsigned(tile, "layer");
                if (layer > UINT16_MAX)
                    throw Json::type_error::create(302, "invalid tile layer", &tile);
                record.tile = Navigation::NavigationBakeTileKey{.profile = Identity<Navigation::NavigationAgentProfileId>(tile, "profile"),
                                                                .surface = Identity<Navigation::SurfaceId>(tile, "surface"),
                                                                .tile = {.x = Coordinate(tile, "x"),
                                                                         .z = Coordinate(tile, "z"),
                                                                         .layer = static_cast<std::uint16_t>(layer)}};
            }
            if (value.contains("source"))
                record.source = Source(value.at("source"));
        }
    }  // namespace

    /** @copydoc DescribeDiagnostic */
    void DescribeDiagnostic(NavigationBakeDiagnosticRecord &record) {
        const auto &description = Descriptions[static_cast<std::size_t>(record.event)];
        record.code = DiagnosticCode{std::string{description.code}};
        record.severity = description.severity;
        record.suggestedFix = description.fix;
    }

    /** @copydoc EncodeDiagnostic */
    std::string EncodeDiagnostic(const NavigationBakeDiagnosticRecord &record, const NavigationBakeDiagnosticsConfig &config) {
        Json value{{"schema", 1},
                   {"project", config.project.Value()},
                   {"definition", config.definition.ToString()},
                   {"sequence", record.sequence},
                   {"operation", record.operation},
                   {"event", record.event},
                   {"stage", record.stage},
                   {"result", record.result},
                   {"message", record.message},
                   {"cause", record.causeCode},
                   {"suppressed", record.suppressedCount},
                   {"totalSuppressed", record.totalSuppressedRecords},
                   {"totalDropped", record.totalDroppedRecords}};
        if (record.progress.has_value())
            value["progress"] = *record.progress;
        if (record.tile) {
            const auto &tile = *record.tile;
            value["tile"] = {{"profile", tile.profile.Value()},
                             {"surface", tile.surface.Value()},
                             {"x", tile.tile.x},
                             {"z", tile.tile.z},
                             {"layer", tile.tile.layer}};
        }
        if (record.source) {
            const auto &source = *record.source;
            value["source"] = {{"kind", source.observation.kind},
                               {"producer", source.observation.producer.Value()},
                               {"contribution", source.observation.contribution.Value()},
                               {"revision", source.observation.revision.Value()},
                               {"digest", FormatSha256(source.observation.contentDigest)},
                               {"asset", source.target.asset.IsValid() ? source.target.asset.ToString() : ""},
                               {"scene", source.target.scene.value},
                               {"object", source.target.object.value},
                               {"path", source.target.relativePath}};
        }
        return value.dump();
    }

    /** @copydoc DecodeDiagnostic */
    std::optional<NavigationBakeDiagnosticRecord> DecodeDiagnostic(const std::string_view bytes,
                                                                   const NavigationBakeDiagnosticsConfig &config) {
        if (bytes.size() > 8192)
            return std::nullopt;
        try {
            const auto value = Json::parse(bytes);
            if (Unsigned(value, "schema") != 1 || Unsigned(value, "project") != config.project.Value() ||
                value.at("definition") != config.definition.ToString())
                return std::nullopt;
            const auto event = Unsigned(value, "event");
            const auto result = Unsigned(value, "result");
            if (event >= Descriptions.size() || result > static_cast<std::uint64_t>(BuildOutputResult::TimedOut))
                return std::nullopt;
            NavigationBakeDiagnosticRecord record{.sequence = Unsigned(value, "sequence"),
                                                  .operation = Unsigned(value, "operation"),
                                                  .event = static_cast<NavigationBakeDiagnosticEvent>(event),
                                                  .stage = value.at("stage").get<std::string>(),
                                                  .result = static_cast<BuildOutputResult>(result),
                                                  .message = value.at("message").get<std::string>(),
                                                  .causeCode = value.at("cause").get<std::string>(),
                                                  .suppressedCount = Unsigned(value, "suppressed"),
                                                  .totalSuppressedRecords = Unsigned(value, "totalSuppressed"),
                                                  .totalDroppedRecords = Unsigned(value, "totalDropped"),
                                                  .recovered = true};
            if (record.sequence == 0 || record.operation == 0 || !ValidText(record))
                return std::nullopt;
            ReadContext(value, record);
            DescribeDiagnostic(record);
            return record;
        } catch (const Json::exception &) {
            return std::nullopt;
        }
    }
}  // namespace Horo::Application::NavigationBakeDetail
