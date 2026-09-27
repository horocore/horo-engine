#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "SecurityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>

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
    };

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
}  // namespace Horo::Extensions::Tests
