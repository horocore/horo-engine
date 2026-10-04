#include "../ProjectMigration.h"
#include "Horo/Navigation/NavigationDefinitionSerialization.h"
#include "src/application/project/ProjectErrors.h"

#include <algorithm>
#include <array>

namespace Horo::ProjectMigrations::R0_2_0 {
    namespace {
        /** @brief Recognizes only the existing Navigation envelope in generic authored asset sources. */
        bool IsNavigationSource(const Application::ProjectDocumentView &source) noexcept {
            constexpr std::array magic{std::byte{'H'}, std::byte{'N'}, std::byte{'A'}, std::byte{'V'}};
            return source.path.ends_with(".horoasset") && source.bytes.size() >= magic.size() &&
                   std::ranges::equal(source.bytes.first(magic.size()), magic);
        }

        /** @brief Admits exact production semantics and preserves unknown optional authoring inert. */
        Result<Navigation::NavigationSourceRecords> ReadNavigation(const Application::ProjectDocumentView &source,
                                                                   const bool declaredDefinition = false) {
            const std::array support{Navigation::NavigationDefinitionRecordSupport()};
            const Navigation::NavigationSourceLoadContext policy{.unknownPolicy =
                                                                     Navigation::NavigationUnknownRecordPolicy::PreserveOptionalInert,
                                                                 .supportedRecords = support};
            auto parsed = Navigation::DeserializeNavigationSourceRecords(source.bytes, policy);
            if (parsed.HasError())
                return parsed;
            std::size_t definitions{};
            for (const auto &record : parsed.Value().Records()) {
                if (record.type != support.front().type)
                    continue;
                auto definition = Navigation::DecodeNavigationDefinitionRecord(record);
                if (definition.HasError())
                    return Result<Navigation::NavigationSourceRecords>::Failure(definition.ErrorValue());
                ++definitions;
            }
            if (definitions > 1 || (declaredDefinition && definitions != 1))
                return Result<Navigation::NavigationSourceRecords>::Failure(
                    MakeError(Application::ProjectErrors::MigrationStageFailed,
                              "A declared definition asset must own exactly one Navigation definition record."));
            return parsed;
        }

        class NavigationSourceStage final : public Application::IProjectMigrationDocumentStage {
        public:
            Application::MigrationStageDescriptor Describe() const override {
                return {.id = {"adopt_navigation_definition_records"},
                        .readFamilies = {"navigation.definition"},
                        .writeFamilies = {"navigation.definition"},
                        .estimatedWeight = 1};
            }

            Result<Application::MigrationDocumentChange> Execute(const Application::ProjectDocumentView &source,
                                                                 const Application::MigrationStageContext &,
                                                                 const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<Application::MigrationDocumentChange>::Failure(MakeError(Application::ProjectErrors::MigrationCancelled));
                if (!IsNavigationSource(source))
                    return Result<Application::MigrationDocumentChange>::Success({.document = source.handle, .changed = false});
                auto parsed = ReadNavigation(source);
                if (parsed.HasError())
                    return Result<Application::MigrationDocumentChange>::Failure(parsed.ErrorValue());
                const std::array support{Navigation::NavigationDefinitionRecordSupport()};
                const Navigation::NavigationSourceLoadContext policy{.unknownPolicy =
                                                                         Navigation::NavigationUnknownRecordPolicy::PreserveOptionalInert,
                                                                     .supportedRecords = support};
                const auto records = parsed.Value().Records();
                auto authored = Navigation::NavigationSourceRecords::Create(Navigation::CurrentNavigationSourceSchemaVersion,
                                                                            {records.begin(), records.end()}, policy);
                if (authored.HasError())
                    return Result<Application::MigrationDocumentChange>::Failure(authored.ErrorValue());
                auto bytes = Navigation::SerializeNavigationSourceRecords(authored.Value());
                if (bytes.HasError())
                    return Result<Application::MigrationDocumentChange>::Failure(bytes.ErrorValue());
                const bool changed = !std::ranges::equal(bytes.Value(), source.bytes);
                return Result<Application::MigrationDocumentChange>::Success(
                    {.document = source.handle, .replacement = std::move(bytes).Value(), .changed = changed});
            }
        };
    }  // namespace

    /** @copydoc BuildNavigationSourceStage */
    std::shared_ptr<const Application::IProjectMigrationDocumentStage> BuildNavigationSourceStage() {
        return std::make_shared<NavigationSourceStage>();
    }

    /** @copydoc ValidateNavigationAuthoring */
    Result<void> ValidateNavigationAuthoring(const Application::ProjectDocumentView &source, const bool declaredDefinition) {
        if (!declaredDefinition && !IsNavigationSource(source))
            return Result<void>::Success();
        auto parsed = ReadNavigation(source, declaredDefinition);
        if (parsed.HasError())
            return Result<void>::Failure(parsed.ErrorValue());
        if (!parsed.Value().GeneratedPayloads().empty() || parsed.Value().HasQuarantinedGeneratedPayloads())
            return Result<void>::Failure(MakeError(Application::ProjectErrors::MigrationStageFailed,
                                                   "Derived Navigation payloads must be invalidated before target admission."));
        return Result<void>::Success();
    }
}  // namespace Horo::ProjectMigrations::R0_2_0
