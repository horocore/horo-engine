#include "../../0.3.0/ProjectMigration.h"
#include "../ProjectMigration.h"
#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
#include "Horo/Application/ProjectMigrationErrors.h"

#include <algorithm>
#include <array>

namespace Horo::ProjectMigrations::R0_4_0 {
    namespace {
        using namespace Application;
        using namespace PlatformServices;
        constexpr std::array Paths{PlatformServicesDocumentPaths::Ledger, PlatformServicesDocumentPaths::Achievements,
                                   PlatformServicesDocumentPaths::Stats, PlatformServicesDocumentPaths::Leaderboards,
                                   PlatformServicesDocumentPaths::Presence};
        using Documents = std::array<std::string, Paths.size()>;

        /** @brief Retains one canonical root capture before candidate mutation. */
        struct RootCapture {
            MigrationDocumentHandle handle;
            ProjectSourceDocument source;
            std::string text;
        };

        /** @brief Reports an adoption precondition without echoing source values. */
        Error Invalid(const char *reason) {
            return MakeError(ProjectErrors::MigrationStageFailed, reason);
        }

        /** @brief Reads the existing strict project parser's sole authoritative root. */
        Result<RootCapture> ReadRoot(const ProjectMigrationContext &context) {
            const auto roots = context.ListDocuments(MigrationDocumentQuery::Kind(MigrationDocumentKind::ProjectMetadata));
            if (roots.size() != 1 || roots.front().path != ".horo/project.json")
                return Result<RootCapture>::Failure(Invalid("Exactly one canonical project root is required for identity adoption."));
            auto document = context.ReadDocument(roots.front().handle);
            if (document.HasError())
                return Result<RootCapture>::Failure(std::move(document).ErrorValue());
            const auto bytes = document.Value().bytes;
            const std::string_view text{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            auto source = DecodeProjectSourceDocument(text);
            if (source.HasError())
                return Result<RootCapture>::Failure(std::move(source).ErrorValue());
            return Result<RootCapture>::Success({roots.front().handle, std::move(source).Value(), std::string{text}});
        }

        /** @brief Captures complete bounded source documents without normalizing their committed bytes. */
        Result<Documents> ReadSources(const ProjectMigrationContext &context) {
            Documents result;
            for (const auto &entry : context.ListDocuments(MigrationDocumentQuery::Any())) {
                const auto path = std::ranges::find(Paths, entry.path);
                if (path == Paths.end())
                    continue;
                auto document = context.ReadDocument(entry.handle);
                if (document.HasError())
                    return Result<Documents>::Failure(std::move(document).ErrorValue());
                const auto bytes = document.Value().bytes;
                if (bytes.empty() || bytes.size() > PlatformStableIdRegistryMaximumDocumentBytes)
                    return Result<Documents>::Failure(Invalid("A Platform Services source document is empty or exceeds 4 MiB."));
                result[static_cast<std::size_t>(path - Paths.begin())] =
                    std::string{reinterpret_cast<const char *>(bytes.data()), bytes.size()};
            }
            return Result<Documents>::Success(std::move(result));
        }

        /** @brief Admits the frozen source ceiling through the existing typed builders, with no provider fallback. */
        Result<void> ValidateSources(const ProjectSourceDocument &root, const Documents &documents) {
            auto salt = ResolveProjectPlatformServicesSalt(root);
            if (salt.HasError())
                return Result<void>::Failure(std::move(salt).ErrorValue());
            auto ids = DecodePlatformServicesLedger(documents[0], root.metadata.projectId, salt.Value());
            if (ids.HasError())
                return Result<void>::Failure(std::move(ids).ErrorValue());
            for (const auto profile : {PlatformServicesHostProfile::InteractiveDevelopment, PlatformServicesHostProfile::HeadlessServer,
                                       PlatformServicesHostProfile::Cook, PlatformServicesHostProfile::Certification}) {
                auto policy = DecodeProjectPlatformServicesConfigurationCandidate(root, profile);
                if (policy.HasError())
                    return Result<void>::Failure(std::move(policy).ErrorValue());
            }
            const PlatformDefinitionRegistryLimits limits{.maximumDefinitions = 4096,
                                                          .maximumLocalizationKeyBytes = 512,
                                                          .maximumPresenceDetailBytes = 4096};
            auto achievements = DecodePlatformAchievementDefinitions(documents[1], ids.Value(),
                                                                     {.maximumDefinitions = 4096, .maximumLocalizationKeyBytes = 512});
            if (achievements.HasError())
                return Result<void>::Failure(std::move(achievements).ErrorValue());
            auto stats = DecodePlatformStatDefinitions(documents[2], ids.Value(), limits);
            if (stats.HasError())
                return Result<void>::Failure(std::move(stats).ErrorValue());
            auto leaderboards = DecodePlatformLeaderboardDefinitions(documents[3], ids.Value(), stats.Value(), limits);
            if (leaderboards.HasError())
                return Result<void>::Failure(std::move(leaderboards).ErrorValue());
            auto presence = DecodePlatformPresenceDefinitions(documents[4], ids.Value(), limits);
            if (presence.HasError())
                return Result<void>::Failure(std::move(presence).ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Generates a single root salt and stages the complete first generation without publishing anything. */
        Result<void> Initialize(ProjectMigrationContext &context, const RootCapture &root, const CancellationToken &cancellation) {
            PlatformServicesIdSalt salt;
            if (auto entropy = context.FillSecureRandom(salt.bytes); entropy.HasError())
                return entropy;
            auto prepared = PreparePlatformServicesInitialDocuments(root.text, salt);
            if (prepared.HasError())
                return Result<void>::Failure(std::move(prepared).ErrorValue());
            const auto &initial = prepared.Value();
            const std::array<std::string_view, 5> documents{initial.ledgerSource, initial.achievementSource, initial.statSource,
                                                            initial.leaderboardSource, initial.presenceSource};
            for (std::size_t index = 0; index < Paths.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ProjectErrors::MigrationCancelled));
                const auto bytes = std::as_bytes(std::span{documents[index].data(), documents[index].size()});
                if (auto added = context.AddDocument(std::string{Paths[index]}, MigrationDocumentKind::ProjectSettings,
                                                     {bytes.begin(), bytes.end()});
                    added.HasError())
                    return added;
            }
            const auto bytes = std::as_bytes(std::span{initial.projectSource.data(), initial.projectSource.size()});
            return context.ReplaceDocument(root.handle, {bytes.begin(), bytes.end()});
        }

        /** @brief Recognizes portable ASCII spelling variants before allocating a new namespace. */
        bool IsPlatformNamespacePath(const std::string_view path) {
            std::string folded{path};
            std::ranges::transform(folded, folded.begin(), [](const unsigned char value) {
                return static_cast<char>(value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value);
            });
            return folded.starts_with(".horo/platform_services.");
        }

        /** @brief Adopts only a truly absent platform namespace; partial or malformed identity never triggers repair. */
        class PlatformIdentityStage final : public IProjectMigrationStage {
        public:
            MigrationStageDescriptor Describe() const override {
                return {.id = {"adopt_project_platform_identity"},
                        .readFamilies = {"project.metadata", "project.settings.platform_services"},
                        .writeFamilies = {"project.metadata", "project.settings.platform_services"}};
            }

            Result<void> Execute(ProjectMigrationContext &context, const CancellationToken &cancellation) const override {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(ProjectErrors::MigrationCancelled));
                auto root = ReadRoot(context);
                if (root.HasError())
                    return Result<void>::Failure(std::move(root).ErrorValue());
                auto documents = ReadSources(context);
                if (documents.HasError())
                    return Result<void>::Failure(std::move(documents).ErrorValue());
                const auto count = static_cast<std::size_t>(std::ranges::count_if(documents.Value(), [](const auto &text) {
                    return !text.empty();
                }));
                const bool hasSalt = root.Value().source.platformServicesIdSaltSource.has_value();
                const bool hasPolicy = root.Value().source.platformServicesSettingsSource.has_value();
                if (count == 0 && !hasSalt && !hasPolicy) {
                    if (CompareHoroVersions(root.Value().source.metadata.horoVersion.value, ParseHoroVersion("0.4.0").Value()) !=
                        std::strong_ordering::less)
                        return Result<void>::Failure(
                            Invalid("A schema requiring platform identity cannot adopt a replacement namespace when it is missing."));
                    for (const auto &entry : context.ListDocuments(MigrationDocumentQuery::Any()))
                        if (IsPlatformNamespacePath(entry.path))
                            return Result<void>::Failure(Invalid(
                                "Unrecognized existing platform authoring requires explicit disposition before identity adoption."));
                    return Initialize(context, root.Value(), cancellation);
                }
                if (count != Paths.size() || !hasSalt || !hasPolicy)
                    return Result<void>::Failure(Invalid(
                        "Partial Platform Services identity requires reviewed recovery; migration never regenerates or repairs it."));
                return ValidateSources(root.Value().source, documents.Value());
            }
        };

        /** @brief Verifies inherited semantics and all platform sources before checking an optional final marker. */
        class PlatformAuthoringValidator final : public IProjectMigrationValidator {
        public:
            explicit PlatformAuthoringValidator(bool marker) : marker_(marker) {}

            MigrationStageDescriptor Describe() const override {
                return {.id = {marker_ ? "validate_0_4_0_target_contract" : "validate_0_4_0_platform_authoring"},
                        .readFamilies = {"project.metadata", "project.settings.platform_services", "project.settings.surfaces",
                                         "project.settings.network", "navigation.definition", "project.migration_history"}};
            }

            Result<void> Validate(const ProjectMigrationContext &context, const CancellationToken &cancellation) const override {
                if (auto inherited = R0_3_0::BuildSurfaceCatalogValidator()->Validate(context, cancellation); inherited.HasError())
                    return inherited;
                auto root = ReadRoot(context);
                if (root.HasError())
                    return Result<void>::Failure(std::move(root).ErrorValue());
                auto documents = ReadSources(context);
                if (documents.HasError())
                    return Result<void>::Failure(std::move(documents).ErrorValue());
                if (auto valid = ValidateSources(root.Value().source, documents.Value()); valid.HasError())
                    return valid;
                const auto &metadata = root.Value().source.metadata;
                if (marker_ && (metadata.horoVersion.value != ParseHoroVersion("0.4.0").Value() ||
                                metadata.persistentContract != ParsePersistentContractHash(TargetContract).Value() ||
                                !metadata.migrationHistoryHead.has_value()))
                    return Result<void>::Failure(Invalid("The final platform source contract marker or migration history is invalid."));
                return Result<void>::Success();
            }

        private:
            const bool marker_;
        };
    }  // namespace

    /** @copydoc BuildPlatformIdentityStage */
    std::shared_ptr<const Application::IProjectMigrationStage> BuildPlatformIdentityStage() {
        return std::make_shared<PlatformIdentityStage>();
    }

    /** @copydoc BuildPlatformAuthoringValidator */
    std::shared_ptr<const Application::IProjectMigrationValidator> BuildPlatformAuthoringValidator() {
        return std::make_shared<PlatformAuthoringValidator>(false);
    }

    /** @copydoc BuildTargetValidator */
    std::shared_ptr<const Application::IProjectMigrationValidator> BuildTargetValidator() {
        return std::make_shared<PlatformAuthoringValidator>(true);
    }
}  // namespace Horo::ProjectMigrations::R0_4_0
