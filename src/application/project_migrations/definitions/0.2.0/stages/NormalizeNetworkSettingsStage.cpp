#include "../ProjectMigration.h"
#include "Horo/Network/NetworkProjectSettings.h"
#include "src/application/project/ProjectErrors.h"

#include <algorithm>
#include <array>
#include <functional>
#include <nlohmann/json.hpp>
#include <set>

namespace Horo::ProjectMigrations::R0_2_0 {
    namespace {
        using Json = nlohmann::json;

        /** @brief Bounds root metadata parsing and rejects duplicate keys before extracting nested policy. */
        struct RootGuard final {
            std::vector<std::set<std::string>> keys;
            bool rejected{};

            bool operator()(const int depth, const Json::parse_event_t event, const Json &value) {
                if (depth < 0 || depth > 32) {
                    rejected = true;
                    return false;
                }
                const auto index = static_cast<std::size_t>(event == Json::parse_event_t::key ? std::max(0, depth - 1) : depth);
                if (keys.size() <= index)
                    keys.resize(index + 1);
                if (event == Json::parse_event_t::object_start)
                    keys[index].clear();
                if (event == Json::parse_event_t::key && !keys[index].insert(value.get<std::string>()).second)
                    rejected = true;
                return !rejected;
            }
        };

        /** @brief Parses one constrained candidate without raw-root access or writes. */
        Result<Json> ReadRoot(const Application::ProjectDocumentView &source) {
            if (source.path != ".horo/project.json" || source.bytes.empty() || source.bytes.size() > 1024U * 1024U)
                return Result<Json>::Failure(
                    MakeError(Application::ProjectErrors::MigrationStageFailed, "Invalid project metadata inventory or size."));
            RootGuard guard;
            const auto text = std::string_view{reinterpret_cast<const char *>(source.bytes.data()), source.bytes.size()};
            auto root = Json::parse(text, std::ref(guard), false);
            if (guard.rejected || root.is_discarded() || !root.is_object() || !root.contains("settings") || !root["settings"].is_object())
                return Result<Json>::Failure(
                    MakeError(Application::ProjectErrors::MigrationStageFailed, "Malformed or duplicate project settings."));
            return Result<Json>::Success(std::move(root));
        }

        /** @brief Calls the sole portable Network codec; absent policy never manufactures defaults or completeness. */
        Result<void> NormalizeNetwork(Json &settings) {
            if (!settings.contains("network"))
                return Result<void>::Success();
            auto parsed = Network::ParseNetworkProjectSettings(settings["network"].dump());
            if (parsed.HasError())
                return Result<void>::Failure(parsed.ErrorValue());
            if (parsed.Value().contractVersion != 3)
                return Result<void>::Failure(MakeError(Application::ProjectErrors::MigrationStageFailed,
                                                       "Project contract 0.2.0 requires the portable Network v3 codec."));
            auto validated = Network::NetworkProjectSettings::Create(parsed.Value());
            if (validated.HasError())
                return Result<void>::Failure(validated.ErrorValue());
            settings["network"] = Json::parse(Network::SerializeNetworkProjectSettings(validated.Value()));
            return Result<void>::Success();
        }

        /** @brief Retains the previous release's compression postconditions rather than weakening target admission. */
        bool ValidCompression(const Json &settings) {
            constexpr std::array<std::string_view, 3> asset{"lz4", "none", "zstd"};
            constexpr std::array<std::string_view, 4> texture{"bc7", "bc5", "astc", "none"};
            const auto matches = [&settings](const char *key, const auto &allowed) {
                return settings.contains(key) && settings[key].is_string() &&
                       std::ranges::find(allowed, settings[key].get_ref<const std::string &>()) != allowed.end();
            };
            return matches("assetCompression", asset) && matches("textureCompression", texture) && settings.contains("renderBackend") &&
                   settings["renderBackend"].is_string() && !settings["renderBackend"].get_ref<const std::string &>().empty();
        }

        /** @brief Checks exact audited target marker types before accessing their values. */
        bool ValidMarker(const Json &root) {
            constexpr std::array fields{"horoVersion", "persistentContract", "migrationHistoryHead"};
            if (!std::ranges::all_of(fields, [&root](const char *key) {
                return root.contains(key) && root[key].is_string();
            }))
                return false;
            return root["horoVersion"] == "0.2.0" && root["persistentContract"].get_ref<const std::string &>() == TargetContract;
        }

        /** @brief Target data is already normalized; parsing must not silently migrate an older codec envelope. */
        bool IsCurrentNetwork(const Json &settings) {
            if (!settings.contains("network"))
                return true;
            const auto &network = settings["network"];
            return network.is_object() && network.contains("contractVersion") && network["contractVersion"].is_number_integer() &&
                   network["contractVersion"] == 3;
        }

        class NetworkSettingsStage final : public Application::IProjectMigrationDocumentStage {
        public:
            Application::MigrationStageDescriptor Describe() const override {
                return {.id = {"normalize_portable_network_v3"},
                        .readFamilies = {"project.metadata", "project.settings.network"},
                        .writeFamilies = {"project.settings.network"},
                        .estimatedWeight = 1};
            }

            Result<Application::MigrationDocumentChange> Execute(const Application::ProjectDocumentView &source,
                                                                 const Application::MigrationStageContext &,
                                                                 const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Application::MigrationDocumentChange>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                auto root = ReadRoot(source);
                if (root.HasError())
                    return Result<Application::MigrationDocumentChange>::Failure(root.ErrorValue());
                if (!root.Value()["settings"].contains("network"))
                    return Result<Application::MigrationDocumentChange>::Success({.document = source.handle, .changed = false});
                auto candidate = std::move(root).Value();
                if (const auto valid = NormalizeNetwork(candidate["settings"]); valid.HasError())
                    return Result<Application::MigrationDocumentChange>::Failure(valid.ErrorValue());
                const auto text = candidate.dump(2) + "\n";
                const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
                return Result<Application::MigrationDocumentChange>::Success({.document = source.handle,
                                                                              .replacement = {bytes.begin(), bytes.end()},
                                                                              .changed = !std::ranges::equal(bytes, source.bytes)});
            }
        };
    }  // namespace

    /** @copydoc BuildNetworkSettingsStage */
    std::shared_ptr<const Application::IProjectMigrationDocumentStage> BuildNetworkSettingsStage() {
        return std::make_shared<NetworkSettingsStage>();
    }

    /** @copydoc ValidateProjectAuthoring */
    Result<void> ValidateProjectAuthoring(const Application::ProjectDocumentView &source, const bool targetMarker) {
        auto parsed = ReadRoot(source);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        const auto &root = parsed.Value();
        constexpr std::array required{"projectId", "name", "projectVersion", "createdAt"};
        if (std::ranges::any_of(required, [&root](const char *key) {
            return !root.contains(key) || !root[key].is_string() || root[key].get_ref<const std::string &>().empty();
        }))
            return Result<void>::Failure(
                MakeError(Application::ProjectErrors::MigrationStageFailed, "Required project identity metadata is invalid."));
        auto settings = root["settings"];
        if (!ValidCompression(settings))
            return Result<void>::Failure(
                MakeError(Application::ProjectErrors::MigrationStageFailed, "Required compression/render settings are invalid."));
        if (targetMarker && !ValidMarker(root))
            return Result<void>::Failure(
                MakeError(Application::ProjectErrors::MigrationStageFailed, "Target marker or audited history binding is invalid."));
        if (targetMarker && !IsCurrentNetwork(settings))
            return Result<void>::Failure(
                MakeError(Application::ProjectErrors::MigrationStageFailed, "Target Network settings were not normalized to portable v3."));
        return NormalizeNetwork(settings);
    }
}  // namespace Horo::ProjectMigrations::R0_2_0
