#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Extensions/NativeExtensionServiceHost.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "SecurityTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
    using namespace Horo;
    using namespace Horo::Extensions;

    struct Package final {
        std::filesystem::path root;

        Package() {
            const auto parent = std::filesystem::temp_directory_path();
            const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
            root = parent / ("horo-service-rollback-" + std::to_string(stamp));
            REQUIRE(std::filesystem::create_directory(root));
            const auto source = std::filesystem::path{HORO_NATIVE_SERVICE_FAILURE_FIXTURE};
            const auto name = "failure" + source.extension().string();
            std::filesystem::copy_file(source, root / name);
            std::ofstream manifest{root / "extension.json"};
            manifest
                << R"({"schemaVersion":1,"id":"com.horo.service-failure","version":"1.0.0","modules":[{"id":"com.horo.service-failure.backend","version":"1.0.0","kind":"native","entry":")"
                << name
                << R"(","roles":["backend-capability"],"abi":{"major":1,"minimumMinor":8},"requiredCapabilities":["extension.services"],"exports":[{"id":"com.horo.service-failure.arithmetic","contract":"horo.examples.checked-arithmetic","version":"1.0.0"}]}]})";
            REQUIRE(static_cast<bool>(manifest));
        }

        ~Package() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };
}  // namespace

TEST_CASE("Real service load rollback drains native registration before module unload") {
    Package package;
    // Find the actual binary rather than treating a manifest as a loadable library.
    std::filesystem::path native;
    for (const auto &entry : std::filesystem::directory_iterator(package.root))
        if (entry.path().filename() != "extension.json")
            native = entry.path();
    auto observed = Platform::LoadDynamicLibrary(native.string());
    REQUIRE(observed.HasValue());
    using Event = unsigned (*)(unsigned);
    const auto event = reinterpret_cast<Event>(observed.Value()->GetSymbol("horo_reference_service_event"));
    REQUIRE(event != nullptr);
    auto capabilities = std::make_shared<ApplicationCapabilityRegistry>();
    auto services = std::make_shared<BackendServiceRegistry>();
    auto scripts = std::make_shared<ScriptInvocationRegistry>();
    auto host =
        std::make_shared<NativeExtensionServiceHost>(capabilities, services, scripts,
                                                     ExtensionAdmissionPolicy{.revision = 1,
                                                                              .knownPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                                                                              .approvedPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                                                                              .availableCapabilities = {
                                                                                  {HORO_EXTENSION_SERVICE_CAPABILITY}}});
    ExtensionManager manager{nullptr,
                             ExtensionHostProfile::Headless,
                             {HORO_EXTENSION_SERVICE_HOST_CAPABILITY},
                             Horo::Tests::CreateAcceptingArtifactGate(),
                             {},
                             ExtensionPlatformProviderCommit{},
                             ExtensionEditorHosts{},
                             host};
    const auto loaded = manager.LoadExtension(package.root.string());
    REQUIRE(loaded.HasError());
    CHECK(event(0) == 1);
    CHECK(event(1) == 2);
    CHECK(event(2) == 3);
    CHECK(event(3) == 0);
    manager.UnloadAll();
    host->BeginShutdown();
    CHECK(event(3) == 0);
}
