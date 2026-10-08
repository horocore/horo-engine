#include "ProjectMigration.h"

#include "src/application/project/ProjectErrors.h"

namespace Horo::ProjectMigrations::R0_1_0 {
    namespace {
        /** @brief Owns the ordered read-only prefab and compression checks behind one terminal definition barrier. */
        class DefinitionPostconditionValidator final : public Application::IProjectMigrationValidator {
        public:
            [[nodiscard]] Application::MigrationStageDescriptor Describe() const override {
                return {.id = {"validate_project_adoption"},
                        .readFamilies = {"asset.sidecar", "prefab.source", "scene.prefab_reference", "project.settings.compression"},
                        .estimatedWeight = 2};
            }

            [[nodiscard]] Result<void> Validate(const Application::ProjectMigrationContext &context,
                                                const CancellationToken &cancellation) const override {
                if (auto valid = prefab_->Validate(context, cancellation); valid.HasError())
                    return valid;
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                return compression_->Validate(context, cancellation);
            }

        private:
            const std::shared_ptr<const Application::IProjectMigrationValidator> prefab_{BuildPrefabAdoptionValidator()};
            const std::shared_ptr<const Application::IProjectMigrationValidator> compression_{BuildCompressionPostconditionValidator()};
        };
    }  // namespace

    /** @copydoc SerializeDocumentBytes */
    std::vector<std::byte> SerializeDocumentBytes(const std::string_view text) {
        const auto *first = reinterpret_cast<const std::byte *>(text.data());
        return {first, first + text.size()};
    }

    /** @copydoc BuildProjectMigration */
    Result<Application::ProjectMigrationDefinition> BuildProjectMigration() {
        using namespace Application;
        auto from = ParseHoroVersion("0.0.1");
        auto to = ParseHoroVersion("0.1.0");
        auto sourceContract = ParsePersistentContractHash("sha256:5ef87e96e24c0a3a5e44f4dee182dbd3bfb5402e08e07aaf3d64d4a3ff24ae6d");
        auto targetContract = ParsePersistentContractHash("sha256:997e790fc23515b362847c755006156aa35353ce7f2624518acf7ed1214ddb03");
        if (from.HasError() || to.HasError() || sourceContract.HasError() || targetContract.HasError())
            return Result<ProjectMigrationDefinition>::Failure(from.HasError()             ? from.ErrorValue()
                                                               : to.HasError()             ? to.ErrorValue()
                                                               : sourceContract.HasError() ? sourceContract.ErrorValue()
                                                                                           : targetContract.ErrorValue());

        auto builder = ProjectMigrationPipelineBuilder::Begin({"core.project_settings.compression_defaults"});
        static_cast<void>(
            builder.AddForEach(MigrationDocumentQuery::Kind(MigrationDocumentKind::ProjectMetadata), BuildCompressionDefaultsStage()));
        static_cast<void>(builder.AddThen(BuildPrefabSourceAdoptionStage()));
        static_cast<void>(builder.AddThen(BuildPrefabReferenceAdoptionStage()));
        static_cast<void>(builder.AddValidator(std::make_shared<DefinitionPostconditionValidator>()));
        auto pipeline = std::move(builder).Build();
        if (pipeline.HasError())
            return Result<ProjectMigrationDefinition>::Failure(pipeline.ErrorValue());

        return Result<ProjectMigrationDefinition>::Success(ProjectMigrationDefinition{
            .id = {"core.project_settings.compression_defaults"},
            .kind = ProjectMigrationDefinitionKind::Sequential,
            .from = ContractBaselineVersion{from.Value()},
            .to = ContractBaselineVersion{to.Value()},
            .sourceContract = sourceContract.Value(),
            .targetContract = targetContract.Value(),
            .pipeline = std::move(pipeline).Value(),
        });
    }
}  // namespace Horo::ProjectMigrations::R0_1_0
