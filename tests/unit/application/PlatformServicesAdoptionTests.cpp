#include "../../support/project/PlatformServicesMigrationPlanFixture.h"

namespace Horo::Tests::PlatformMigration {
    TEST_CASE("Real platform adoption stages a complete generation without source publication", "[platform-adoption]") {
        Project project(std::string(256, 'x'));
        const auto original = project.Read(".horo/project.json");
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        auto random = std::make_shared<Random>();
        const auto plan = PlatformAdoptionPlan(project);
        CHECK(random->calls == 0);
        auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/platform-candidate", plan, jobs, {}, {},
                                                          {.random = random});
        REQUIRE(prepared.HasValue());
        CHECK(random->calls == 1);
        CHECK(project.Read(".horo/project.json") == original);
        CHECK_FALSE(std::filesystem::exists(project.root / PlatformServicesDocumentPaths::Ledger));
        const auto rootBytes = prepared.Value().ReadCandidateDocument(".horo/project.json");
        REQUIRE(rootBytes.HasValue());
        const std::string_view rootText{reinterpret_cast<const char *>(rootBytes.Value().data()), rootBytes.Value().size()};
        const auto root = DecodeProjectSourceDocument(rootText);
        REQUIRE(root.HasValue());
        const auto salt = ResolveProjectPlatformServicesSalt(root.Value());
        REQUIRE(salt.HasValue());
        const auto ledgerBytes = prepared.Value().ReadCandidateDocument(PlatformServicesDocumentPaths::Ledger);
        REQUIRE(ledgerBytes.HasValue());
        const std::string_view ledgerText{reinterpret_cast<const char *>(ledgerBytes.Value().data()), ledgerBytes.Value().size()};
        const auto ledger = DecodePlatformServicesLedger(ledgerText, root.Value().metadata.projectId, salt.Value());
        REQUIRE(ledger.HasValue());
        CHECK(ledger.Value().Entries().empty());
        CHECK(ledgerText.find("psid1:") == std::string_view::npos);
        CHECK(nlohmann::json::parse(rootText)["unrelatedRootValue"] == nlohmann::json::parse(original)["unrelatedRootValue"]);
        CHECK(prepared.Value().Changes().size() == 6);
    }

    TEST_CASE("Platform adoption preserves complete existing identity and unresolved exact selection", "[platform-adoption]") {
        Project project;
        PlatformServicesIdSalt salt;
        salt.bytes.front() = std::byte{1};
        auto initial = PreparePlatformServicesInitialDocuments(project.Read(".horo/project.json"), salt);
        REQUIRE(initial.HasValue());
        auto root = nlohmann::json::parse(initial.Value().projectSource);
        root["settings"]["platformServices"]["provider"] = {{"mode", "exact"}, {"key", "platform.not_installed"}};
        initial.Value().projectSource = root.dump(2) + "\n";
        project.WriteInitial(initial.Value());
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        auto random = std::make_shared<Random>();
        random->fail = true;
        auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/platform-candidate",
                                                          PlatformAdoptionPlan(project), jobs, {}, {}, {.random = random});
        REQUIRE(prepared.HasValue());
        CHECK(random->calls == 0);
        CHECK(prepared.Value().Changes().empty());
        CHECK(project.Read(".horo/project.json") == initial.Value().projectSource);
        const auto source = DecodeProjectSourceDocument(initial.Value().projectSource);
        REQUIRE(source.HasValue());
        const auto portable = DecodeProjectPlatformServicesConfigurationCandidate(source.Value(), PlatformServicesHostProfile::Cook);
        REQUIRE(portable.HasValue());
        CHECK(portable.Value().provider.providerKey == "platform.not_installed");
        const auto activation = ResolveProjectPlatformServicesConfiguration(source.Value(), PlatformServicesHostProfile::Cook, {}, {});
        REQUIRE(activation.HasError());
        CHECK(activation.ErrorValue().code.Value() == PlatformProjectConfigurationErrors::ProviderNotFound.code.Value());
    }

    TEST_CASE("Platform adoption never repairs partial or unrecognized identity", "[platform-adoption]") {
        Project project;
        const auto plan = PlatformAdoptionPlan(project);
        SECTION("Salt alone") {
            auto root = nlohmann::json::parse(project.Read(".horo/project.json"));
            root["platformServicesIdSalt"] = "psid1:01000000000000000000000000000000";
            project.Write(".horo/project.json", root.dump());
        }
        SECTION("Ledger alone") {
            project.Write(PlatformServicesDocumentPaths::Ledger, "{}");
        }
        SECTION("Unknown existing provider document") {
            project.Write(".horo/platform_services.private.json", "private fixture bytes");
        }
        SECTION("Noncanonical case on a portable namespace") {
            project.Write(".horo/PLATFORM_SERVICES.IDS.JSON", "{}");
        }
        SECTION("Required current schema with an erased namespace") {
            auto root = nlohmann::json::parse(project.Read(".horo/project.json"));
            root["horoVersion"] = "0.4.0";
            const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
            REQUIRE(decision != nullptr);
            root["persistentContract"] = FormatPersistentContractHash(decision->persistentContract);
            project.Write(".horo/project.json", root.dump());
        }
        SECTION("Missing semantic document from a complete identity pair") {
            PlatformServicesIdSalt salt;
            salt.bytes.front() = std::byte{1};
            const auto initial = PreparePlatformServicesInitialDocuments(project.Read(".horo/project.json"), salt);
            REQUIRE(initial.HasValue());
            project.WriteInitial(initial.Value());
            REQUIRE(std::filesystem::remove(project.root / PlatformServicesDocumentPaths::Presence));
        }
        const auto original = project.Read(".horo/project.json");
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        auto random = std::make_shared<Random>();
        auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/platform-candidate", plan, jobs, {}, {},
                                                          {.random = random});
        REQUIRE(prepared.HasError());
        CHECK(random->calls == 0);
        CHECK(project.Read(".horo/project.json") == original);
    }

    TEST_CASE("Platform adoption entropy failure leaves the source namespace absent", "[platform-adoption]") {
        Project project;
        auto random = std::make_shared<Random>();
        SECTION("Provider failure") {
            random->fail = true;
        }
        SECTION("Zero output") {
            random->zero = true;
        }
        const auto original = project.Read(".horo/project.json");
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        auto prepared = ProjectMigrationExecutor::Prepare(project.root, project.root / ".horo/local/platform-candidate",
                                                          PlatformAdoptionPlan(project), jobs, {}, {}, {.random = random});
        REQUIRE(prepared.HasError());
        CHECK(random->calls == 1);
        CHECK(project.Read(".horo/project.json") == original);
        CHECK_FALSE(std::filesystem::exists(project.root / PlatformServicesDocumentPaths::Ledger));
    }
}  // namespace Horo::Tests::PlatformMigration
