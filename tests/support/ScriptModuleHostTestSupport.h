#pragma once
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/HeadlessExtensionHost.h"
#include "Horo/Platform/ExternalProcess.h"
#include "HostSignatureDocument.h"
#include "ModuleScriptHostBridge.h"
#include "ModuleScriptPackage.h"
#include "OpenXRProductSigningTestSupport.h"
#include "ScriptModuleHostServices.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <thread>

namespace Horo::Extensions::Tests::ModuleHostSupport {
    template <class T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "ok"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline void Accept(Result<void> result) {
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "ok"));
        REQUIRE(result.HasValue());
    }

    inline std::vector<std::byte> Read(const std::filesystem::path &path) {
        std::ifstream stream{path, std::ios::binary};
        REQUIRE(stream);
        const std::vector<char> text{std::istreambuf_iterator<char>{stream}, {}};
        REQUIRE(text.size() <= 32U * 1024U * 1024U);
        std::vector<std::byte> bytes;
        bytes.reserve(text.size());
        for (const auto value : text)
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
        return bytes;
    }

    inline void Write(const std::filesystem::path &path, std::string_view text) {
        std::ofstream stream{path, std::ios::binary | std::ios::trunc};
        REQUIRE(stream);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(stream);
    }

    inline std::string Hex(std::span<const std::byte> bytes) {
        constexpr char alphabet[] = "0123456789abcdef";
        std::string output;
        for (const auto byte : bytes) {
            const auto value = std::to_integer<unsigned>(byte);
            output += alphabet[value >> 4U];
            output += alphabet[value & 15U];
        }
        return output;
    }

    inline ExtensionAdmissionPolicy Policy() {
        return {.revision = 1,
                .knownPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                .approvedPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                .availableCapabilities = {{HORO_EXTENSION_SERVICE_CAPABILITY}}};
    }

    class NoTools final : public IToolchainInvocationPolicy {
        Result<ExternalProcessRequest> Resolve(const ToolchainProviderDescriptor &, const ToolchainInvocationIntent &) const override {
            return Result<ExternalProcessRequest>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        }
    };

    // Genuine built test native binaries, canonical test signer and actual product authentication. Never installed production trust.
    struct SignedPackage final {
        Horo::Tests::OpenXRProduct::SigningKey key;
        std::filesystem::path root, package, temporary, trust, source;
        std::shared_ptr<const Application::Internal::AuthenticatedExtensionScript> packet;
        std::filesystem::path staged;
        ScriptModuleRuntimeKind runtime;
        bool reference, sourceOnly;
        std::string id, module;

        SignedPackage(ScriptModuleRuntimeKind vm, std::string_view code, bool builtin = true, bool noImports = false)
            : runtime(vm), reference(builtin), sourceOnly(noImports), id(noImports ? "com.horo.source" : "com.horo.concurrent"),
              module(id + (noImports ? ".script" : ".client")) {
            root = std::filesystem::temp_directory_path() /
                   ("horo-module-operation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            REQUIRE(std::filesystem::create_directory(root));
            root = std::filesystem::canonical(root);
            package = root / "package";
            temporary = root / "temporary";
            trust = root / "trust.json";
            REQUIRE(std::filesystem::create_directory(package));
            REQUIRE(std::filesystem::create_directory(temporary));
            const auto suffix = std::filesystem::path{HORO_NATIVE_SERVICE_PROVIDER_FIXTURE}.extension().string();
            if (sourceOnly)
                std::filesystem::copy_file(HORO_MODULE_SCRIPT_SOURCE_FIXTURE, package / ("script" + suffix));
            else {
                std::filesystem::copy_file(HORO_NATIVE_SERVICE_PROVIDER_FIXTURE, package / ("backend" + suffix));
                std::filesystem::copy_file(HORO_NATIVE_SERVICE_CLIENT_FIXTURE, package / ("client" + suffix));
            }
            Write(package / "extension.json", Manifest(suffix));
            source = runtime == ScriptModuleRuntimeKind::Lua54 ? "program.lua" : "program.js";
            Write(package / source, code);
            Sign(package / "extension.json");
            if (sourceOnly)
                Sign(package / ("script" + suffix));
            else {
                Sign(package / ("backend" + suffix));
                Sign(package / ("client" + suffix));
            }
            Sign(package / source);
            Trust(false);
        }

        ~SignedPackage() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::string Manifest(const std::string &suffix) const {
            const std::string caps = reference ? R"("extension.services","horo.script.reference")" : R"("extension.services")";
            if (sourceOnly)
                return R"({"schemaVersion":1,"id":"com.horo.source","version":"1.0.0","modules":[{"id":"com.horo.source.script","version":"1.0.0","kind":"native","entry":"script)" +
                       suffix + R"(","roles":["script-provider"],"abi":{"major":1,"minimumMinor":8},"requiredCapabilities":[)" + caps +
                       R"(]}]})";
            return R"({"schemaVersion":1,"id":"com.horo.concurrent","version":"1.0.0","modules":[{"id":"com.horo.concurrent.backend","version":"1.0.0","kind":"native","entry":"backend)" +
                   suffix +
                   R"(","roles":["backend-capability","script-provider"],"abi":{"major":1,"minimumMinor":8},"requiredCapabilities":["extension.services"],"exports":[{"id":"com.horo.concurrent.arithmetic","contract":"horo.examples.checked-arithmetic","version":"1.0.0"}]},{"id":"com.horo.concurrent.client","version":"1.0.0","kind":"native","entry":"client)" +
                   suffix + R"(","roles":["script-provider"],"abi":{"major":1,"minimumMinor":8},"requiredCapabilities":[)" + caps +
                   R"(],"dependencies":["com.horo.concurrent.backend"],"imports":[{"id":"backend-import","service":"com.horo.concurrent.arithmetic","contract":"horo.examples.checked-arithmetic","minimumVersion":"1.0.0","required":true}]}],"contributions":[{"type":"script.api","id":"com.horo.concurrent.api","module":"com.horo.concurrent.backend"}]})";
        }

        void Sign(const std::filesystem::path &path) {
            Write(path.string() + ".signature.json", Application::Internal::EncodeDetachedSignatureDocument(key.Sign(Read(path))));
        }

        void Trust(bool revoked) {
            Write(
                trust,
                R"({"schemaVersion":1,"allowUnsigned":false,"publishers":[{"publisherId":"com.horo.tests","keyId":"host-test-key","algorithm":"ecdsa-p256-sha256","publicKeyHex":")" +
                    Hex(key.root.publicKey) + R"(","expiresAtUnixMilliseconds":0,"revoked":)" + (revoked ? "true" : "false") + "}]}");
        }

        Application::Internal::ExtensionScriptOptions Options() const {
            return {.packageRoot = package,
                    .hostTrust = trust,
                    .temporaryRoot = temporary,
                    .script = source,
                    .package = id,
                    .module = module,
                    .publisher = key.root.publisherId,
                    .moduleOperation = true,
                    .runtime = runtime};
        }

        void Capture() {
            packet =
                Take(Application::Internal::AuthenticateModuleScriptPackage(Options(), {}, ExtensionErrors::ScriptInvocationCancelled));
            staged = Take(Application::Internal::StageModuleScriptPackage(Options(), *packet));
        }

        std::shared_ptr<const ScriptModuleProgram> Program() const {
            return Take(Application::Internal::MakeModuleScriptProgram(staged, packet));
        }

        HeadlessExtensionHostConfiguration Configuration() const {
            return {.roots = {{"module-test", staged.parent_path(), Discovery::RootKind::User, Discovery::RootApproval::Approved,
                               Discovery::ConfigurationOrigin::UserLocal}},
                    .capabilities = {"horo.script.reference"},
                    .artifactGate = std::make_shared<Application::Internal::ExtensionScriptArtifactGate>(staged, packet),
                    .servicePolicy = Policy()};
        }
    };

    inline ScriptModuleOperationPolicy OperationPolicy() {
        return {.projectCapabilities = {{"horo.script.reference"}, {"com.horo.concurrent.arithmetic"}},
                .contextCapabilities = {{"horo.script.reference"}, {"com.horo.concurrent.arithmetic"}},
                .nativeConsent = true,
                .scriptConsent = true};
    }

    struct Headless final {
        NoTools tools;
        NativeExternalProcessRunner processes;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 64}};
        std::unique_ptr<HeadlessExtensionHost> host;
        std::shared_ptr<ScriptModuleHostServices> modules;
        ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project}, operation{ScriptCapabilityScopeKind::Operation};

        explicit Headless(const SignedPackage &package) {
            host = Take(HeadlessExtensionHost::Create(package.Configuration(), tools, processes));
            modules = Take(ScriptModuleHostServices::CreateConfiguringHeadless(*host, jobs));
            const std::array<Discovery::PackageLocation, 1> locations{{{package.id, "module-test", package.staged.filename(), {}}}};
            Accept(host->Start(locations));
        }

        ~Headless() {
            modules.reset();
            host->Shutdown();
        }

        ScriptModuleOperationPolicy Policy() const {
            auto policy = OperationPolicy();
            policy.scopes = {project.Lease(), operation.Lease()};
            return policy;
        }
    };

    struct Interactive final {
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 64}};
        std::shared_ptr<ApplicationCapabilityRegistry> capabilities{std::make_shared<ApplicationCapabilityRegistry>()};
        std::shared_ptr<BackendServiceRegistry> services{std::make_shared<BackendServiceRegistry>()};
        std::shared_ptr<ScriptInvocationRegistry> invocations{std::make_shared<ScriptInvocationRegistry>()};
        std::shared_ptr<NativeExtensionServiceHost> native{
            std::make_shared<NativeExtensionServiceHost>(capabilities, services, invocations, Policy())};
        std::shared_ptr<ScriptModuleHostServices> modules;
        std::unique_ptr<ExtensionManager> manager;

        explicit Interactive(const SignedPackage &package, ScriptCapabilityProfile profile = ScriptCapabilityProfile::Gameplay) {
            modules = Take(ScriptModuleHostServices::Create(native, jobs, profile));
            manager =
                std::make_unique<ExtensionManager>(nullptr,
                                                   profile == ScriptCapabilityProfile::Gameplay ? ExtensionHostProfile::Interactive
                                                                                                : ExtensionHostProfile::Headless,
                                                   std::vector<std::string>{"extension.services", "horo.script.reference"},
                                                   std::make_shared<Application::Internal::ExtensionScriptArtifactGate>(package.staged,
                                                                                                                        package.packet),
                                                   ExtensionManager::NativeLibraryLoader{}, ExtensionPlatformProviderCommit{},
                                                   ExtensionEditorHosts{}, native);
            CHECK(Take(manager->LoadExtension(package.staged.string())) == package.id);
        }

        ~Interactive() {
            modules.reset();
            manager->UnloadAll();
            native->BeginShutdown();
        }
    };

    inline ScriptCallResult Await(ScriptModuleOperation &operation) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!operation.ResultSnapshot() && std::chrono::steady_clock::now() < deadline) {
            Accept(operation.Poll());
            std::this_thread::yield();
        }
        auto result = operation.ResultSnapshot();
        REQUIRE(result);
        return std::move(*result);
    }

    inline std::string SumSource(ScriptModuleRuntimeKind runtime) {
        return runtime == ScriptModuleRuntimeKind::Lua54 ? R"(return horo.import("horo.reference").horo.reference.sum(41))"
                                                         : R"(return horo.import("horo.reference").horo.reference.sum(41n);)";
    }
}  // namespace Horo::Extensions::Tests::ModuleHostSupport
