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
#include <iomanip>
#include <string_view>

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

    /** @brief Observe release of the host's native lease independently of the test's counter observer. */
    class TrackedLibrary final : public Platform::DynamicLibrary {
    public:
        TrackedLibrary(std::unique_ptr<Platform::DynamicLibrary> library, std::uint32_t &releases)
            : library_(std::move(library)), releases_(releases) {}

        ~TrackedLibrary() override {
            library_.reset();
            ++releases_;
        }

        void *GetSymbol(const std::string_view name) const noexcept override {
            return library_->GetSymbol(name);
        }

    private:
        std::unique_ptr<Platform::DynamicLibrary> library_;
        std::uint32_t &releases_;
    };

    /** @brief Use the platform loader while recording host lease retirement. */
    static ExtensionManager::NativeLibraryLoader TrackReleases(std::uint32_t &releases) {
        return [&releases](const std::string &path) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
            auto loaded = Platform::LoadDynamicLibrary(path);
            if (loaded.HasError())
                return loaded;
            return Result<std::unique_ptr<Platform::DynamicLibrary>>::Success(
                std::make_unique<TrackedLibrary>(std::move(loaded).Value(), releases));
        };
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
            CHECK(releases == 1);
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

    TEST_CASE_METHOD(AbiIntegrationFixture, "Native importer leases survive host shutdown and contain hostile callbacks",
                     "[Extensions][ABI][Hostile]") {
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
            {
                Assets::AssetImporterCatalog catalog;
                ExtensionManager manager{&catalog,
                                         ExtensionHostProfile::Interactive,
                                         {},
                                         Horo::Tests::CreateAcceptingArtifactGate(),
                                         TrackReleases(releases)};
                REQUIRE(manager.LoadExtension(libraryPath.parent_path().string()).HasValue());
                auto published = catalog.Publish();
                REQUIRE(published.HasValue());
                snapshot = std::move(published).Value();
                manager.UnloadAll();
                CHECK(manager.GetLoadedExtensionIds().empty());
                CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 0);
            }
            REQUIRE(snapshot->FindByExtension("raw") != nullptr);
            CHECK(releases == 0);
            const auto imported = snapshot->FindByExtension("raw")->Import(Assets::AssetImportInput{.sourceExtension = "raw"}, {});
            if (mode == 1 || mode == 3) {
                REQUIRE(imported.HasError());
                CHECK(imported.ErrorValue().code.Value() == ExtensionErrors::InvocationFailed.code.Value());
                if (mode == 1)
                    CHECK(Counter(*observer.Value(), "horo_test_output_status") == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
            } else {
                REQUIRE(imported.HasValue());
                CHECK(imported.Value().editorPayload == std::vector<std::uint8_t>{42});
            }
            CHECK(Counter(*observer.Value(), "horo_test_destroy_count") == 0);
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 0);
            snapshot.reset();
            CHECK(Counter(*observer.Value(), "horo_test_destroy_count") == 1);
            CHECK(Counter(*observer.Value(), "horo_test_unload_count") == 1);
            CHECK(releases == 1);
        }
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
