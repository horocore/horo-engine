#include "../ProjectMigration.h"
#include "Horo/Assets/AssetRegistry.h"
#include "src/application/project/ProjectErrors.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace Horo::ProjectMigrations::R0_2_0 {
    namespace {
        class AuthoringValidator final : public Application::IProjectMigrationValidator {
        public:
            explicit AuthoringValidator(const bool targetMarker) noexcept : targetMarker_(targetMarker) {}

            Application::MigrationStageDescriptor Describe() const override {
                return {.id = {targetMarker_ ? "validate_0_2_0_target_contract" : "validate_0_2_0_authoring"},
                        .readFamilies = {"project.metadata", "project.settings.network", "navigation.definition",
                                         "project.migration_history"},
                        .estimatedWeight = 1};
            }

            Result<void> Validate(const Application::ProjectMigrationContext &context,
                                  const CancellationToken &cancellation) const override {
                const auto roots =
                    context.ListDocuments(Application::MigrationDocumentQuery::Kind(Application::MigrationDocumentKind::ProjectMetadata));
                if (roots.size() != 1)
                    return Result<void>::Failure(
                        MakeError(Application::ProjectErrors::MigrationStageFailed, "Exactly one project metadata document is required."));
                auto definitions = NavigationDefinitionPaths(context, cancellation);
                if (definitions.HasError())
                    return Result<void>::Failure(definitions.ErrorValue());
                for (const auto &entry : context.ListDocuments(Application::MigrationDocumentQuery::Any())) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                    auto document = context.ReadDocument(entry.handle);
                    if (document.HasError())
                        return Result<void>::Failure(document.ErrorValue());
                    const auto valid =
                        entry.kind == Application::MigrationDocumentKind::ProjectMetadata
                            ? ValidateProjectAuthoring(document.Value(), targetMarker_)
                            : ValidateNavigationAuthoring(document.Value(),
                                                          std::ranges::find(definitions.Value(), entry.path) != definitions.Value().end());
                    if (valid.HasError())
                        return valid;
                }
                return Result<void>::Success();
            }

        private:
            const bool targetMarker_;
        };
    }  // namespace

    /** @copydoc NavigationDefinitionPaths */
    Result<std::vector<std::string>> NavigationDefinitionPaths(const Application::ProjectMigrationContext &context,
                                                               const CancellationToken &cancellation) {
        const auto entries = context.ListDocuments(Application::MigrationDocumentQuery::Any());
        std::vector<std::string> paths;
        std::set<Assets::AssetId> identities;
        for (const auto &entry : entries) {
            if (cancellation.IsCancellationRequested())
                return Result<std::vector<std::string>>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
            std::string folded = entry.path;
            std::ranges::transform(folded, folded.begin(), [](const unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (!folded.ends_with(".horoasset.horo"))
                continue;
            auto document = context.ReadDocument(entry.handle);
            if (document.HasError())
                return Result<std::vector<std::string>>::Failure(document.ErrorValue());
            const auto bytes = document.Value().bytes;
            const auto sourcePath = entry.path.substr(0, entry.path.size() - std::string_view{".horo"}.size());
            auto identity = Assets::DecodeAssetIdentitySidecar(sourcePath, {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
            if (identity.HasError())
                return Result<std::vector<std::string>>::Failure(identity.ErrorValue());
            if (identity.Value().type.Value() != "core.navmesh.definition")
                continue;
            if (!identities.insert(identity.Value().id).second ||
                std::ranges::find(entries, sourcePath, &Application::MigrationDocumentEntry::path) == entries.end())
                return Result<std::vector<std::string>>::Failure(MakeError(Application::ProjectErrors::MigrationStageFailed,
                                                                           "Navigation identity is duplicated or its source is missing."));
            paths.push_back(sourcePath);
        }
        return Result<std::vector<std::string>>::Success(std::move(paths));
    }

    /** @copydoc BuildAuthoringValidator */
    std::shared_ptr<const Application::IProjectMigrationValidator> BuildAuthoringValidator() {
        return std::make_shared<AuthoringValidator>(false);
    }

    /** @copydoc BuildTargetValidator */
    std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator() {
        return std::make_shared<AuthoringValidator>(true);
    }
}  // namespace Horo::ProjectMigrations::R0_2_0
