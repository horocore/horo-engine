#include "Horo/Assets/AssetImporter.h"
#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "SecurityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <string_view>
#include <thread>
#include <vector>

namespace Horo::Extensions::Tests {
    struct AbiIntegrationFixture {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo-abi-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        AbiIntegrationFixture() {
            REQUIRE(std::filesystem::create_directory(root));
        }

        ~AbiIntegrationFixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        /** @brief Stage a real library and its importer declaration in a private package. */
        std::filesystem::path Stage(const std::filesystem::path &fixturePath) const {
            const auto packageRoot = root / "native package with spaces";
            REQUIRE(std::filesystem::create_directory(packageRoot));
            const auto libraryPath = packageRoot / fixturePath.filename();
            std::filesystem::copy_file(fixturePath, libraryPath);
            WriteManifest(packageRoot, libraryPath.filename());
            return libraryPath;
        }

        /** @brief Rewrite only the selected entry to exercise path admission before native loading. */
        static void WriteManifest(const std::filesystem::path &packageRoot, const std::filesystem::path &entry) {
            std::ofstream manifest(packageRoot / "extension.json");
            manifest
                << R"({"id":"com.example.abi","version":"1.0.0","modules":[{"id":"com.example.abi.native","version":"1.0.0","kind":"asset_importer","roles":["backend-capability"],"entry":)"
                << std::quoted(entry.generic_string())
                << R"(}],"contributions":[{"type":"asset.importer","id":"fixture.importer","module":"com.example.abi.native"}]})";
            REQUIRE(manifest.good());
        }
    };

    /** @brief Read counters through the real shared-library boundary without borrowing host state. */
    static std::uint32_t Counter(const Platform::DynamicLibrary &library, const std::string_view symbol) {
        const auto count = reinterpret_cast<std::uint32_t (*)()>(library.GetSymbol(symbol));  // NOSONAR(cpp:S3630)
        REQUIRE(count != nullptr);
        return count();
    }

    struct NativeReleaseAudit final {
        std::vector<std::string> packages;
        std::vector<std::thread::id> lanes;
    };

    /** @brief Observe release of the host's native lease independently of the test's counter observer. */
    class TrackedLibrary final : public Platform::DynamicLibrary {
    public:
        TrackedLibrary(std::unique_ptr<Platform::DynamicLibrary> library, std::uint32_t &releases,
                       std::shared_ptr<NativeReleaseAudit> audit = {}, std::string package = {})
            : library_(std::move(library)), releases_(releases), audit_(std::move(audit)), package_(std::move(package)) {}

        ~TrackedLibrary() override {
            library_.reset();
            ++releases_;
            if (audit_) {
                audit_->packages.push_back(std::move(package_));
                audit_->lanes.push_back(std::this_thread::get_id());
            }
        }

        void *GetSymbol(const std::string_view name) const noexcept override {
            return library_->GetSymbol(name);
        }

    private:
        std::unique_ptr<Platform::DynamicLibrary> library_;
        std::uint32_t &releases_;
        std::shared_ptr<NativeReleaseAudit> audit_;
        std::string package_;
    };

    /** @brief Use the platform loader while recording host lease retirement. */
    static ExtensionManager::NativeLibraryLoader TrackReleases(std::uint32_t &releases, std::shared_ptr<NativeReleaseAudit> audit = {}) {
        return [&releases, audit = std::move(audit)](const std::string &path) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
            auto loaded = Platform::LoadDynamicLibrary(path);
            if (loaded.HasError())
                return loaded;
            return Result<std::unique_ptr<Platform::DynamicLibrary>>::Success(
                std::make_unique<TrackedLibrary>(std::move(loaded).Value(), releases, audit,
                                                 std::filesystem::path{path}.parent_path().filename().string()));
        };
    }

    /** @brief Stages a real noncontributing native module with package-specific identity. */
    static std::filesystem::path StageNativePackage(const std::filesystem::path &root, const std::string &id) {
        const auto package = root / id;
        REQUIRE(std::filesystem::create_directory(package));
        const std::filesystem::path source{HORO_ABI_FIXTURE_0};
        std::filesystem::copy_file(source, package / source.filename());
        std::ofstream manifest{package / "extension.json"};
        manifest << "{\"id\":" << std::quoted(id) << ",\"version\":\"1.0.0\",\"modules\":[{\"id\":" << std::quoted(id + ".native")
                 << ",\"version\":\"1.0.0\",\"kind\":\"native\",\"roles\":[\"backend-capability\"],\"entry\":"
                 << std::quoted(source.filename().generic_string()) << "}]}";
        REQUIRE(manifest.good());
        return package;
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native package retirement pins provider code and finalizes dependents on the owner lane",
                     "[Extensions][ABI][Retirement]") {
        const auto provider = StageNativePackage(root, "com.example.provider");
        const auto dependent = StageNativePackage(root, "com.example.dependent");
        auto audit = std::make_shared<NativeReleaseAudit>();
        audit->packages.reserve(2);
        audit->lanes.reserve(2);
        std::uint32_t releases{};
        auto loader = TrackReleases(releases, audit);
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 std::move(loader)};
        REQUIRE(manager.LoadExtension(provider.string()).HasValue());
        const std::array<std::string, 1> providers{"com.example.provider"};
        REQUIRE(manager.LoadExtension(dependent.string(), providers).HasValue());
        auto retirement = manager.Retirement("com.example.dependent");
        REQUIRE(retirement);
        auto work = retirement->Acquire("com.example.dependent.native", ExtensionLeaseKind::Job, "native-job", std::make_shared<int>(0));
        REQUIRE(work);
        const auto providerReport = manager.RetireExtension("com.example.provider");
        CHECK(manager.GetLoadedExtensionIds().empty());
        CHECK(retirement->Inspect().disposition == ExtensionRetirementDisposition::Draining);
        CHECK(releases == 0);
        auto released = std::async(std::launch::async, [work = std::move(work)]() mutable {
            work.reset();
        });
        released.get();
        CHECK(releases == 0);
        manager.FinalizeRetirements();
        CHECK(releases == 2);
        CHECK(audit->packages == std::vector<std::string>{"com.example.dependent", "com.example.provider"});
        CHECK(audit->lanes == std::vector<std::thread::id>{std::this_thread::get_id(), std::this_thread::get_id()});
        CHECK(retirement->IsDrained());
        REQUIRE(providerReport.outstanding.size() == 1);
        CHECK(providerReport.disposition == ExtensionRetirementDisposition::Draining);
        CHECK(providerReport.outstanding.front().subject == "dependent-package:com.example.dependent");
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native C ABI negotiation gates actual dynamic module activation", "[Extensions][ABI]") {
        const std::array paths{HORO_ABI_FIXTURE_0, HORO_ABI_FIXTURE_1, HORO_ABI_FIXTURE_2, HORO_ABI_FIXTURE_3, HORO_ABI_FIXTURE_9};
        for (std::size_t mode = 0; mode < paths.size(); ++mode) {
            const auto packageRoot = root / std::to_string(mode);
            REQUIRE(std::filesystem::create_directory(packageRoot));
            const auto libraryPath = packageRoot / std::filesystem::path(paths[mode]).filename();
            std::filesystem::copy_file(paths[mode], libraryPath);
            {
                std::ofstream manifest(packageRoot / "extension.json");
                manifest
                    << R"({"id":"com.example.abi","version":"1.0.0","modules":[{"id":"com.example.abi.native","version":"1.0.0","kind":"asset_importer","roles":["backend-capability"],"entry":")"
                    << libraryPath.filename().generic_string() << R"("}]})";
                REQUIRE(manifest.good());
            }
            auto loaded = Platform::LoadDynamicLibrary(libraryPath.string());
            REQUIRE(loaded.HasValue());
            const auto count =
                reinterpret_cast<std::uint32_t (*)()>(loaded.Value()->GetSymbol("horo_test_load_count"));  // NOSONAR(cpp:S3630)
            REQUIRE(count != nullptr);
            REQUIRE(count() == 0);
            ExtensionManager manager{nullptr, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
            const auto result = manager.LoadExtension(packageRoot.string());
            CHECK(result.HasValue() == (mode < 2 || mode == 4));
            CHECK(manager.GetLoadedExtensionIds().size() == ((mode < 2 || mode == 4) ? 1 : 0));
            CHECK(count() == (mode == 2 ? 0 : 1));
        }
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native host reports exact errors for missing symbols incompatible ABI and oversized identity",
                     "[Extensions][ABI][Hostile]") {
        const std::array paths{HORO_ABI_MISSING_LOAD_FIXTURE, HORO_ABI_FIXTURE_2, HORO_ABI_FIXTURE_3, HORO_ABI_FIXTURE_7};
        const std::array codes{ExtensionErrors::MissingEntryPoint.code, ExtensionErrors::LoadFailed.code,
                               ExtensionErrors::InvalidManifest.code, ExtensionErrors::InvalidManifest.code};
        for (std::size_t index = 0; index < paths.size(); ++index) {
            CAPTURE(index);
            AbiIntegrationFixture package;
            const auto libraryPath = package.Stage(paths[index]);
            std::uint32_t releases{};
            ExtensionManager manager{nullptr,
                                     ExtensionHostProfile::Interactive,
                                     {},
                                     Horo::Tests::CreateAcceptingArtifactGate(),
                                     TrackReleases(releases)};
            const auto result = manager.LoadExtension(libraryPath.parent_path().string());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == codes[index].Value());
            CHECK(manager.GetLoadedExtensionIds().empty());
            CHECK(releases == 1);
        }
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native hostile activation rolls back callbacks and preserves stable errors",
                     "[Extensions][ABI][Hostile]") {
        const std::array paths{HORO_ABI_HOSTILE_FIXTURE_1, HORO_ABI_HOSTILE_FIXTURE_2, HORO_ABI_HOSTILE_FIXTURE_3,
                               HORO_ABI_HOSTILE_FIXTURE_6};
        const std::array codes{ExtensionErrors::LoadFailed.code, ExtensionErrors::LoadFailed.code, ExtensionErrors::InvalidManifest.code,
                               ExtensionErrors::LoadFailed.code};
        for (std::size_t mode = 0; mode < paths.size(); ++mode) {
            CAPTURE(mode);
            AbiIntegrationFixture package;
            const auto libraryPath = package.Stage(paths[mode]);
            auto observer = Platform::LoadDynamicLibrary(libraryPath.string());
            REQUIRE(observer.HasValue());
            std::uint32_t releases{};
            Assets::AssetImporterCatalog catalog;
            ExtensionManager manager{&catalog,
                                     ExtensionHostProfile::Interactive,
                                     {},
                                     Horo::Tests::CreateAcceptingArtifactGate(),
                                     TrackReleases(releases)};
            const auto result = manager.LoadExtension(libraryPath.parent_path().string());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == codes[mode].Value());
            CHECK(manager.GetLoadedExtensionIds().empty());
            CHECK(releases == (mode == 3 ? 0U : 1U));
            CHECK(Counter(*observer.Value(), "horo_test_load_count") == (mode == 0 ? 0 : 1));
            CHECK(Counter(*observer.Value(), "horo_test_destroy_count") == (mode == 0 ? 0 : 1));
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == (mode == 0 ? 0 : 1));
            if (mode == 3) {
                REQUIRE(result.ErrorValue().diagnostics.size() == 1);
                CHECK(result.ErrorValue().diagnostics.front().code.Value() == "extension.activation.rollback_cleanup_failed");
            }
            auto published = catalog.Publish();
            REQUIRE(published.HasValue());
            CHECK(published.Value()->FindById("fixture.importer") == nullptr);
            manager.UnloadAll();
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == (mode == 0 ? 0 : 1));
        }
    }

    /** @brief Loads the native fixture and publishes its real importer through the existing catalog contract. */
    static std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> LoadPublishedImporter(ExtensionManager &manager,
                                                                                             Assets::AssetImporterCatalog &catalog,
                                                                                             const std::filesystem::path &packageRoot) {
        REQUIRE(manager.LoadExtension(packageRoot.string()).HasValue());
        auto published = catalog.Publish();
        REQUIRE(published.HasValue());
        return std::move(published).Value();
    }

    /** @brief Verifies hostile callback containment before terminal retirement closes executable admission. */
    static void VerifyHostileImport(const Assets::AssetImporterCatalogSnapshot &snapshot, const Platform::DynamicLibrary &observer,
                                    const std::size_t mode) {
        const auto imported = snapshot.FindByExtension("raw")->Import(Assets::AssetImportInput{.sourceExtension = "raw"}, {});
        if (mode == 1 || mode == 3) {
            REQUIRE(imported.HasError());
            CHECK(imported.ErrorValue().code.Value() == ExtensionErrors::InvocationFailed.code.Value());
            if (mode == 1)
                CHECK(Counter(observer, "horo_test_output_status") == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
        } else {
            REQUIRE(imported.HasValue());
            CHECK(imported.Value().editorPayload == std::vector<std::uint8_t>{42});
        }
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native importer leases drain before owner-lane finalization", "[Extensions][ABI][Hostile]") {
        const std::array paths{HORO_ABI_HOSTILE_FIXTURE_0, HORO_ABI_HOSTILE_FIXTURE_4, HORO_ABI_HOSTILE_FIXTURE_5,
                               HORO_ABI_HOSTILE_FIXTURE_7};
        for (std::size_t mode = 0; mode < paths.size(); ++mode) {
            CAPTURE(mode);
            AbiIntegrationFixture package;
            const auto libraryPath = package.Stage(paths[mode]);
            auto observer = Platform::LoadDynamicLibrary(libraryPath.string());
            REQUIRE(observer.HasValue());
            std::uint32_t releases{};
            std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> snapshot;
            Assets::AssetImporterCatalog catalog;
            ExtensionManager manager{&catalog,
                                     ExtensionHostProfile::Interactive,
                                     {},
                                     Horo::Tests::CreateAcceptingArtifactGate(),
                                     TrackReleases(releases)};
            snapshot = LoadPublishedImporter(manager, catalog, libraryPath.parent_path());
            REQUIRE(snapshot->FindByExtension("raw") != nullptr);
            CHECK(releases == 0);
            VerifyHostileImport(*snapshot, *observer.Value(), mode);
            CHECK(Counter(*observer.Value(), "horo_test_destroy_count") == 0);
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 0);
            manager.UnloadAll();
            CHECK(manager.GetLoadedExtensionIds().empty());
            const auto withdrawn = catalog.Snapshot();
            REQUIRE(withdrawn);
            CHECK(withdrawn->FindById("fixture.importer") == nullptr);
            const auto rejected = snapshot->FindByExtension("raw")->Import(Assets::AssetImportInput{.sourceExtension = "raw"}, {});
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == ExtensionErrors::InvocationFailed.code.Value());
            auto released = std::async(std::launch::async, [snapshot = std::move(snapshot)]() mutable {
                snapshot.reset();
            });
            released.get();
            CHECK(Counter(*observer.Value(), "horo_test_destroy_count") == 1);
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 0);
            CHECK(releases == 0);
            manager.FinalizeRetirements();
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 1);
            CHECK(releases == (mode == 2 ? 0U : 1U));
            if (mode == 2) {
                const auto failed = manager.Retirement("com.example.abi");
                REQUIRE(failed);
                CHECK(failed->Inspect().disposition == ExtensionRetirementDisposition::RestartRequired);
            }
        }
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Manager destruction withdraws native importer publications and requires restart for held work",
                     "[Extensions][ABI][Retirement]") {
        const auto libraryPath = Stage(HORO_ABI_HOSTILE_FIXTURE_0);
        auto observer = Platform::LoadDynamicLibrary(libraryPath.string());
        REQUIRE(observer.HasValue());
        Assets::AssetImporterCatalog catalog;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> snapshot;
        std::shared_ptr<ExtensionRetirement> retirement;
        {
            ExtensionManager manager{&catalog, ExtensionHostProfile::Interactive, {}, Horo::Tests::CreateAcceptingArtifactGate()};
            REQUIRE(manager.LoadExtension(libraryPath.parent_path().string()).HasValue());
            retirement = manager.Retirement("com.example.abi");
            REQUIRE(retirement);
            auto published = catalog.Publish();
            REQUIRE(published.HasValue());
            snapshot = std::move(published).Value();
        }
        const auto withdrawn = catalog.Snapshot();
        REQUIRE(withdrawn);
        CHECK(withdrawn->FindById("fixture.importer") == nullptr);
        CHECK(retirement->Inspect().disposition == ExtensionRetirementDisposition::RestartRequired);
        const auto rejected = snapshot->FindByExtension("raw")->Import(Assets::AssetImportInput{.sourceExtension = "raw"}, {});
        REQUIRE(rejected.HasError());
        CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 0);
        snapshot.reset();
        CHECK(Counter(*observer.Value(), "horo_test_destroy_count") == 1);
        // Destruction with outstanding work is terminal retention, not deferred live unload.
        CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 0);
        CHECK(retirement->Inspect().disposition == ExtensionRetirementDisposition::RestartRequired);
    }

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native host rejects unsafe entries before calling the library loader",
                     "[Extensions][ABI][Hostile]") {
        const auto libraryPath = Stage(HORO_ABI_HOSTILE_FIXTURE_0);
        std::uint32_t loaderCalls{};
        ExtensionManager manager{nullptr,
                                 ExtensionHostProfile::Interactive,
                                 {},
                                 Horo::Tests::CreateAcceptingArtifactGate(),
                                 [&loaderCalls](const std::string &path) {
            ++loaderCalls;
            return Platform::LoadDynamicLibrary(path);
        }};
        const std::array entries{std::filesystem::path{"../outside.so"}, libraryPath};
        for (const auto &entry : entries) {
            WriteManifest(libraryPath.parent_path(), entry);
            const auto result = manager.LoadExtension(libraryPath.parent_path().string());
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == ExtensionErrors::InvalidManifest.code.Value());
            CHECK(manager.GetLoadedExtensionIds().empty());
            CHECK(loaderCalls == 0);
        }
#if !defined(_WIN32)
        // Windows symlink creation requires privileges absent from some CI runners.
        const auto linkPath = libraryPath.parent_path() / "linked-module.so";
        std::filesystem::create_symlink(libraryPath, linkPath);
        WriteManifest(libraryPath.parent_path(), linkPath.filename());
        const auto linked = manager.LoadExtension(libraryPath.parent_path().string());
        REQUIRE(linked.HasError());
        CHECK(linked.ErrorValue().code.Value() == ExtensionErrors::InvalidManifest.code.Value());
        CHECK(manager.GetLoadedExtensionIds().empty());
        CHECK(loaderCalls == 0);
#endif
    }
}  // namespace Horo::Extensions::Tests
