#include "../../support/project/PlatformServicesAuthoringFixture.h"
#include "../../support/project/PlatformServicesMigrationFixture.h"
#include "Horo/Application/PlatformServicesProjectCook.h"
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/PlatformServices/PlatformProviderManifestCook.h"

#include <concepts>

namespace Horo::Tests::PlatformMigration {
    static_assert(!std::default_initializable<PlatformServicesProjectSourceSnapshot>);

    namespace {
        /** @brief Publishes explicit fixture inputs through the real ledger replacement and semantic builders. */
        void WriteSemanticSources(Project &project, const bool addAlias) {
            const auto root = DecodeProjectSourceDocument(project.Read(".horo/project.json"));
            REQUIRE(root.HasValue());
            const auto salt = ResolveProjectPlatformServicesSalt(root.Value());
            REQUIRE(salt.HasValue());
            const auto previous = DecodePlatformServicesLedger(project.Read(PlatformServicesDocumentPaths::Ledger),
                                                               root.Value().metadata.projectId, salt.Value());
            REQUIRE(previous.HasValue());
            PlatformStableIdRegistryCandidate candidate{.projectId = root.Value().metadata.projectId, .salt = salt.Value()};
            for (const auto kind : {PlatformServiceIdKind::Achievement, PlatformServiceIdKind::Stat, PlatformServiceIdKind::Leaderboard,
                                    PlatformServiceIdKind::PresenceStatus}) {
                candidate.entries.push_back(
                    {.kind = kind,
                     .canonicalKey = "active.key",
                     .storedId = DerivePlatformServiceStableId(salt.Value(), kind, "active.key").Value(),
                     .aliases = addAlias ? std::vector<std::string>{"active.compatibility"} : std::vector<std::string>{}});
                candidate.entries.push_back({.kind = kind,
                                             .canonicalKey = "retired.key",
                                             .storedId = DerivePlatformServiceStableId(salt.Value(), kind, "retired.key").Value(),
                                             .state = PlatformStableIdState::Tombstoned,
                                             .removalProvenance = "removed:fixture"});
            }
            const auto ids = BuildPlatformStableIdRegistryReplacement(previous.Value(), previous.Value().Fingerprint(), candidate);
            REQUIRE(ids.HasValue());
            const auto definitions = Tests::PlatformAuthoring::Define(ids.Value());
            project.Write(PlatformServicesDocumentPaths::Ledger, EncodePlatformServicesLedger(ids.Value()).Value());
            project.Write(PlatformServicesDocumentPaths::Achievements,
                          EncodePlatformAchievementDefinitions(definitions.achievements).Value());
            project.Write(PlatformServicesDocumentPaths::Stats, EncodePlatformStatDefinitions(definitions.stats).Value());
            project.Write(PlatformServicesDocumentPaths::Leaderboards,
                          EncodePlatformLeaderboardDefinitions(definitions.leaderboards).Value());
            project.Write(PlatformServicesDocumentPaths::Presence, EncodePlatformPresenceDefinitions(definitions.presence).Value());
        }

        /** @brief Cooks the actual captured immutable generation, not reconstructed fixture models. */
        PlatformProviderManifestCookOutput Cook(const PlatformServicesProjectSourceSnapshot &source) {
            auto cooked = CookPlatformProviderManifest({.stableIds = source.StableIds(),
                                                        .achievements = source.Achievements(),
                                                        .leaderboards = source.Leaderboards(),
                                                        .stats = source.Stats(),
                                                        .presence = source.Presence(),
                                                        .configuration = source.Configuration()});
            REQUIRE(cooked.HasValue());
            return std::move(cooked).Value();
        }

        /** @brief Publishes through genuine native staging, source/target fences and tracked atomic replacement. */
        Assets::AssetSourceCapture Publish(const PlatformServicesProjectSourceSnapshot &source) {
            NativeDurableFileSystem files;
            const auto destination = source.ProjectRoot() / PlatformServicesProjectCookPath;
            auto lockPath = Assets::AssetSourcePublicationLockPath(source.ProjectRoot(), destination);
            REQUIRE(lockPath.HasValue());
            auto lease = files.TryAcquireExclusive(lockPath.Value(), "platform-cook-regression");
            REQUIRE(lease.HasValue());
            auto previous =
                Assets::CaptureAssetSource(source.ProjectRoot(), destination, PlatformProviderManifestCookGenerationMaximumBytes);
            REQUIRE(previous.HasValue());
            auto published = CookAndPublishPlatformServicesProject({.sources = source}, previous.Value().identity, lease.Value(), files);
            REQUIRE(published.HasValue());
            REQUIRE(published.Value().publication.status == Assets::AssetSourcePublicationStatus::Durable);
            auto current =
                Assets::CaptureAssetSource(source.ProjectRoot(), destination, PlatformProviderManifestCookGenerationMaximumBytes);
            REQUIRE(current.HasValue());
            CHECK(current.Value().identity.digest == published.Value().generationFingerprint);
            REQUIRE(
                DecodePlatformProviderManifestCookGeneration(current.Value().bytes, published.Value().generationFingerprint).HasValue());
            return std::move(current).Value();
        }
    }  // namespace

    TEST_CASE("Every host captures the same six portable sources with an explicit profile", "[platform-sources]") {
        Project project;
        project.InitializeCurrent();
        const auto admission = InspectProjectCompatibility(project.root);
        REQUIRE(admission.status == ProjectCompatibilityStatus::Current);
        REQUIRE(admission.sourceFingerprint.has_value());
        std::optional<Sha256Digest> fingerprint;
        for (const auto profile : {PlatformServicesHostProfile::InteractiveDevelopment, PlatformServicesHostProfile::HeadlessServer,
                                   PlatformServicesHostProfile::Cook, PlatformServicesHostProfile::Certification}) {
            const auto captured = CapturePlatformServicesProjectSources(project.root, admission, {.profile = profile});
            REQUIRE(captured.HasValue());
            CHECK(captured.Value().Configuration().Profile() == profile);
            CHECK(captured.Value().Configuration().UsesNullProvider());
            CHECK(captured.Value().RootIdentity().digest == *admission.sourceFingerprint);
            if (fingerprint)
                CHECK(captured.Value().SourceFingerprint() == *fingerprint);
            fingerprint = captured.Value().SourceFingerprint();
            REQUIRE(RevalidatePlatformServicesProjectSources(captured.Value()).HasValue());
        }
    }

    TEST_CASE("Incremental captured semantic sources converge with a clean checkout cook", "[platform-sources]") {
        Project incremental;
        incremental.InitializeCurrent();
        WriteSemanticSources(incremental, false);
        const auto initial = CapturePlatformServicesProjectSources(incremental.root, InspectProjectCompatibility(incremental.root),
                                                                   {.profile = PlatformServicesHostProfile::Cook});
        REQUIRE(initial.HasValue());
        REQUIRE(initial.Value().Achievements().Definitions().size() == 1);
        REQUIRE(initial.Value().Stats().Definitions().size() == 1);
        REQUIRE(initial.Value().Leaderboards().Definitions().size() == 1);
        REQUIRE(initial.Value().Presence().Definitions().size() == 1);
        CHECK(initial.Value().StableIds().Entries().size() == 8);
        const auto previouslyPublished = Publish(initial.Value());
        WriteSemanticSources(incremental, true);
        CHECK(RevalidatePlatformServicesProjectSources(initial.Value()).HasError());
        const auto edited = CapturePlatformServicesProjectSources(incremental.root, InspectProjectCompatibility(incremental.root),
                                                                  {.profile = PlatformServicesHostProfile::Cook});
        REQUIRE(edited.HasValue());
        Project clean;
        clean.InitializeCurrent();
        WriteSemanticSources(clean, true);
        const auto reloaded = CapturePlatformServicesProjectSources(clean.root, InspectProjectCompatibility(clean.root),
                                                                    {.profile = PlatformServicesHostProfile::Cook});
        REQUIRE(reloaded.HasValue());
        CHECK(edited.Value().SourceFingerprint() == reloaded.Value().SourceFingerprint());
        CHECK(edited.Value().StableIds().ResolveAchievement("retired.key").HasError());
        const auto incrementalCook = Cook(edited.Value());
        const auto cleanCook = Cook(reloaded.Value());
        CHECK(incrementalCook.neutralBytes == cleanCook.neutralBytes);
        CHECK(incrementalCook.mappingBytes == cleanCook.mappingBytes);
        CHECK(incrementalCook.generationFingerprint == cleanCook.generationFingerprint);
        const auto incrementalPublished = Publish(edited.Value());
        const auto cleanPublished = Publish(reloaded.Value());
        CHECK(incrementalPublished.bytes == cleanPublished.bytes);
        CHECK(incrementalPublished.identity == cleanPublished.identity);
        CHECK(previouslyPublished.identity != incrementalPublished.identity);
    }

    TEST_CASE("Exact source edits invalidate captured generations even when semantic fingerprints stay equal", "[platform-sources]") {
        Project project;
        project.InitializeCurrent();
        const auto admission = InspectProjectCompatibility(project.root);
        auto captured = CapturePlatformServicesProjectSources(project.root, admission, {.profile = PlatformServicesHostProfile::Cook});
        REQUIRE(captured.HasValue());
        std::string_view path;
        SECTION("Root") {
            path = ".horo/project.json";
        }
        SECTION("Ledger") {
            path = PlatformServicesDocumentPaths::Ledger;
        }
        SECTION("Achievement") {
            path = PlatformServicesDocumentPaths::Achievements;
        }
        SECTION("Stat") {
            path = PlatformServicesDocumentPaths::Stats;
        }
        SECTION("Leaderboard") {
            path = PlatformServicesDocumentPaths::Leaderboards;
        }
        SECTION("Presence") {
            path = PlatformServicesDocumentPaths::Presence;
        }
        project.Write(path, project.Read(path) + " \n");
        const auto stale = RevalidatePlatformServicesProjectSources(captured.Value());
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == Assets::AssetSourceErrors::Conflict.code.Value());
        const auto refreshedAdmission = InspectProjectCompatibility(project.root);
        auto reloaded =
            CapturePlatformServicesProjectSources(project.root, refreshedAdmission, {.profile = PlatformServicesHostProfile::Cook});
        REQUIRE(reloaded.HasValue());
        CHECK(reloaded.Value().SourceFingerprint() != captured.Value().SourceFingerprint());
        CHECK(reloaded.Value().StableIds().Fingerprint() == captured.Value().StableIds().Fingerprint());
        const auto before = Cook(captured.Value());
        const auto after = Cook(reloaded.Value());
        CHECK(before.neutralBytes == after.neutralBytes);
        CHECK(before.mappingBytes == after.mappingBytes);
        CHECK(before.generationFingerprint == after.generationFingerprint);
    }

    TEST_CASE("Source capture binds compatibility to exact root bytes and never repairs identity", "[platform-sources]") {
        Project project;
        project.InitializeCurrent();
        auto admission = InspectProjectCompatibility(project.root);
        const auto original = project.Read(".horo/project.json");
        SECTION("Changed root after admission") {
            project.Write(".horo/project.json", original + " ");
        }
        SECTION("Admission without a real source witness") {
            admission.sourceFingerprint.reset();
        }
        SECTION("Pending root marker migration") {
            admission.markerUpdateRequired = true;
        }
        SECTION("Nonadmitted compatibility status") {
            admission.status = ProjectCompatibilityStatus::FutureVersion;
        }
        SECTION("Missing required salt") {
            auto metadata = nlohmann::json::parse(original);
            metadata.erase("platformServicesIdSalt");
            project.Write(".horo/project.json", metadata.dump());
            admission = InspectProjectCompatibility(project.root);
        }
        SECTION("Deleted required sidecar") {
            REQUIRE(std::filesystem::remove(project.root / PlatformServicesDocumentPaths::Stats));
        }
        const auto sourceBefore = project.Read(".horo/project.json");
        const auto captured = CapturePlatformServicesProjectSources(project.root, admission, {});
        REQUIRE(captured.HasError());
        CHECK(project.Read(".horo/project.json") == sourceBefore);
    }

    TEST_CASE("Source capture resolves actual trust and profile metadata without provider fallback", "[platform-sources]") {
        Project project;
        project.InitializeCurrent();
        auto metadata = nlohmann::json::parse(project.Read(".horo/project.json"));
        metadata["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "platform.fixture"}};
        metadata["settings"]["platformServices"]["services"][0] = 2;
        project.Write(".horo/project.json", metadata.dump());
        const auto admission = InspectProjectCompatibility(project.root);
        PlatformServicesProjectValidationContext context{.profile = PlatformServicesHostProfile::Cook};
        auto unavailable = CapturePlatformServicesProjectSources(project.root, admission, context);
        REQUIRE(unavailable.HasError());
        CHECK(unavailable.ErrorValue().code.Value() == PlatformProjectConfigurationErrors::ProviderNotFound.code.Value());
        PlatformProviderModuleContribution contribution{.module = ModuleId{"horo.platform.fixture"},
                                                        .providerKey = "platform.fixture",
                                                        .provider = PlatformProviderId{41},
                                                        .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                             PlatformServicesBackendInterfaceMinor},
                                                        .allowedProfiles = PlatformServicesHostProfileMask::Cook};
        contribution.supportedServices.fill(true);
        context.contributions.push_back(contribution);
        const auto untrusted = CapturePlatformServicesProjectSources(project.root, admission, context);
        REQUIRE(untrusted.HasError());
        CHECK(untrusted.ErrorValue().code.Value() == PlatformProjectConfigurationErrors::UntrustedContribution.code.Value());
        context.trustedModules.push_back(contribution.module);
        const auto resolved = CapturePlatformServicesProjectSources(project.root, admission, context);
        REQUIRE(resolved.HasValue());
        CHECK_FALSE(resolved.Value().Configuration().UsesNullProvider());
        CHECK(resolved.Value().Configuration().SelectedProvider() == PlatformProviderId{41});
        context.profile = PlatformServicesHostProfile::HeadlessServer;
        const auto denied = CapturePlatformServicesProjectSources(project.root, admission, context);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == PlatformProjectConfigurationErrors::ProfileDenied.code.Value());
    }

    TEST_CASE("Source capture obeys finite sidecar bounds and cooperative cancellation", "[platform-sources]") {
        Project project;
        project.InitializeCurrent();
        const auto admission = InspectProjectCompatibility(project.root);
        CancellationSource cancellation;
        SECTION("Cancelled") {
            cancellation.RequestCancellation();
        }
        SECTION("Oversized source") {
            project.Write(PlatformServicesDocumentPaths::Presence, std::string(4U * 1024U * 1024U + 1, ' '));
        }
        const auto captured = CapturePlatformServicesProjectSources(project.root, admission, {}, cancellation.Token());
        REQUIRE(captured.HasError());
    }
}  // namespace Horo::Tests::PlatformMigration
