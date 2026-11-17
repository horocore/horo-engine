#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorSvgIcon.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Extensions/LuaExtensionServiceBinding.h"
#include "Horo/Foundation/JobSystem.h"
#include "SecurityTestSupport.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Extensions;

    void Check(const bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error{message};
    }

    template <typename T> T Take(Result<T> result) {
        if (result.HasError())
            throw std::runtime_error{result.ErrorValue().code.Value() + ": " + result.ErrorValue().message};
        return std::move(result).Value();
    }

    void Accept(Result<void> result) {
        if (result.HasError())
            throw std::runtime_error{result.ErrorValue().code.Value() + ": " + result.ErrorValue().message};
    }

    ExtensionAdmissionPolicy ServicePolicy(const bool approved = true) {
        ExtensionAdmissionPolicy policy{.revision = 1,
                                        .knownPermissions = {{HORO_EXTENSION_SERVICE_PERMISSION}},
                                        .availableCapabilities = {{HORO_EXTENSION_SERVICE_CAPABILITY}}};
        if (approved)
            policy.approvedPermissions.push_back({HORO_EXTENSION_SERVICE_PERMISSION});
        return policy;
    }

    struct Host final {
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 8}};
        std::shared_ptr<ApplicationCapabilityRegistry> capabilities{std::make_shared<ApplicationCapabilityRegistry>()};
        std::shared_ptr<BackendServiceRegistry> services{std::make_shared<BackendServiceRegistry>()};
        std::shared_ptr<ScriptInvocationRegistry> invocations{std::make_shared<ScriptInvocationRegistry>()};
        std::shared_ptr<NativeExtensionServiceHost> native;
        std::shared_ptr<EditorActivityHost> activity;
        std::vector<std::string> loadedLibraries;
        std::unique_ptr<ExtensionManager> manager;
        ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project};

        Host(const bool interactive, const bool approved = true) {
            native = std::make_shared<NativeExtensionServiceHost>(capabilities, services, invocations, ServicePolicy(approved));
            std::vector<std::string> features{HORO_EXTENSION_SERVICE_HOST_CAPABILITY};
            if (interactive) {
                activity = std::make_shared<EditorActivityHost>(jobs, ExtensionAdmissionPolicy{.revision = 1,
                                                                                               .availableCapabilities = {
                                                                                                   EditorCustomCompositionCapability}});
                features.push_back(HORO_EDITOR_ACTIVITY_HOST_CAPABILITY);
                features.push_back(HORO_EDITOR_CUSTOM_UI_HOST_CAPABILITY);
            }
            manager = std::make_unique<ExtensionManager>(nullptr,
                                                         interactive ? ExtensionHostProfile::Interactive : ExtensionHostProfile::Headless,
                                                         std::move(features), Horo::Tests::CreateAcceptingArtifactGate(),
                                                         [this](const std::string &path) {
                loadedLibraries.push_back(std::filesystem::path{path}.filename().string());
                return Platform::LoadDynamicLibrary(path);
            }, ExtensionPlatformProviderCommit{}, ExtensionEditorHosts{.activities = activity}, native);
        }

        ScriptCapabilityContextDescriptor ScriptPolicy(const std::string &id) const {
            const ExtensionCapabilityId capability{id + ".arithmetic"};
            return {.scriptId = id + ".reference-script",
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
                    .scopes = {project.Lease()}};
        }

        std::shared_ptr<NativeExtensionScriptSession> Bind(const std::string &id) {
            return Take(native->BindScript(id, id + ".script", id + ".backend-import", ScriptPolicy(id)));
        }
    };

    std::string ReadResource(const std::filesystem::path &package, const std::filesystem::path &relative) {
        const auto path = package / relative;
        Check(std::filesystem::file_size(path) <= 64U * 1024U, "resource exceeds bounded SDK fixture source cap");
        std::ifstream input{path, std::ios::binary};
        Check(static_cast<bool>(input), "cannot read real packaged reference resource");
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    void IconChecks(const std::filesystem::path &package) {
        const auto icon = Take(RasterizeEditorSvgIcon(ReadResource(package, "resources/icon.svg")));
        Check(icon.width > 0 && icon.height > 0 && !icon.pixels.empty(), "actual packaged icon produced no raster");
        Check(std::ranges::any_of(icon.pixels,
                                  [](const auto byte) {
            return byte != 0;
        }),
              "actual packaged icon raster is empty");
        const auto rejected = RasterizeEditorSvgIcon(
            R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"><path fill="currentColor" d="M4 4h16v16H4z"/></svg>)");
        Check(rejected.HasError(), "unsupported relative path commands escaped the canonical bounded icon grammar");
    }

    void ScriptChecks(Host &host, const std::filesystem::path &package, const std::string &id) {
        auto binding = host.Bind(id);
        Check(Take(ExecuteLuaExtensionService(*binding, ReadResource(package, "resources/reference.lua"))) == 42,
              "actual Lua result is not42");
        Check(Take(ExecuteLuaExtensionService(*binding, "local v,e=api.add(9223372036854775807,1); assert(v==nil and e.code=='overflow'); "
                                                        "return 7")) == 7,
              "overflow error lost its native semantic identity");
        Check(ExecuteLuaExtensionService(*binding, "return io.open('forbidden')").HasError(), "Lua acquired undeclared file authority");
        Check(ExecuteLuaExtensionService(*binding, "while true do end").HasError(), "Lua instruction ceiling did not terminate");
        Check(ExecuteLuaExtensionService(*binding, "return 1", {}, {.maximumInstructions = 1001}).HasError(),
              "unsupported partial hook budget was silently rounded above its ceiling");
        Check(Take(ExecuteLuaExtensionService(*binding, "return 1", {}, {.maximumInstructions = 1000})) == 1,
              "minimum supported exact hook budget failed");
        Check(ExecuteLuaExtensionService(*binding, "while true do end", {}, {.maximumInstructions = 1000}).HasError(),
              "minimum hook ceiling did not stop unbounded Lua");
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        Check(ExecuteLuaExtensionService(*binding, "return api.add(1,2)", cancelled.Token()).HasError(), "cancelled Lua call succeeded");
        auto denied = host.ScriptPolicy(id);
        denied.scriptTrusted = false;
        Check(host.native->BindScript(id, id + ".script", id + ".backend-import", std::move(denied)).HasError(),
              "untrusted script received a real binding");
        const auto retired = host.manager->RetireExtension(id);
        Check(retired.disposition == ExtensionRetirementDisposition::Complete, "idle native package did not retire");
        Check(binding->Invoke("add", std::array<std::int64_t, 2>{1, 2}).HasError(), "retired script binding entered native provider");
    }

    void GuiChecks(Host &host, const std::string &id) {
        host.activity->Update();
        const auto initial = host.activity->Prepared();
        Check(!initial.empty(), "real native GUI module published no activity");
        const auto provider = initial.front().surface.descriptor.provider;
        const auto revision = initial.front().revision;
        Accept(host.activity->QueueAction(provider, id + ".activity", "reference.run", "reference.run", revision));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            host.activity->Update();
            const auto projected = host.activity->Prepared();
            if (!projected.empty() && projected.front().revision > revision)
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        throw std::runtime_error{"real native GUI action worker did not publish its copied result"};
    }

    void OutlivingBinding(const std::filesystem::path &package, const std::string &id) {
        std::shared_ptr<NativeExtensionScriptSession> binding;
        std::weak_ptr<BackendServiceRegistry> actualOwner;
        {
            Host host{false};
            actualOwner = host.services;
            Check(Take(host.manager->LoadExtension(package.string())) == id, "wrong actual package identity");
            binding = host.Bind(id);
            Check(binding->Invoke("add", std::array<std::int64_t, 2>{20, 22}).HasValue(), "live native call failed");
            host.manager->UnloadAll();
            host.native->BeginShutdown();
        }
        Check(binding->Invoke("add", std::array<std::int64_t, 2>{1, 2}).HasError(), "outliving client used destroyed registry");
        Check(actualOwner.expired(), "inactive metadata unexpectedly retains the backend registry owner");
    }

    void Exercise(const std::filesystem::path &package, const std::string &id, const std::string &shape) {
        const bool gui = shape == "gui-basic" || shape == "hybrid-basic";
        const bool script = shape == "script-api-basic" || shape == "hybrid-basic";
        if (gui)
            IconChecks(package);
        Host host{gui};
        Check(Take(host.manager->LoadExtension(package.string())) == id, "actual module package identity mismatch");
        if (gui)
            GuiChecks(host, id);
        if (script)
            ScriptChecks(host, package, id);
        if (shape == "gui-basic")
            return;
        Host denied{false, false};
        Check(denied.manager->LoadExtension(package.string()).HasError(), "missing explicit service permission was admitted");
    }

    void Headless(const std::filesystem::path &package, const std::string &id, const std::string &shape) {
        if (shape != "hybrid-basic" && shape != "script-api-basic")
            return;
        Host host{false};
        Check(Take(host.manager->LoadExtension(package.string())) == id, "headless package activation failed");
        Check(std::ranges::none_of(host.loadedLibraries,
                                   [](const auto &file) {
            return file.find("reference_editor") != std::string::npos;
        }),
              "headless composition loaded native GUI code");
        ScriptChecks(host, package, id);
        OutlivingBinding(package, id);
    }
}  // namespace

int main(const int argc, char **argv) {
    if (argc != 4)
        return 2;
    try {
        Exercise(argv[1], argv[2], argv[3]);
        Headless(argv[1], argv[2], argv[3]);
        std::cout << "actual package/registry/Lua/GUI-worker checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
