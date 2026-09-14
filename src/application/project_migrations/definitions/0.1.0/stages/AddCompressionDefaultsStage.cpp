#include "../ProjectMigration.h"
#include "src/application/project/ProjectErrors.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace Horo::ProjectMigrations::R0_1_0 {
    namespace {
        using Json = nlohmann::json;

        [[nodiscard]] Error InvalidSettings(std::string message) {
            return MakeError(Application::ProjectErrors::MigrationStageFailed, std::move(message));
        }

        class CompressionDefaultsStage final : public Application::IProjectMigrationDocumentStage {
        public:
            [[nodiscard]] Application::MigrationStageDescriptor Describe() const override {
                return {.id = {"add_compression_defaults"},
                        .readFamilies = {"project.metadata"},
                        .writeFamilies = {"project.settings.compression"},
                        .estimatedWeight = 1};
            }

            [[nodiscard]] Result<Application::MigrationDocumentChange> Execute(const Application::ProjectDocumentView &source,
                                                                               const Application::MigrationStageContext &,
                                                                               const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Application::MigrationDocumentChange>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                Json root;
                try {
                    root = Json::parse(reinterpret_cast<const char *>(source.bytes.data()),
                                       reinterpret_cast<const char *>(source.bytes.data() + source.bytes.size()));
                } catch (...) {
                    return Result<Application::MigrationDocumentChange>::Failure(InvalidSettings("Project metadata is not valid JSON."));
                }
                if (!root.is_object() || !root.contains("settings") || !root["settings"].is_object())
                    return Result<Application::MigrationDocumentChange>::Failure(
                        InvalidSettings("Project metadata settings must be an object."));
                Json &settings = root["settings"];
                if (!settings.contains("assetCompression"))
                    settings["assetCompression"] = "lz4";
                if (!settings.contains("textureCompression"))
                    settings["textureCompression"] = "bc7";
                const std::string serialized = root.dump(2) + "\n";
                return Result<Application::MigrationDocumentChange>::Success({
                    .document = source.handle,
                    .replacement = SerializeDocumentBytes(serialized),
                    .changed = serialized.size() != source.bytes.size() ||
                               !std::equal(serialized.begin(), serialized.end(), reinterpret_cast<const char *>(source.bytes.data())),
                });
            }
        };
    }  // namespace

    /** @copydoc BuildCompressionDefaultsStage */
    std::shared_ptr<const Application::IProjectMigrationDocumentStage> BuildCompressionDefaultsStage() {
        return std::make_shared<CompressionDefaultsStage>();
    }
}  // namespace Horo::ProjectMigrations::R0_1_0
