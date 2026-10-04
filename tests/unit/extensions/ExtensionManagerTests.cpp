#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetReimport.h"
#include "Horo/Extensions/ExtensionDiscovery.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionManifest.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Security/SecurityErrors.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <type_traits>

namespace Horo::Extensions::Tests {
    namespace fs = std::filesystem;

    static_assert(!std::is_move_constructible_v<ExtensionManager>);
    static_assert(!std::is_move_assignable_v<ExtensionManager>);

    TEST_CASE("Marketplace registry parsing filters immutable compatible entries") {
        constexpr std::string_view Registry = R"json({
          "packages": [{
            "id": "com.example.mesh-tools",
            "displayName": "Mesh Tools",
            "description": "Mesh import helpers",
            "publisher": "Example",
            "latest": "1.2.0",
            "versions": {
              "1.2.0": {
                "packageUrl": "https://example.com/mesh-tools-1.2.0.zip",
                "sha256": "sha256:0000000000000000000000000000000000000000000000000000000000000000"
              }
            }
          }]
        })json";

        auto parsed = ParseExtensionMarketplaceRegistry(Registry, "mesh");
        REQUIRE(parsed.HasValue());
        REQUIRE(parsed.Value().size() == 1);
        CHECK(parsed.Value().front().packageId == "com.example.mesh-tools");
        CHECK(parsed.Value().front().version == "1.2.0");

        auto noMatch = ParseExtensionMarketplaceRegistry(Registry, "audio");
        REQUIRE(noMatch.HasValue());
        CHECK(noMatch.Value().empty());
    }

    TEST_CASE("Marketplace registry rejects mutable or unverifiable artifacts") {
        constexpr std::string_view Registry = R"json({
          "packages": [{
            "id": "com.example.unsafe",
            "latest": "1.0.0",
            "versions": {
              "1.0.0": {
                "packageUrl": "http://example.com/unsafe.zip",
                "sha256": "missing"
              }
            }
          }]
        })json";

        auto parsed = ParseExtensionMarketplaceRegistry(Registry, "");
        REQUIRE(parsed.HasValue());
        CHECK(parsed.Value().empty());
    }

    class ExistingImporter final : public Assets::IAssetImporter {
    public:
        [[nodiscard]] Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &,
                                                                 const CancellationToken &) const override {
            return Result<Assets::PreparedAssetImport>::Failure(MakeError(ExtensionErrors::InvocationFailed, "Not invoked by this test."));
        }
    };

    /** @brief Creates a pre-existing catalog entry used to verify transaction isolation. */
    [[nodiscard]] static Assets::AssetImporterContribution MakeExistingImporterContribution(const std::string_view contributionId) {
        return {
            .contributionId = std::string{contributionId},
            .packageId = "existing.package",
            .moduleId = "existing.module",
            .moduleVersion = "1.0.0",
            .version = "1.0.0",
            .fileExtensions = {"existing"},
            .assetTypes = {Assets::AssetTypeId::Parse("example.raw").Value()},
            .strategy = std::make_shared<const ExistingImporter>(),
        };
    }

    struct ExtensionManagerTestFixture {
        fs::path tempDir;

        ExtensionManagerTestFixture() {
            tempDir = fs::temp_directory_path() / "horo_extension_tests" / "plugins";
            fs::create_directories(tempDir);
        }

        ~ExtensionManagerTestFixture() {
            std::error_code ec;
            fs::remove_all(tempDir, ec);
        }

#ifdef HORO_ABI_FIXTURE_0
        [[nodiscard]] std::string PrepareMultiModuleLibraries() const {
            const fs::path source = fs::absolute(HORO_ABI_FIXTURE_0);
            const std::string extension = source.extension().string();
            fs::copy_file(source, tempDir / ("backend" + extension), fs::copy_options::overwrite_existing);
            fs::copy_file(source, tempDir / ("editor" + extension), fs::copy_options::overwrite_existing);
            return extension;
        }

        void WriteMultiModuleManifest(const std::string &modules) const {
            std::ofstream output{tempDir / "extension.json", std::ios::binary | std::ios::trunc};
            output << "{\"id\":\"com.example.multi\",\"version\":\"1.0.0\",\"modules\":" << modules << '}';
        }

        [[nodiscard]] static std::string MissingPresentationModules(const std::string &extension) {
            return "[{\"id\":\"com.example.multi.backend\",\"version\":\"1.0.0\",\"kind\":\"native\","
                   "\"roles\":[\"backend-capability\"],\"entry\":\"backend" +
                   extension +
                   "\"},{\"id\":\"com.example.multi.editor\",\"version\":\"1.0.0\",\"kind\":\"native\","
                   "\"roles\":[\"editor-presentation\"],\"dependencies\":[\"com.example.multi.backend\"],\"entry\":\"missing" +
                   extension + "\"}]";
        }

        void RequireMultiModuleLoaded(const ExtensionHostProfile profile, std::vector<std::string> capabilities = {}) const {
            ExtensionManager manager{nullptr, profile, std::move(capabilities), Horo::Tests::CreateAcceptingArtifactGate()};
            REQUIRE(manager.LoadExtension(fs::absolute(tempDir).string()).HasValue());
            CHECK(manager.GetLoadedExtensionIds() == std::vector<std::string>{"com.example.multi"});
        }
#endif
    };

    class RejectingArtifactGate final : public Security::NativeArtifactGate {
    public:
        explicit RejectingArtifactGate(const ErrorCodeDescriptor &error) : error_(error) {}

        [[nodiscard]] Result<Security::VerifiedArtifactEvidence> Verify(const fs::path &) const override {
            return Result<Security::VerifiedArtifactEvidence>::Failure(MakeError(error_));
        }

    private:
        const ErrorCodeDescriptor &error_;
    };

    class MutatingArtifactGate final : public Security::NativeArtifactGate {
    public:
        explicit MutatingArtifactGate(std::shared_ptr<const Security::NativeArtifactGate> delegate) : delegate_(std::move(delegate)) {}

        [[nodiscard]] Result<Security::VerifiedArtifactEvidence> Verify(const fs::path &path) const override {
            auto evidence = delegate_->Verify(path);
            if (evidence.HasError())
                return evidence;
            std::ofstream changed{path, std::ios::binary | std::ios::app};
            changed.put('\0');
            changed.close();
            return evidence;
        }

    private:
        std::shared_ptr<const Security::NativeArtifactGate> delegate_;
    };

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "ExtensionManager Discovery", "[Extensions]") {
        const Discovery::RootRequest root{.id = "user",
                                          .path = fs::absolute(tempDir),
                                          .kind = Discovery::RootKind::User,
                                          .approval = Discovery::RootApproval::Approved,
                                          .configuration = Discovery::ConfigurationOrigin::UserLocal};

        SECTION("No declared packages returns an empty list") {
            const auto discovered = Discovery::DiscoverDeclaredPackages(std::span(&root, 1), {}, {});
            REQUIRE(discovered.HasValue());
            REQUIRE(discovered.Value().packages.empty());
        }

        SECTION("An approved declared package resolves to a canonical directory") {
            fs::path pluginDir = tempDir / "com.example.test_plugin";
            fs::create_directories(pluginDir);
            const Discovery::PackageLocation location{"com.example.test-plugin", "user", "com.example.test_plugin"};
            const auto discovered = Discovery::DiscoverDeclaredPackages(std::span(&root, 1), std::span(&location, 1), {});
            REQUIRE(discovered.HasValue());
            REQUIRE(discovered.Value().packages.size() == 1);
            REQUIRE(discovered.Value().packages[0].canonicalPath == fs::canonical(pluginDir));
        }
    }

    TEST_CASE("ExtensionManifest Parser", "[Extensions]") {
        SECTION("Valid JSON returns manifest") {
            std::string json = R"({
                "package": {
                    "id": "com.example.test",
                    "version": "1.2.3",
                    "kind": "editor_panel",
                    "displayName": "Test Panel",
                    "description": "A test panel",
                    "author": "Horo"
                },
                "modules": [{
                    "id": "com.example.test.importer",
                    "version": "2.0.0",
                    "kind": "asset_importer",
                    "roles": ["backend-capability"],
                    "entry": "test_importer"
                }]
            })";

            auto result = ParseExtensionManifest(json);
            REQUIRE(result.HasValue());

            const auto &manifest = result.Value();
            REQUIRE(manifest.id == "com.example.test");
            REQUIRE(manifest.version == "1.2.3");
            REQUIRE(manifest.kind == "editor_panel");
            REQUIRE(manifest.displayName == "Test Panel");
            REQUIRE(manifest.description == "A test panel");
            REQUIRE(manifest.author == "Horo");
            REQUIRE(manifest.modules.size() == 1);
            REQUIRE(manifest.modules[0].id == "com.example.test.importer");
            REQUIRE(manifest.modules[0].version == "2.0.0");
        }

        SECTION("Module version is explicit") {
            auto result = ParseExtensionManifest(R"({
                "package": {"id": "com.example.defaulted", "version": "3.2.1"},
                "modules": [{"id": "com.example.defaulted.importer", "kind": "asset_importer"}]
            })");
            REQUIRE(result.HasError());
            REQUIRE_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring("$.modules[0].version"));
        }

        SECTION("Canonical top-level package manifest is accepted") {
            auto result = ParseExtensionManifest(R"({
                "id": "com.example.top-level",
                "version": "4.0.0",
                "modules": [{
                    "id": "com.example.top-level.importer",
                    "version": "4.1.0",
                    "kind": "asset_importer",
                    "roles": ["backend-capability"]
                }]
            })");
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().version == "4.0.0");
            REQUIRE(result.Value().modules[0].version == "4.1.0");
        }

        SECTION("Contribution must reference a module in the same package") {
            auto result = ParseExtensionManifest(R"({
                "id": "com.example.contributions",
                "version": "1.0.0",
                "modules": [{
                    "id": "com.example.contributions.native",
                    "version": "1.0.0",
                    "kind": "asset_importer"
                }],
                "contributions": [{
                    "type": "asset.importer",
                    "id": "com.example.contributions.raw",
                    "module": "com.example.missing"
                }]
            })");
            REQUIRE(result.HasError());
        }

        SECTION("Invalid module version is rejected") {
            auto result = ParseExtensionManifest(R"({
                "package": {"id": "com.example.invalid", "version": "1.0.0"},
                "modules": [{"id": "com.example.invalid.importer", "version": "next"}]
            })");
            REQUIRE(result.HasError());
        }

        SECTION("Missing package returns error") {
            std::string json = R"({
                "some_other_field": "value"
            })";

            auto result = ParseExtensionManifest(json);
            REQUIRE(result.HasError());
        }
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension inventory exposes built-ins and persists activation state",
                     "[Extensions][Inventory]") {
        const fs::path installRoot = fs::absolute(tempDir / "installed");
        ExtensionInventory inventory{installRoot};
        REQUIRE(inventory.Refresh().HasValue());
        REQUIRE(inventory.InstallRoot().is_absolute());
        REQUIRE(inventory.Entries().size() == 1);
        const ExtensionInventoryEntry &builtIn = inventory.Entries().front();
        REQUIRE(builtIn.packageId == "horo.builtin.assets");
        REQUIRE(builtIn.origin == ExtensionOrigin::BuiltIn);
        REQUIRE(builtIn.enabled);
        REQUIRE(builtIn.modules.size() == 2);

        REQUIRE(inventory.MarkRuntimeActive(builtIn.packageId).HasValue());
        REQUIRE_FALSE(inventory.Entries().front().RestartRequired());
        REQUIRE(inventory.SetEnabled(builtIn.packageId, false).HasValue());
        REQUIRE(inventory.Entries().front().RestartRequired());

        ExtensionInventory reloaded{installRoot};
        REQUIRE(reloaded.Refresh().HasValue());
        REQUIRE_FALSE(reloaded.IsEnabled(builtIn.packageId));
        REQUIRE(reloaded.InstallRoot() == installRoot.lexically_normal());
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension manifest files are bounded before decoding", "[Extensions][Inventory]") {
        const fs::path source = fs::absolute(tempDir / "oversized");
        fs::create_directories(source);
        {
            std::ofstream manifest{source / "extension.json", std::ios::binary};
            manifest << std::string(ExtensionManifestLimits{}.maximumDocumentBytes + 1U, 'x');
        }

        ExtensionManager manager;
        REQUIRE(manager.LoadExtension(source.string()).HasError());

        ExtensionInventory inventory{fs::absolute(tempDir / "managed")};
        REQUIRE(inventory.InstallFromDirectory(source).HasError());
    }

#ifdef HORO_ABI_FIXTURE_0
    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Security failures prevent native loading and extension callbacks",
                     "[Extensions][Security]") {
        const fs::path source = fs::absolute(HORO_ABI_FIXTURE_0);
        const fs::path library = tempDir / source.filename();
        fs::copy_file(source, library, fs::copy_options::overwrite_existing);
        {
            std::ofstream manifest{tempDir / "extension.json", std::ios::binary | std::ios::trunc};
            manifest
                << R"({"id":"com.example.secure","version":"1.0.0","modules":[{"id":"com.example.secure.native","version":"1.0.0","kind":"native","roles":["backend-capability"],"entry":")"
                << library.filename().generic_string() << R"("}]})";
        }
        const std::array<const ErrorCodeDescriptor *, 6> failures{
            &SecurityErrors::MissingEvidence,   &SecurityErrors::IntegrityMismatch,    &SecurityErrors::InvalidSignature,
            &SecurityErrors::UnknownSigningKey, &SecurityErrors::UnsupportedAlgorithm, &SecurityErrors::StaleEvidence,
        };
        for (const ErrorCodeDescriptor *failure : failures) {
            std::size_t loaderCalls{};
            auto loader = [&loaderCalls](const std::string &) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
                ++loaderCalls;
                return Result<std::unique_ptr<Platform::DynamicLibrary>>::Failure(MakeError(ExtensionErrors::LoadFailed));
            };
            ExtensionManager manager{nullptr,
                                     ExtensionHostProfile::Interactive,
                                     {},
                                     std::make_shared<RejectingArtifactGate>(*failure),
                                     std::move(loader)};
            const auto loaded = manager.LoadExtension(fs::absolute(tempDir).string());
            REQUIRE(loaded.HasError());
            CHECK(loaded.ErrorValue().code.Value() == failure->code.Value());
            CHECK(loaderCalls == 0U);
        }

        std::size_t missingGateLoaderCalls{};
        auto missingGateLoader = [&missingGateLoaderCalls](const std::string &) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
            ++missingGateLoaderCalls;
            return Result<std::unique_ptr<Platform::DynamicLibrary>>::Failure(MakeError(ExtensionErrors::LoadFailed));
        };
        ExtensionManager missingGateManager{nullptr, ExtensionHostProfile::Interactive, {}, nullptr, std::move(missingGateLoader)};
        const auto missingGate = missingGateManager.LoadExtension(fs::absolute(tempDir).string());
        REQUIRE(missingGate.HasError());
        CHECK(missingGate.ErrorValue().code.Value() == SecurityErrors::MissingEvidence.code.Value());
        CHECK(missingGateLoaderCalls == 0U);

        std::size_t staleLoaderCalls{};
        auto staleLoader = [&staleLoaderCalls](const std::string &) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
            ++staleLoaderCalls;
            return Result<std::unique_ptr<Platform::DynamicLibrary>>::Failure(MakeError(ExtensionErrors::LoadFailed));
        };
        ExtensionManager staleManager{
            nullptr,
            ExtensionHostProfile::Interactive,
            {},
            std::make_shared<MutatingArtifactGate>(Horo::Tests::CreateAcceptingArtifactGate()),
            std::move(staleLoader),
        };
        const auto stale = staleManager.LoadExtension(fs::absolute(tempDir).string());
        REQUIRE(stale.HasError());
        CHECK(stale.ErrorValue().code.Value() == SecurityErrors::StaleEvidence.code.Value());
        CHECK(staleLoaderCalls == 0U);
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension manager stages selected modules atomically", "[Extensions][Modules]") {
        const std::string extension = PrepareMultiModuleLibraries();
        SECTION("all selected siblings publish as one loaded package") {
            WriteMultiModuleManifest(
                "[{\"id\":\"com.example.multi.backend\",\"version\":\"1.0.0\",\"kind\":\"native\","
                "\"roles\":[\"backend-capability\"],\"entry\":\"backend" +
                extension +
                "\"},{\"id\":\"com.example.multi.editor\",\"version\":\"1.0.0\",\"kind\":\"native\","
                "\"roles\":[\"editor-presentation\"],\"dependencies\":[\"com.example.multi.backend\"],\"entry\":\"editor" +
                extension + "\"}]");
            RequireMultiModuleLoaded(ExtensionHostProfile::Interactive);
        }

        SECTION("explicit host capabilities admit a required module before activation") {
            WriteMultiModuleManifest("[{\"id\":\"com.example.multi.backend\",\"version\":\"1.0.0\",\"kind\":\"native\","
                                     "\"roles\":[\"backend-capability\"],\"entry\":\"backend" +
                                     extension + "\",\"requiredCapabilities\":[\"com.horo.assets\"]}]");
            RequireMultiModuleLoaded(ExtensionHostProfile::Interactive, {"com.horo.assets", "com.horo.assets", ""});
        }
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension manager rolls back or omits invalid presentation siblings",
                     "[Extensions][Modules]") {
        const std::string extension = PrepareMultiModuleLibraries();
        SECTION("a failing sibling rolls back the complete activation attempt") {
            WriteMultiModuleManifest(MissingPresentationModules(extension));
            ExtensionManager manager{nullptr, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
            REQUIRE(manager.LoadExtension(fs::absolute(tempDir).string()).HasError());
            CHECK(manager.GetLoadedExtensionIds().empty());
        }

        SECTION("headless activation does not construct an optional presentation sibling") {
            WriteMultiModuleManifest(MissingPresentationModules(extension));
            RequireMultiModuleLoaded(ExtensionHostProfile::Headless);
        }
    }
#endif

#ifdef HORO_BASIC_EXTENSION_DIR
    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension inventory installs an absolute local package disabled by default",
                     "[Extensions][Inventory]") {
        const fs::path installRoot = fs::absolute(tempDir / "managed");
        ExtensionInventory inventory{installRoot};
        REQUIRE(inventory.Refresh().HasValue());
        REQUIRE(inventory.InstallFromDirectory("relative/package").HasError());

        const fs::path source = fs::absolute(HORO_BASIC_EXTENSION_DIR);
        auto installed = inventory.InstallFromDirectory(source);
        REQUIRE(installed.HasValue());
        REQUIRE(installed.Value() == "com.horo.examples.asset-importer-basic");

        auto entry = std::ranges::find(inventory.Entries(), installed.Value(), &ExtensionInventoryEntry::packageId);
        REQUIRE(entry != inventory.Entries().end());
        REQUIRE(entry->origin == ExtensionOrigin::UserInstalled);
        REQUIRE(entry->absoluteRootPath.is_absolute());
        REQUIRE(entry->absoluteManifestPath.is_absolute());
        REQUIRE_FALSE(entry->enabled);
        REQUIRE_FALSE(entry->locallyTrusted);
        REQUIRE(inventory.EnabledUserPackageRoots().empty());

        {
            std::ofstream legacyState{installRoot / "_state.json", std::ios::binary | std::ios::trunc};
            legacyState << R"({"schemaVersion":1,"enabled":["com.horo.examples.asset-importer-basic"],)"
                           R"("trusted":["com.horo.examples.asset-importer-basic"]})";
        }
        REQUIRE(inventory.Refresh().HasValue());
        entry = std::ranges::find(inventory.Entries(), installed.Value(), &ExtensionInventoryEntry::packageId);
        REQUIRE(entry != inventory.Entries().end());
        REQUIRE(entry->enabled);
        REQUIRE_FALSE(entry->locallyTrusted);
        REQUIRE(inventory.EnabledUserPackageRoots().empty());
        REQUIRE(inventory.SetEnabled(entry->packageId, false).HasValue());
        REQUIRE(inventory.SetEnabled(entry->packageId, true).HasValue());
        REQUIRE_FALSE(entry->locallyTrusted);
        REQUIRE(inventory.EnabledUserPackageRoots().empty());
        REQUIRE(inventory.SetTrusted(entry->packageId, true).HasValue());
        const auto roots = inventory.EnabledUserPackageRoots();
        REQUIRE(roots.size() == 1);
        REQUIRE(roots.front().is_absolute());
        ExtensionInventory persisted{installRoot};
        REQUIRE(persisted.Refresh().HasValue());
        const auto persistedEntry = std::ranges::find(persisted.Entries(), installed.Value(), &ExtensionInventoryEntry::packageId);
        REQUIRE(persistedEntry != persisted.Entries().end());
        REQUIRE(persistedEntry->enabled);
        REQUIRE(persistedEntry->locallyTrusted);
        REQUIRE(persistedEntry->trustedCompositionVersion == persistedEntry->compositionVersion);
        REQUIRE(inventory.MarkRuntimeActive(installed.Value()).HasValue());
        entry = std::ranges::find(inventory.Entries(), installed.Value(), &ExtensionInventoryEntry::packageId);
        REQUIRE_FALSE(entry->RestartRequired());
        REQUIRE(inventory.SetTrusted(entry->packageId, false).HasValue());
        REQUIRE(entry->runtimeActive);
        REQUIRE(entry->ActivationState().RestartReason() == ExtensionRestartReason::DeactivationRequired);
        REQUIRE(inventory.EnabledUserPackageRoots().empty());
        REQUIRE(inventory.SetTrusted(entry->packageId, true).HasValue());
        REQUIRE_FALSE(entry->RestartRequired());

        const fs::path installedManifest = roots.front() / "extension.json";
        std::ifstream manifestInput(installedManifest, std::ios::binary);
        std::string manifestText{std::istreambuf_iterator<char>{manifestInput}, std::istreambuf_iterator<char>{}};
        const std::string authorField{"\"author\": \"Horo Engine\""};
        const std::size_t author = manifestText.find(authorField);
        REQUIRE(author != std::string::npos);
        manifestText.replace(author, authorField.size(), "\"author\": \"Changed Publisher\"");
        {
            std::ofstream manifestOutput(installedManifest, std::ios::binary | std::ios::trunc);
            manifestOutput << manifestText;
        }
        REQUIRE(inventory.Refresh().HasValue());
        const auto changedPublisher = std::ranges::find(inventory.Entries(), installed.Value(), &ExtensionInventoryEntry::packageId);
        REQUIRE(changedPublisher != inventory.Entries().end());
        REQUIRE(changedPublisher->runtimeActive);
        REQUIRE_FALSE(changedPublisher->locallyTrusted);
        REQUIRE(changedPublisher->RestartRequired());
        REQUIRE(inventory.SetTrusted(changedPublisher->packageId, true).HasValue());

        const std::string versionField{"\"version\": \"1.0.0\""};
        const std::size_t packageVersion = manifestText.find(versionField);
        REQUIRE(packageVersion != std::string::npos);
        const std::size_t moduleVersion = manifestText.find(versionField, packageVersion + versionField.size());
        REQUIRE(moduleVersion != std::string::npos);
        manifestText.replace(moduleVersion, versionField.size(), "\"version\": \"1.1.0\"");
        {
            std::ofstream manifestOutput(installedManifest, std::ios::binary | std::ios::trunc);
            manifestOutput << manifestText;
        }
        REQUIRE(inventory.Refresh().HasValue());
        const auto updated = std::ranges::find(inventory.Entries(), installed.Value(), &ExtensionInventoryEntry::packageId);
        REQUIRE(updated != inventory.Entries().end());
        REQUIRE(updated->runtimeActive);
        REQUIRE_FALSE(updated->locallyTrusted);
        REQUIRE(inventory.EnabledUserPackageRoots().empty());
        REQUIRE(updated->RestartRequired());
        REQUIRE(inventory.InstallFromDirectory(source).HasError());
    }

    TEST_CASE("External importer registration conflict leaves the catalog candidate unchanged", "[Extensions][Assets]") {
        using namespace Horo::Assets;
        AssetImporterCatalog catalog;
        REQUIRE(catalog.Register(MakeExistingImporterContribution("com.horo.examples.asset-importer-basic.raw")).HasValue());

        ExtensionManager manager{&catalog, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
        auto loaded = manager.LoadExtension(fs::absolute(HORO_BASIC_EXTENSION_DIR).string());
        REQUIRE(loaded.HasError());
        REQUIRE(manager.GetLoadedExtensionIds().empty());

        auto published = catalog.Publish();
        REQUIRE(published.HasValue());
        const auto *retained = published.Value()->FindById("com.horo.examples.asset-importer-basic.raw");
        REQUIRE(retained != nullptr);
        REQUIRE(retained->packageId == "existing.package");
    }

    TEST_CASE("Failed activation preserves the exact published importer registry snapshot", "[Extensions][Assets]") {
        using namespace Horo::Assets;
        AssetImporterCatalog catalog;
        REQUIRE(catalog.Register(MakeExistingImporterContribution("com.example.existing.raw")).HasValue());
        auto published = catalog.Publish();
        REQUIRE(published.HasValue());
        const auto previous = published.Value();

        ExtensionManager manager{&catalog, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
        const auto loaded = manager.LoadExtension(fs::absolute(HORO_BASIC_EXTENSION_DIR).string());

        REQUIRE(loaded.HasError());
        CHECK(manager.GetLoadedExtensionIds().empty());
        CHECK(catalog.Snapshot() == previous);
        CHECK(catalog.Snapshot()->FindById("com.example.existing.raw") != nullptr);
        CHECK(catalog.Snapshot()->FindById("com.horo.examples.asset-importer-basic.raw") == nullptr);
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "A failed sibling cannot publish an earlier module contribution",
                     "[Extensions][Assets][Modules]") {
        const fs::path sourceRoot = fs::absolute(HORO_BASIC_EXTENSION_DIR);
        fs::path libraryPath;
        for (const fs::directory_entry &entry : fs::directory_iterator{sourceRoot}) {
            const std::string extension = entry.path().extension().string();
            if (extension == ".dll" || extension == ".dylib" || extension == ".so") {
                libraryPath = entry.path();
                break;
            }
        }
        REQUIRE_FALSE(libraryPath.empty());
        fs::copy_file(libraryPath, tempDir / libraryPath.filename(), fs::copy_options::overwrite_existing);

        std::ofstream manifest{tempDir / "extension.json", std::ios::binary | std::ios::trunc};
        manifest << R"json({
            "id":"com.horo.examples.asset-importer-basic",
            "version":"1.0.0",
            "modules":[{
                "id":"com.horo.examples.asset-importer-basic.native",
                "version":"1.0.0",
                "kind":"asset_importer",
                "roles":["backend-capability","headless-tooling"],
                "entry":")json"
                 << libraryPath.filename().generic_string() << R"json("
            },{
                "id":"com.horo.examples.asset-importer-basic.presentation",
                "version":"1.0.0",
                "kind":"native",
                "roles":["editor-presentation"],
                "dependencies":["com.horo.examples.asset-importer-basic.native"],
                "entry":"missing)json"
                 << libraryPath.extension().string() << R"json("
            }],
            "contributions":[{
                "type":"asset.importer",
                "id":"com.horo.examples.asset-importer-basic.raw",
                "module":"com.horo.examples.asset-importer-basic.native"
            }]
        })json";
        manifest.close();

        Assets::AssetImporterCatalog catalog;
        ExtensionManager manager{&catalog, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
        REQUIRE(manager.LoadExtension(fs::absolute(tempDir).string()).HasError());
        CHECK(manager.GetLoadedExtensionIds().empty());
        auto published = catalog.Publish();
        REQUIRE(published.HasValue());
        CHECK(published.Value()->FindById("com.horo.examples.asset-importer-basic.raw") == nullptr);
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension manager rejects incompatible artifacts before library loading",
                     "[Extensions][Compatibility]") {
#if defined(_WIN32)
        constexpr std::string_view OtherPlatform = "linux";
#else
        constexpr std::string_view OtherPlatform = "windows";
#endif
        std::ofstream manifest{tempDir / "extension.json", std::ios::binary | std::ios::trunc};
        manifest << R"json({
            "id":"com.example.incompatible",
            "version":"1.0.0",
            "modules":[{
                "id":"com.example.incompatible.native",
                "version":"1.0.0",
                "kind":"native",
                "roles":["backend-capability"],
                "abi":{"major":1,"minimumMinor":1},
                "entries":[{"platform":")json"
                 << OtherPlatform << R"json(","architecture":"x86_64","buildProfile":"debug","entry":"missing-library"}]
            }]
        })json";
        manifest.close();

        ExtensionManager manager;
        const auto loaded = manager.LoadExtension(fs::absolute(tempDir).string());
        REQUIRE(loaded.HasError());
        CHECK_THAT(loaded.ErrorValue().message, Catch::Matchers::ContainsSubstring("rejected compatibility requirement"));
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "Extension manager maps explicit host capabilities into compatibility admission",
                     "[Extensions][Compatibility]") {
        std::ofstream manifest{tempDir / "extension.json", std::ios::binary | std::ios::trunc};
        manifest << R"json({
            "id":"com.example.capability",
            "version":"1.0.0",
            "modules":[{
                "id":"com.example.capability.native",
                "version":"1.0.0",
                "kind":"native",
                "entry":"missing-library",
                "roles":["backend-capability"],
                "requiredCapabilities":["com.horo.assets"]
            }]
        })json";
        manifest.close();

        ExtensionManager withoutCapability;
        const auto rejected = withoutCapability.LoadExtension(fs::absolute(tempDir).string());
        REQUIRE(rejected.HasError());
        CHECK_THAT(rejected.ErrorValue().message, Catch::Matchers::ContainsSubstring("rejected compatibility requirement"));

        ExtensionManager withCapability{nullptr, ExtensionHostProfile::Interactive, {"com.horo.assets"}};
        const auto admitted = withCapability.LoadExtension(fs::absolute(tempDir).string());
        REQUIRE(admitted.HasError());
        CHECK_THAT(admitted.ErrorValue().message, !Catch::Matchers::ContainsSubstring("rejected compatibility requirement"));
    }

    TEST_CASE_METHOD(ExtensionManagerTestFixture, "External asset importer loads, previews, reimports, and retires held snapshots safely",
                     "[Extensions][Assets]") {
        using namespace Horo::Assets;

        AssetImporterCatalog catalog;
        ExtensionManager manager{&catalog, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
        const fs::path packagePath = fs::absolute(HORO_BASIC_EXTENSION_DIR);
        auto loaded = manager.LoadExtension(packagePath.string());
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value() == "com.horo.examples.asset-importer-basic");

        auto published = catalog.Publish();
        REQUIRE(published.HasValue());
        const auto snapshot = published.Value();
        const auto *contribution = snapshot->FindById("com.horo.examples.asset-importer-basic.raw");
        REQUIRE(contribution != nullptr);
        REQUIRE(contribution->packageId == "com.horo.examples.asset-importer-basic");
        REQUIRE(contribution->moduleId == "com.horo.examples.asset-importer-basic.native");
        REQUIRE(contribution->moduleVersion == "1.0.0");
        REQUIRE(contribution->version == "1.0.0");
        REQUIRE(contribution->settings.size() == 1);
        REQUIRE(contribution->settings[0].id == "invertPreview");
        REQUIRE(contribution->settings[0].includeInPresets);
        REQUIRE(contribution->previewProvider != nullptr);

        const std::vector<std::uint8_t> sourceBytes{7U, 8U, 9U};
        auto imported = contribution->strategy->Import(
            AssetImportInput{
                .sourceBytes = sourceBytes,
                .sourceExtension = "hraw",
                .settings = {false},
            },
            CancellationToken{});
        REQUIRE(imported.HasValue());
        REQUIRE(imported.Value().type.Value() == "example.raw");
        REQUIRE(imported.Value().editorPayload.size() == sourceBytes.size() + 5);

        auto preview = contribution->previewProvider->GeneratePreview(
            AssetPreviewInput{
                .editorPayload = imported.Value().editorPayload,
                .absoluteAssetPath = (tempDir / "preview.horoasset").string(),
                .assetType = imported.Value().type,
                .width = 16,
                .height = 12,
            },
            CancellationToken{});
        REQUIRE(preview.HasValue());
        REQUIRE(preview.Value().IsValid());

        const fs::path projectRoot = tempDir / "project";
        const fs::path sourcePath = projectRoot / "source.hraw";
        const fs::path assetPath = projectRoot / "assets" / "sample.horoasset";
        const fs::path sidecarPath = assetPath.string() + ".horo";
        fs::create_directories(assetPath.parent_path());
        {
            std::ofstream source(sourcePath, std::ios::binary);
            source << "old";
            std::ofstream asset(assetPath, std::ios::binary);
            asset << "old-payload";
        }
        auto oldSource = ReadAssetImportSource(fs::absolute(sourcePath));
        REQUIRE(oldSource.HasValue());
        const AssetId assetId = AssetId::Parse("11112222-3333-4444-8555-666677778888").Value();
        AssetImportMetadata metadata{
            .assetId = assetId,
            .assetType = AssetTypeId::Parse("example.raw").Value(),
            .importerContributionId = contribution->contributionId,
            .importerVersion = "0.9.0",
            .importerPackageId = contribution->packageId,
            .importerModuleId = contribution->moduleId,
            .importerModuleVersion = "0.9.0",
            .absoluteSourcePath = fs::absolute(sourcePath),
            .sourceExtension = "hraw",
            .sourceHash = HashAssetImportSource(oldSource.Value()),
            .sourceByteSize = oldSource.Value().size(),
            .importSettings = {{"settings.invertPreview", "false"}},
            .lastImportReasons = {AssetImportReason::InitialImport},
            .importedAtUtc = CurrentImportTimestampUtc(),
        };
        auto serialized = SerializeAssetImportMetadata(metadata);
        REQUIRE(serialized.HasValue());
        {
            std::ofstream sidecar(sidecarPath, std::ios::binary);
            sidecar << serialized.Value();
            std::ofstream changedSource(sourcePath, std::ios::binary | std::ios::trunc);
            changedSource << "changed";
        }

        AssetRegistry registry;
        REQUIRE(RebuildAssetRegistry(registry, fs::absolute(projectRoot), AssetRegistryOpenMode::Edit).HasValue());
        NativeDurableFileSystem files;
        auto reimported = ReimportProjectAsset(
            AssetReimportRequest{
                .absoluteProjectRoot = fs::absolute(projectRoot),
                .absoluteAssetPath = fs::absolute(assetPath),
                .importerCatalog = snapshot.get(),
                .registry = &registry,
                .files = &files,
            },
            CancellationToken{});
        REQUIRE(reimported.HasValue());
        REQUIRE(reimported.Value().assetId == assetId);
        REQUIRE(reimported.Value().reasons.size() == 3);
        REQUIRE(reimported.Value().reasons[0] == AssetImportReason::SourceChanged);
        REQUIRE(reimported.Value().reasons[1] == AssetImportReason::ImporterChanged);
        REQUIRE(reimported.Value().reasons[2] == AssetImportReason::ModuleChanged);

        const auto retirement = manager.RetireExtension(loaded.Value());
        REQUIRE(manager.GetLoadedExtensionIds().empty());
        REQUIRE(retirement.disposition == ExtensionRetirementDisposition::Draining);
        REQUIRE_FALSE(retirement.outstanding.empty());
        CHECK(retirement.outstanding.front().moduleId == "com.horo.examples.asset-importer-basic.native");
        REQUIRE(catalog.Snapshot() != nullptr);
        CHECK(catalog.Snapshot()->FindById(contribution->contributionId) == nullptr);
        const auto afterWithdrawal =
            contribution->strategy->Import(AssetImportInput{.sourceBytes = sourceBytes, .sourceExtension = "hraw", .settings = {false}},
                                           CancellationToken{});
        REQUIRE(afterWithdrawal.HasError());
        CHECK_THAT(afterWithdrawal.ErrorValue().message, Catch::Matchers::ContainsSubstring("retiring"));
        const auto refusedReplacement = manager.LoadExtension(packagePath.string());
        REQUIRE(refusedReplacement.HasError());
        manager.UnloadExtension(loaded.Value());
    }
#endif

}  // namespace Horo::Extensions::Tests
