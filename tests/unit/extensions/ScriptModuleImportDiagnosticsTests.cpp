#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleRuntimeFixture.h"

#include <array>
#include <chrono>
#include <thread>

namespace Horo::Extensions::Tests {
    using namespace ModuleSupport;

    namespace {
        Result<ExtensionModulePlan> Resolve(bool required, bool available, std::string version = "1.0.0", bool ambiguous = false) {
            ExtensionManifest manifest;
            manifest.id = "horo.reference.package";
            manifest.modules = {Consumer(required)};
            if (available) {
                auto provider = Provider();
                provider.exports[0].version = std::move(version);
                manifest.modules.push_back(provider);
                if (ambiguous) {
                    provider.id = "horo.reference.other";
                    manifest.modules.push_back(std::move(provider));
                }
            }
            return ResolveExtensionModules(manifest, Environment());
        }

        ScriptModuleImport Bound(Fixture &fixture, bool required) {
            auto input = fixture.Input();
            input.import = Take(Resolve(required, true)).serviceImports.front();
            return Take(BindScriptModuleImport("reference", Take(BindScriptRuntimeBackend<ObservedService>(std::move(input),
                                                                                                           &ObservedService::Invoke))));
        }

        ScriptModuleImport Missing(bool incompatible = false) {
            auto plan = Take(Resolve(false, incompatible, incompatible ? "2.0.0" : "1.0.0"));
            return Take(MakeUnavailableScriptModuleImport("reference", plan.serviceImports.front()));
        }

        auto Context(Fixture &fixture, const JobSystem &jobs, ScriptModuleBindingCache &cache, ScriptModuleImport import) {
            std::vector<ScriptModuleImport> imports;
            imports.push_back(std::move(import));
            return ScriptModuleContext::Create(fixture.invocations, jobs, cache, LuaScriptModuleRuntimeIdentity("headless"),
                                               fixture.Policy(), std::move(imports));
        }

        ScriptCallResult AwaitImportEvidence(IScriptModuleRuntime &runtime) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (!runtime.ResultSnapshot() && std::chrono::steady_clock::now() < deadline) {
                Accept(runtime.Poll());
                std::this_thread::yield();
            }
            auto result = runtime.ResultSnapshot();
            REQUIRE(result);
            return std::move(*result);
        }

        void Actionable(const Result<ExtensionModulePlan> &result, std::string_view reason, std::string_view owner) {
            REQUIRE(result.HasError());
            const auto &error = result.ErrorValue();
            CHECK(error.code.Value() == ExtensionErrors::ModuleResolutionFailed.code.Value());
            CHECK(error.domain.Value() == ExtensionErrors::ModuleResolutionFailed.domain.Value());
            CHECK_FALSE(error.message.empty());
            CHECK(error.message.find(reason) != std::string::npos);
            CHECK(error.message.find("horo.script.reference") != std::string::npos);
            CHECK(error.message.find(owner) != std::string::npos);
        }
    }  // namespace

    TEST_CASE("Missing optional and incompatible imports retain distinct actual canonical resolver diagnostics",
              "[Extensions][ScriptModule][Imports]") {
        const auto absent = Missing();
        CHECK(absent.Resolution().Status() == ExtensionServiceImportStatus::Unavailable);
        REQUIRE(absent.Unavailable());
        const auto incompatible = Missing(true);
        CHECK(incompatible.Resolution().Status() == ExtensionServiceImportStatus::Incompatible);
        REQUIRE(incompatible.Unavailable());
        CHECK(absent.Unavailable()->code.Value() != incompatible.Unavailable()->code.Value());
        CHECK(Resolve(true, false).HasError());
        CHECK(Resolve(true, true, "2.0.0").HasError());
        CHECK(Resolve(true, true, "1.0.0", true).HasError());
    }

    TEST_CASE("Compatible optional availability transitions use fresh actual imports without weakening a required binding",
              "[Extensions][ScriptModule][Replacement][Imports]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto missing = Take(Context(fixture, jobs, cache, Missing()));
        auto present = Take(Context(fixture, jobs, cache, Bound(fixture, false)));
        Accept(missing->ValidateReplacement(*present));
        Accept(present->ValidateReplacement(*missing));
        CHECK(missing->Invoke("reference", "sum", {ScriptValue{std::int64_t{1}}}).HasError());
        auto required = Take(Context(fixture, jobs, cache, Bound(fixture, true)));
        CHECK(required->ValidateReplacement(*missing).HasError());
        CHECK(required->ValidateReplacement(*present).HasError());
    }

    TEST_CASE("Duplicate local import aliases cannot produce ambiguous materialized bindings", "[Extensions][ScriptModule][Imports]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        std::vector<ScriptModuleImport> imports;
        imports.push_back(Bound(fixture, true));
        imports.push_back(Bound(fixture, true));
        CHECK(ScriptModuleContext::Create(fixture.invocations, jobs, cache, LuaScriptModuleRuntimeIdentity("headless"), fixture.Policy(),
                                          std::move(imports))
                  .HasError());
    }

    TEST_CASE("Both real VMs materialize distinct canonical unavailable and incompatible optional diagnostics",
              "[Extensions][ScriptModule][Imports][TwoVm]") {
        using Identity = ScriptModuleRuntimeIdentity (*)(std::string);
        using Factory = Result<std::unique_ptr<IScriptModuleRuntime>> (*)(std::shared_ptr<ScriptModuleContext>);
        const std::array<std::pair<Identity, Factory>, 2> adapters{{{LuaScriptModuleRuntimeIdentity, CreateLuaScriptModuleRuntime},
                                                                    {JavaScriptModuleRuntimeIdentity, CreateJavaScriptModuleRuntime}}};
        for (std::size_t adapter = 0; adapter < adapters.size(); ++adapter) {
            for (const bool incompatible : {false, true}) {
                INFO(adapter);
                INFO(incompatible);
                Fixture fixture;
                JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
                ScriptModuleBindingCache cache;
                auto plan = Take(Resolve(false, incompatible, incompatible ? "2.0.0" : "1.0.0"));
                auto optional = Take(MakeUnavailableScriptModuleImport("optional", plan.serviceImports.front()));
                REQUIRE(optional.Unavailable());
                const auto code = optional.Unavailable()->code.Value();
                const auto &expected =
                    incompatible ? ExtensionErrors::BackendServiceContractMismatch : ExtensionErrors::BackendServiceUnavailable;
                CHECK(code == expected.code.Value());
                std::vector<ScriptModuleImport> imports;
                imports.push_back(std::move(optional));
                imports.push_back(Take(BindScriptModuleImport("success", fixture.Binding())));
                auto context = Take(ScriptModuleContext::Create(fixture.invocations, jobs, cache, adapters[adapter].first("headless"),
                                                                fixture.Policy(), std::move(imports)));
                auto runtime = Take(adapters[adapter].second(context));
                const std::string source =
                    adapter == 0
                        ? "local p=horo.import(\"optional\");assert(p.available==false and p.code==\"" + code +
                              "\");return horo.import(\"success\").horo.reference.sum(41)"
                        : "const p=horo.import(\"optional\");if(p.available!==false||p.code!==\"" + code +
                              "\")throw new Error(\"optional diagnostic\");return horo.import(\"success\").horo.reference.sum(41n);";
                Accept(runtime->Start(source));
                const auto result = AwaitImportEvidence(*runtime);
                REQUIRE(result.IsSuccess());
                REQUIRE(result.values.size() == 1);
                REQUIRE(result.values[0].AsSignedInteger());
                CHECK(*result.values[0].AsSignedInteger() == 42);
                CHECK(fixture.audit->calls.load() == 1);
                runtime->Close();
            }
        }
    }

    TEST_CASE("Required missing incompatible and ambiguous imports fail before VM admission with actionable canonical diagnostics",
              "[Extensions][ScriptModule][Imports][Diagnostics]") {
        struct Case {
            bool available;
            const char *version;
            bool ambiguous;
            const char *reason;
            const char *owner;
        };

        const Case cases[]{{false, "1.0.0", false, "Missing service export", "horo.reference.consumer"},
                           {true, "2.0.0", false, "Incompatible service export", "horo.reference.consumer"},
                           {true, "1.0.0", true, "Duplicate service export", "horo.reference.other"}};
        for (const auto &item : cases) {
            Fixture fixture;
            const auto result = Resolve(true, item.available, item.version, item.ambiguous);
            Actionable(result, item.reason, item.owner);
            CHECK(fixture.audit->calls.load() == 0);
        }
    }

}  // namespace Horo::Extensions::Tests
