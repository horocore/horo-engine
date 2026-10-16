#include "Horo/Extensions/ExtensionModuleResolution.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
#include <utility>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] ExtensionModuleManifest Module(std::string id, const ExtensionModuleRole role) {
            return ExtensionModuleManifest{
                .id = std::move(id),
                .version = "1.0.0",
                .kind = "native",
                .roles = {role},
            };
        }

        void RequireResolutionError(const Result<ExtensionModulePlan> &result, const std::string_view expected) {
            REQUIRE(result.HasError());
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(std::string{expected}));
        }

        [[nodiscard]] ExtensionHostEnvironment Host(const ExtensionHostProfile profile = ExtensionHostProfile::Interactive) {
            static constexpr std::array<std::string_view, 2> Capabilities{"com.horo.assets", "com.horo.logging"};
            return {
                .profile = profile,
                .platform = ExtensionHostPlatform::Linux,
                .architecture = ExtensionHostArchitecture::X86_64,
                .buildProfile = ExtensionBuildProfile::Debug,
                .engineVersion = "0.1.0",
                .abiMajor = 1,
                .abiMinor = 1,
                .capabilities = Capabilities,
            };
        }

        struct CompatibilityFixture {
            ExtensionManifest manifest;
            ExtensionModuleManifest module;
        };

        [[nodiscard]] CompatibilityFixture CompatibleNativeModule() {
            CompatibilityFixture fixture;
            fixture.manifest.engineMin = "0.1.0";
            fixture.manifest.engineMax = "0.2.0";
            fixture.module = Module("com.example.backend", ExtensionModuleRole::BackendCapability);
            fixture.module.abi = ExtensionAbiRequirement{.major = 1, .minimumMinor = 1};
            fixture.module.requiredCapabilities = {"com.horo.assets"};
            fixture.module.entries = {
                {.platform = ExtensionHostPlatform::Linux,
                 .architecture = ExtensionHostArchitecture::X86_64,
                 .buildProfile = ExtensionBuildProfile::Debug,
                 .entry = "bin/linux-x86_64-debug/module"},
                {.platform = ExtensionHostPlatform::MacOS,
                 .architecture = ExtensionHostArchitecture::X86_64,
                 .buildProfile = ExtensionBuildProfile::Debug,
                 .entry = "bin/macos-x86_64-debug/module"},
                {.platform = ExtensionHostPlatform::Windows,
                 .architecture = ExtensionHostArchitecture::X86_64,
                 .buildProfile = ExtensionBuildProfile::Debug,
                 .entry = "bin/windows-x86_64-debug/module"},
            };
            return fixture;
        }
    }  // namespace

    TEST_CASE("Extension module resolution is deterministic and binds service ownership", "[Extensions][Resolution]") {
        ExtensionModuleManifest backend = Module("com.example.backend", ExtensionModuleRole::BackendCapability);
        backend.exports.push_back({.id = "com.example.service", .contract = "com.horo.example", .version = "2.1.0"});
        ExtensionModuleManifest scripting = Module("com.example.scripting", ExtensionModuleRole::ScriptProvider);
        scripting.imports.push_back(
            {.id = "com.example.import", .service = "com.example.service", .contract = "com.horo.example", .minimumVersion = "2.0.0"});
        ExtensionModuleManifest editor = Module("com.example.editor", ExtensionModuleRole::EditorPresentation);
        editor.dependencies.push_back("com.example.backend");

        ExtensionManifest manifest;
        manifest.id = "com.example.package";
        manifest.modules = {editor, scripting, backend};
        manifest.contributions = {{.type = "editor.panel", .id = "com.example.panel", .owningModule = editor.id},
                                  {.type = "application.service", .id = "com.example.backend-service", .owningModule = backend.id}};

        const auto interactive = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive));
        REQUIRE(interactive.HasValue());
        CHECK(interactive.Value().moduleIds ==
              std::vector<std::string>{"com.example.backend", "com.example.editor", "com.example.scripting"});
        CHECK(interactive.Value().contributions.size() == 2);
        REQUIRE(interactive.Value().serviceImports.size() == 1);
        CHECK(interactive.Value().serviceImports.front().Status() == ExtensionServiceImportStatus::Bound);
        CHECK(interactive.Value().serviceImports.front().ConsumerExtensionId() == manifest.id);
        CHECK(interactive.Value().serviceImports.front().ConsumerModuleId() == scripting.id);
        CHECK(interactive.Value().serviceImports.front().ProviderModuleId() == backend.id);
        CHECK(interactive.Value().serviceImports.front().ProviderVersion() == "2.1.0");

        std::ranges::reverse(manifest.modules);
        const auto reordered = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive));
        REQUIRE(reordered.HasValue());
        CHECK(reordered.Value().moduleIds == interactive.Value().moduleIds);
        CHECK(reordered.Value().serviceImports.front().ProviderModuleId() == interactive.Value().serviceImports.front().ProviderModuleId());

        const auto headless = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Headless));
        REQUIRE(headless.HasValue());
        CHECK(headless.Value().moduleIds == std::vector<std::string>{"com.example.backend", "com.example.scripting"});
        REQUIRE(headless.Value().contributions.size() == 1);
        CHECK(headless.Value().contributions.front().owningModule == backend.id);
    }

    TEST_CASE("Extension module resolution records optional import failures without load-order fallback", "[Extensions][Resolution]") {
        ExtensionModuleManifest provider = Module("com.example.provider", ExtensionModuleRole::BackendCapability);
        provider.exports.push_back({.id = "com.example.service", .contract = "com.example.contract", .version = "1.4.0"});
        ExtensionModuleManifest consumer = Module("com.example.consumer", ExtensionModuleRole::BackendCapability);
        consumer.imports.push_back({.id = "com.example.missing-import",
                                    .service = "com.example.missing",
                                    .contract = "com.example.contract",
                                    .minimumVersion = "1.0.0",
                                    .required = false});
        consumer.imports.push_back({.id = "com.example.incompatible-import",
                                    .service = "com.example.service",
                                    .contract = "com.example.contract",
                                    .minimumVersion = "2.0.0",
                                    .required = false});
        ExtensionManifest manifest;
        manifest.modules = {consumer, provider};

        const auto resolved = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Headless));
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value().serviceImports.size() == 2);
        CHECK(resolved.Value().serviceImports[0].Status() == ExtensionServiceImportStatus::Incompatible);
        CHECK(resolved.Value().serviceImports[0].ProviderModuleId() == provider.id);
        CHECK(resolved.Value().serviceImports[1].Status() == ExtensionServiceImportStatus::Unavailable);
        CHECK(resolved.Value().serviceImports[1].ProviderModuleId().empty());
    }

    TEST_CASE("Headless resolution excludes presentation modules without promoting optional imports to required dependencies",
              "[Extensions][Resolution][Headless]") {
        ExtensionModuleManifest presentation = Module("com.example.presentation", ExtensionModuleRole::EditorPresentation);
        ExtensionModuleManifest tooling = Module("com.example.tooling", ExtensionModuleRole::HeadlessTooling);
        tooling.imports.push_back({.id = "com.example.presentation.import",
                                   .service = "com.example.presentation.service",
                                   .contract = "com.example.presentation.contract",
                                   .minimumVersion = "1.0.0",
                                   .required = false});
        ExtensionManifest manifest;
        manifest.id = "com.example.optional-presentation";
        manifest.modules = {presentation, tooling};

        const auto resolved = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Headless));
        REQUIRE(resolved.HasValue());
        CHECK(resolved.Value().moduleIds == std::vector<std::string>{tooling.id});
        REQUIRE(resolved.Value().serviceImports.size() == 1);
        CHECK(resolved.Value().serviceImports.front().Status() == ExtensionServiceImportStatus::Unavailable);
        CHECK_FALSE(resolved.Value().serviceImports.front().IsRequired());
    }

    TEST_CASE("Extension module resolution reports complete graph failures", "[Extensions][Resolution]") {
        SECTION("duplicate service exports identify both owners") {
            ExtensionModuleManifest first = Module("com.example.first", ExtensionModuleRole::BackendCapability);
            ExtensionModuleManifest second = Module("com.example.second", ExtensionModuleRole::BackendCapability);
            first.exports.push_back({.id = "com.example.service", .contract = "com.example.contract", .version = "1.0.0"});
            second.exports = first.exports;
            ExtensionManifest manifest;
            manifest.modules = {second, first};
            const auto result = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive));
            RequireResolutionError(result, "Duplicate service export");
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(first.id));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(second.id));
            std::ranges::reverse(manifest.modules);
            const auto reordered = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive));
            RequireResolutionError(reordered, "Duplicate service export");
            CHECK(reordered.ErrorValue().message == result.ErrorValue().message);
        }

        SECTION("missing and incompatible imports identify consumers and providers") {
            ExtensionModuleManifest provider = Module("com.example.provider", ExtensionModuleRole::BackendCapability);
            provider.exports.push_back({.id = "com.example.service", .contract = "com.example.contract", .version = "1.4.0"});
            ExtensionModuleManifest consumer = Module("com.example.consumer", ExtensionModuleRole::ScriptProvider);
            consumer.imports.push_back({.id = "com.example.import",
                                        .service = "com.example.missing",
                                        .contract = "com.example.contract",
                                        .minimumVersion = "1.0.0"});
            ExtensionManifest manifest;
            manifest.modules = {provider, consumer};
            RequireResolutionError(ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive)), "Missing service export");

            consumer.imports.front().service = "com.example.service";
            consumer.imports.front().minimumVersion = "2.0.0";
            manifest.modules = {provider, consumer};
            const auto result = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive));
            RequireResolutionError(result, "Incompatible service export");
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(provider.id));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(consumer.id));

            provider.exports.front().version = "999999999999999999999999999999.0.0";
            consumer.imports.front().minimumVersion = "1.0.0";
            manifest.modules = {provider, consumer};
            RequireResolutionError(ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive)),
                                   "Incompatible service export");
        }

        SECTION("cycles identify every unresolved participant") {
            ExtensionModuleManifest first = Module("com.example.first", ExtensionModuleRole::BackendCapability);
            ExtensionModuleManifest second = Module("com.example.second", ExtensionModuleRole::BackendCapability);
            ExtensionModuleManifest third = Module("com.example.third", ExtensionModuleRole::BackendCapability);
            first.dependencies = {second.id};
            second.dependencies = {third.id};
            third.dependencies = {first.id};
            ExtensionManifest manifest;
            manifest.modules = {third, first, second};
            const auto result = ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Interactive));
            RequireResolutionError(result, "contains a cycle");
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(first.id));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(second.id));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring(third.id));
        }

        SECTION("headless backends cannot depend on omitted presentation modules") {
            ExtensionModuleManifest backend = Module("com.example.backend", ExtensionModuleRole::BackendCapability);
            ExtensionModuleManifest editor = Module("com.example.editor", ExtensionModuleRole::EditorPresentation);
            backend.dependencies = {editor.id};
            ExtensionManifest manifest;
            manifest.modules = {backend, editor};
            RequireResolutionError(ResolveExtensionModules(manifest, Host(ExtensionHostProfile::Headless)), "excluded presentation-only");
        }
    }

    TEST_CASE("Extension module compatibility selects only an exact admitted native entry", "[Extensions][Resolution][Compatibility]") {
        auto [manifest, module] = CompatibleNativeModule();

        struct PlatformCase {
            ExtensionHostPlatform platform;
            std::string_view expectedEntry;
        };

        constexpr std::array PlatformCases{
            PlatformCase{ExtensionHostPlatform::Windows, "bin/windows-x86_64-debug/module"},
            PlatformCase{ExtensionHostPlatform::MacOS, "bin/macos-x86_64-debug/module"},
            PlatformCase{ExtensionHostPlatform::Linux, "bin/linux-x86_64-debug/module"},
        };
        for (const auto &[platform, expectedEntry] : PlatformCases) {
            auto host = Host();
            host.platform = platform;
            const auto accepted = EvaluateExtensionModuleCompatibility(manifest, module, host);
            REQUIRE(accepted.IsCompatible());
            CHECK(accepted.selectedEntry == expectedEntry);
        }
    }

    TEST_CASE("Extension module compatibility rejects unsupported native entry selectors", "[Extensions][Resolution][Compatibility]") {
        auto [manifest, module] = CompatibleNativeModule();
        auto host = Host();
        std::erase_if(module.entries, [](const ExtensionNativeEntryManifest &entry) {
            return entry.platform == ExtensionHostPlatform::MacOS;
        });
        host.platform = ExtensionHostPlatform::MacOS;
        CHECK(EvaluateExtensionModuleCompatibility(manifest, module, host).status == ExtensionCompatibilityStatus::HostPlatformUnsupported);
        host = Host();
        host.architecture = ExtensionHostArchitecture::Arm64;
        CHECK(EvaluateExtensionModuleCompatibility(manifest, module, host).status ==
              ExtensionCompatibilityStatus::HostArchitectureUnsupported);
        host = Host();
        host.buildProfile = ExtensionBuildProfile::Release;
        CHECK(EvaluateExtensionModuleCompatibility(manifest, module, host).status == ExtensionCompatibilityStatus::BuildProfileUnsupported);
    }

    TEST_CASE("Extension module compatibility fails closed for engine ABI and capabilities", "[Extensions][Resolution][Compatibility]") {
        ExtensionManifest manifest;
        manifest.engineMin = "0.2.0";
        ExtensionModuleManifest module = Module("com.example.backend", ExtensionModuleRole::BackendCapability);
        module.abi = ExtensionAbiRequirement{.major = 1, .minimumMinor = 2};
        module.requiredCapabilities = {"com.horo.unavailable"};

        CHECK(EvaluateExtensionModuleCompatibility(manifest, module, Host()).status ==
              ExtensionCompatibilityStatus::EngineVersionUnsupported);
        manifest.engineMin = "0.1.0";
        CHECK(EvaluateExtensionModuleCompatibility(manifest, module, Host()).status == ExtensionCompatibilityStatus::AbiUnsupported);
        module.abi->minimumMinor = 1;
        CHECK(EvaluateExtensionModuleCompatibility(manifest, module, Host()).status == ExtensionCompatibilityStatus::CapabilityUnavailable);
    }

    TEST_CASE("Extension module resolution rejects competing or duplicate entry authority", "[Extensions][Resolution][Compatibility]") {
        ExtensionManifest manifest;
        ExtensionModuleManifest module = Module("com.example.backend", ExtensionModuleRole::BackendCapability);
        module.entry = "legacy-module";
        module.entries = {{.platform = ExtensionHostPlatform::Linux,
                           .architecture = ExtensionHostArchitecture::X86_64,
                           .buildProfile = ExtensionBuildProfile::Debug,
                           .entry = "typed-module"}};
        manifest.modules = {module};
        RequireResolutionError(ResolveExtensionModules(manifest, Host()), "cannot compete");

        module.entry.clear();
        module.entries.push_back(module.entries.front());
        manifest.modules = {module};
        RequireResolutionError(ResolveExtensionModules(manifest, Host()), "selector is duplicated");

        module.entries.pop_back();
        manifest.platforms = {"linux"};
        manifest.modules = {module};
        RequireResolutionError(ResolveExtensionModules(manifest, Host()), "compatibility authority cannot compete");
    }
}  // namespace Horo::Extensions::Tests

namespace Horo::Extensions::Tests {
    TEST_CASE("Explicit module roots isolate optional incompatibility and retain required dependencies",
              "[Extensions][Resolution][Hybrid]") {
        auto backend = Module("com.example.backend", ExtensionModuleRole::BackendCapability);
        auto script = Module("com.example.script", ExtensionModuleRole::ScriptProvider);
        script.dependencies = {backend.id};
        auto gui = Module("com.example.gui", ExtensionModuleRole::EditorPresentation);
        gui.abi = ExtensionAbiRequirement{99, 0};
        gui.imports = {{"com.example.missing", "com.example.absent", "com.example.contract", "1.0.0", true}};
        ExtensionManifest manifest;
        manifest.id = "com.example.hybrid";
        manifest.modules = {gui, script, backend};
        const std::array selected{script.id};
        const auto healthy = ResolveExtensionModules(manifest, Host(), selected);
        REQUIRE(healthy.HasValue());
        CHECK(healthy.Value().moduleIds == std::vector<std::string>{backend.id, script.id});
        CHECK(ResolveExtensionModules(manifest, Host()).HasError());
        const std::array invalid{std::string{"missing.module"}};
        CHECK(ResolveExtensionModules(manifest, Host(), invalid).HasError());
        manifest.modules[2].abi = ExtensionAbiRequirement{99, 0};
        CHECK(ResolveExtensionModules(manifest, Host(), selected).HasError());
    }
}  // namespace Horo::Extensions::Tests
