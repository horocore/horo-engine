#include "ScriptModuleHostTestSupport.h"

namespace Horo::Extensions::Tests {
    using namespace ModuleHostSupport;

    namespace {
        constexpr ScriptModuleRuntimeKind Vms[]{ScriptModuleRuntimeKind::Lua54, ScriptModuleRuntimeKind::JavaScriptNg};

        std::string NativeSource(ScriptModuleRuntimeKind runtime) {
            return runtime == ScriptModuleRuntimeKind::Lua54
                       ? R"(return horo.import("backend-import").com.horo.concurrent.api.add(20,22))"
                       : R"(return horo.import("backend-import").com.horo.concurrent.api.add(20n,22n);)";
        }

        void Answer42(const ScriptCallResult &result) {
            REQUIRE(result.IsSuccess());
            REQUIRE(result.values.size() == 1);
            REQUIRE(result.values[0].AsSignedInteger());
            CHECK(*result.values[0].AsSignedInteger() == 42);
        }
    }  // namespace

    TEST_CASE("Both module VMs run through the genuine signed Headless host native import",
              "[Extensions][ScriptModule][Headless][Native]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, NativeSource(vm)};
            package.Capture();
            Headless host{package};
            const auto snapshot = host.host->Inspect();
            REQUIRE(snapshot.state == HeadlessExtensionHostState::Ready);
            REQUIRE(snapshot.loadedExtensions == std::vector<std::string>{"com.horo.concurrent"});
            auto operation = Take(host.modules->Begin(package.Program(), host.Policy()));
            Answer42(Await(*operation));
        }
    }

    TEST_CASE("Both module VMs share the requested general API in the actual Headless composition",
              "[Extensions][ScriptModule][Headless]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, SumSource(vm)};
            package.Capture();
            Headless host{package};
            auto operation = Take(host.modules->Begin(package.Program(), host.Policy()));
            Answer42(Await(*operation));
            auto second = host.modules->Begin(package.Program(), host.Policy());
            REQUIRE(second.HasValue());
            Answer42(Await(*second.Value()));
            CHECK(host.modules->CacheStats().hits > 0);
        }
    }

    TEST_CASE("Signed source requesting the builtin cannot bypass either actual operation policy or consent",
              "[Extensions][ScriptModule][Headless][Policy]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, SumSource(ScriptModuleRuntimeKind::Lua54)};
        package.Capture();
        Headless host{package};
        auto policy = host.Policy();
        SECTION("native consent missing") {
            policy.nativeConsent = false;
        }
        SECTION("script consent missing") {
            policy.scriptConsent = false;
        }
        SECTION("project capability missing") {
            policy.projectCapabilities.clear();
        }
        SECTION("context capability missing") {
            policy.contextCapabilities.clear();
        }
        CHECK(host.modules->Begin(package.Program(), std::move(policy)).HasError());
    }

    TEST_CASE("A general builtin cannot appear for an unrequesting signed native module", "[Extensions][ScriptModule][Headless][Policy]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, SumSource(vm), false};
            package.Capture();
            Headless host{package};
            auto operation = Take(host.modules->Begin(package.Program(), host.Policy()));
            const auto result = Await(*operation);
            CHECK_FALSE(result.IsSuccess());
        }
    }

    TEST_CASE("Actual post-load trust revocation prevents cold module operation admission",
              "[Extensions][ScriptModule][Headless][Security]") {
        SignedPackage package{ScriptModuleRuntimeKind::JavaScriptNg, SumSource(ScriptModuleRuntimeKind::JavaScriptNg)};
        package.Capture();
        Headless host{package};
        const auto program = package.Program();
        package.Trust(true);
        CHECK(host.modules->Begin(program, host.Policy()).HasError());
    }

    TEST_CASE("Same path and manifest version cannot hide a signed native binary replacement from active evidence",
              "[Extensions][ScriptModule][Headless][Security]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, SumSource(ScriptModuleRuntimeKind::Lua54)};
        package.Capture();
        Headless host{package};
        const auto suffix = std::filesystem::path{HORO_NATIVE_SERVICE_PROVIDER_FIXTURE}.extension().string();
        const auto native = package.package / ("backend" + suffix);
        {
            std::ofstream changed{native, std::ios::binary | std::ios::app};
            REQUIRE(changed);
            changed.put('x');
        }
        package.Sign(native);
        package.Capture();
        CHECK(host.modules->Begin(package.Program(), host.Policy()).HasError());
    }

    TEST_CASE("Retained Headless module operation cannot reenter the retired actual native host",
              "[Extensions][ScriptModule][Headless][Lifetime]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, NativeSource(ScriptModuleRuntimeKind::Lua54)};
        package.Capture();
        Headless host{package};
        auto program = package.Program();
        auto operation = Take(host.modules->Begin(program, host.Policy()));
        Answer42(Await(*operation));
        host.host->Shutdown();
        CHECK(host.modules->Begin(program, host.Policy()).HasError());
        operation->Cancel();
    }
}  // namespace Horo::Extensions::Tests
