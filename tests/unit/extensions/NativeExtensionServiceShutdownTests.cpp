#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"
#include "Horo/Extensions/NativeExtensionServiceHost.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "SecurityTestSupport.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <semaphore>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Extensions;

    ExtensionAdmissionPolicy Policy() {
        return {.revision = 1,
                .knownPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                .approvedPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                .availableCapabilities = {{HORO_EXTENSION_SERVICE_CAPABILITY}}};
    }

    class UnusedPolicy final : public IToolchainInvocationPolicy {
        Result<ExternalProcessRequest> Resolve(const ToolchainProviderDescriptor &, const ToolchainInvocationIntent &) const override {
            return Result<ExternalProcessRequest>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        }
    };

    struct Probe final {
        std::binary_semaphore entered{0}, release{0};
        std::atomic_bool released{};
        std::atomic_bool cancellationObserved{};

        void Release() {
            if (!released.exchange(true))
                release.release();
        }

        static void Enter(void *value) {
            static_cast<Probe *>(value)->entered.release();
        }

        static void Wait(void *value, const HoroExtensionCancellation token) {
            auto &probe = *static_cast<Probe *>(value);
            while (!probe.release.try_acquire_for(std::chrono::milliseconds{1})) {
                if (token.isCancellationRequested && token.isCancellationRequested(token.context)) {
                    probe.cancellationObserved.store(true);
                    return;
                }
            }
        }
    };

    struct ReleaseOnExit final {
        Probe &probe;

        ~ReleaseOnExit() {
            probe.Release();
        }
    };

    template <class T> T Symbol(const Platform::DynamicLibrary &library, const char *name) {
        const auto value = reinterpret_cast<T>(library.GetSymbol(name));
        REQUIRE(value != nullptr);
        return value;
    }

    struct Package final {
        std::filesystem::path root;
        std::shared_ptr<Platform::DynamicLibrary> backend, client;

        Package() {
            root = std::filesystem::temp_directory_path() /
                   ("horo-service-concurrent-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            REQUIRE(std::filesystem::create_directory(root));
            const auto suffix = std::filesystem::path{HORO_NATIVE_SERVICE_PROVIDER_FIXTURE}.extension().string();
            std::filesystem::copy_file(HORO_NATIVE_SERVICE_PROVIDER_FIXTURE, root / ("backend" + suffix));
            std::filesystem::copy_file(HORO_NATIVE_SERVICE_CLIENT_FIXTURE, root / ("client" + suffix));
            std::ofstream manifest{root / "extension.json"};
            manifest
                << R"({"schemaVersion":1,"id":"com.horo.concurrent","version":"1.0.0","modules":[{"id":"com.horo.concurrent.backend","version":"1.0.0","kind":"native","entry":"backend)"
                << suffix
                << R"(","roles":["backend-capability","script-provider"],"abi":{"major":1,"minimumMinor":8},"requiredCapabilities":["extension.services"],"exports":[{"id":"com.horo.concurrent.arithmetic","contract":"horo.examples.checked-arithmetic","version":"1.0.0"}]},{"id":"com.horo.concurrent.client","version":"1.0.0","kind":"native","entry":"client)"
                << suffix
                << R"(","roles":["script-provider"],"abi":{"major":1,"minimumMinor":8},"requiredCapabilities":["extension.services"],"dependencies":["com.horo.concurrent.backend"],"imports":[{"id":"backend-import","service":"com.horo.concurrent.arithmetic","contract":"horo.examples.checked-arithmetic","minimumVersion":"1.0.0"}]}],"contributions":[{"type":"script.api","id":"com.horo.concurrent.api","module":"com.horo.concurrent.backend"}]})";
            REQUIRE(manifest.good());
            manifest.close();
            auto first = Platform::LoadDynamicLibrary((root / ("backend" + suffix)).string());
            REQUIRE(first.HasValue());
            backend = std::move(first).Value();
            auto second = Platform::LoadDynamicLibrary((root / ("client" + suffix)).string());
            REQUIRE(second.HasValue());
            client = std::move(second).Value();
        }

        ~Package() {
            client.reset();
            backend.reset();
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    ScriptCapabilityContextDescriptor ScriptPolicy(const ScriptCapabilityScope &scope) {
        const ExtensionCapabilityId capability{"com.horo.concurrent.arithmetic"};
        return {.scriptId = "concurrent-test",
                .policyRevision = 1,
                .profile = ScriptCapabilityProfile::Tooling,
                .packageVerified = true,
                .packageTrusted = true,
                .scriptTrusted = true,
                .runtimeCompatible = true,
                .packageCapabilities = {capability},
                .scriptCapabilities = {capability},
                .projectCapabilities = {capability},
                .contextCapabilities = {capability},
                .scopes = {scope.Lease()}};
    }
}  // namespace

TEST_CASE("Service-enabled empty headless host preserves actual off-lane destruction contract") {
    UnusedPolicy policy;
    NativeExternalProcessRunner runner;
    auto result = HeadlessExtensionHost::Create({.servicePolicy = Policy()}, policy, runner);
    REQUIRE(result.HasValue());
    auto host = std::move(result).Value();
    std::thread destroy{[owner = std::move(host)]() mutable {
        owner.reset();
    }};
    destroy.join();
    CHECK(true);  // Reaching here proves the real destructor did not terminate.
}

TEST_CASE("Loaded service headless destruction on worker retains owner-only native finalization for restart", "[native-retained-process]") {
    Package package;
    UnusedPolicy policy;
    NativeExternalProcessRunner runner;
    HeadlessExtensionHostConfiguration configuration;
    configuration.roots.push_back({.id = "service-root",
                                   .path = package.root.parent_path(),
                                   .kind = Discovery::RootKind::Development,
                                   .approval = Discovery::RootApproval::Approved,
                                   .configuration = Discovery::ConfigurationOrigin::UserLocal});
    configuration.discoveryPolicy = {.profile = Discovery::DiscoveryProfile::Development, .enableDevelopmentOverrides = true};
    configuration.artifactGate = Horo::Tests::CreateAcceptingArtifactGate();
    configuration.servicePolicy = Policy();
    auto created = HeadlessExtensionHost::Create(std::move(configuration), policy, runner);
    REQUIRE(created.HasValue());
    auto host = std::move(created).Value();
    const std::array locations{Discovery::PackageLocation{"com.horo.concurrent", "service-root", package.root.filename(), {}}};
    const auto started = host->Start(locations);
    REQUIRE(started.HasValue());
    auto shutdown = std::async(std::launch::async, [owner = std::move(host)]() mutable {
        owner->Shutdown();
        auto snapshot = owner->Inspect();
        owner.reset();
        return snapshot;
    });
    REQUIRE(shutdown.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    const auto snapshot = shutdown.get();
    REQUIRE(snapshot.retirements.size() == 1);
    CHECK(snapshot.retirements.front().disposition == ExtensionRetirementDisposition::RestartRequired);
    using Count = unsigned (*)(void);
    CHECK(Symbol<Count>(*package.backend, "horo_reference_unloaded")() == 0);
    CHECK(Symbol<Count>(*package.client, "horo_reference_unloaded")() == 0);
    // Canonical retained native record intentionally survives until process restart; no leak-free shutdown claim.
}

TEST_CASE("Off-lane service close denies script admission while actual native calls drain") {
    Package package;
    Probe probe;
    using Configure = void (*)(void *, void (*)(void *), void (*)(void *, HoroExtensionCancellation));
    Symbol<Configure>(*package.backend, "horo_reference_block")(&probe, Probe::Enter, Probe::Wait);
    auto host = std::make_shared<NativeExtensionServiceHost>(std::make_shared<ApplicationCapabilityRegistry>(),
                                                             std::make_shared<BackendServiceRegistry>(),
                                                             std::make_shared<ScriptInvocationRegistry>(), Policy());
    ExtensionManager manager{nullptr,
                             ExtensionHostProfile::Headless,
                             {HORO_EXTENSION_SERVICE_HOST_CAPABILITY},
                             Horo::Tests::CreateAcceptingArtifactGate(),
                             {},
                             ExtensionPlatformProviderCommit{},
                             ExtensionEditorHosts{},
                             host};
    const auto loaded = manager.LoadExtension(package.root.string());
    REQUIRE(loaded.HasValue());
    ScriptCapabilityScope scope{ScriptCapabilityScopeKind::Project};
    auto session = host->BindScript("com.horo.concurrent", "com.horo.concurrent.client", "backend-import", ScriptPolicy(scope));
    REQUIRE(session.HasValue());
    const auto retirement = manager.Retirement("com.horo.concurrent");
    REQUIRE(retirement);
    auto callback = retirement->Acquire("com.horo.concurrent.client", ExtensionLeaseKind::Callback, "real-native-import", package.client);
    REQUIRE(callback);
    using Call = HoroExtensionStatus (*)(void);
    const auto call = Symbol<Call>(*package.client, "horo_reference_call");
    auto work = std::async(std::launch::async, [call, pin = callback] {
        static_cast<void>(pin);
        return call();
    });
    ReleaseOnExit releaseOnExit{probe};
    const bool entered = probe.entered.try_acquire_for(std::chrono::seconds{5});
    if (!entered)
        probe.Release();
    REQUIRE(entered);
    std::thread close{[host] {
        host->BeginShutdown();
    }};
    close.join();
    CHECK(host->BindScript("com.horo.concurrent", "com.horo.concurrent.client", "backend-import", ScriptPolicy(scope)).HasError());
    CHECK(session.Value()->Invoke("add", std::array<std::int64_t, 2>{1, 2}).HasError());
    const auto retiring = manager.RetireExtension("com.horo.concurrent");
    CHECK(retiring.disposition == ExtensionRetirementDisposition::Draining);
    using Count = unsigned (*)(void);
    const auto stopped = Symbol<Count>(*package.backend, "horo_reference_stopped");
    const auto unloaded = Symbol<Count>(*package.backend, "horo_reference_unloaded");
    CHECK(probe.cancellationObserved.load());
    CHECK(stopped() == 1);
    CHECK(unloaded() == 0);  // Actual attributed caller lease still owns native dependency closure.
    REQUIRE(work.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
    CHECK(work.get() == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
    callback.reset();
    manager.FinalizeRetirements();
    CHECK(stopped() == 1);
    CHECK(unloaded() == 1);
    CHECK(retirement->Inspect().disposition == ExtensionRetirementDisposition::Complete);
}
