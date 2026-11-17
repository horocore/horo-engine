#include "AllocationProbe.h"
#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleRuntimeFixture.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace Horo::Extensions::Tests {
    using namespace ModuleSupport;

    namespace {
        std::optional<ScriptCallResult> Finish(IScriptModuleRuntime &vm) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (!vm.ResultSnapshot() && std::chrono::steady_clock::now() < end) {
                const auto advanced = vm.Poll();
                if (advanced.HasError())
                    return std::nullopt;
                std::this_thread::yield();
            }
            return vm.ResultSnapshot();
        }

        void RejectedJavaScript(std::string_view source) {
            Fixture fixture;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto context = fixture.Context(jobs, cache, JavaScriptModuleRuntimeIdentity("headless"));
            auto vm = Take(CreateJavaScriptModuleRuntime(context));
            const auto started = vm->Start(source);
            if (started.HasValue()) {
                const auto result = Finish(*vm);
                CHECK((!result || !result->IsSuccess()));
            }
            CHECK(fixture.audit->calls.load() == 0);
            vm->Close();
        }
    }  // namespace

    TEST_CASE("Actual JavaScript plain-data projection rejects traps and accessors before native entry",
              "[Extensions][ScriptModule][JavaScript][Projection]") {
        SECTION("proxy") {
            RejectedJavaScript(R"(const r=horo.import("reference").horo.reference;
const x=new Proxy({}, {ownKeys(){r.sum(1n);return [];}});return r.echo(x);)");
        }
        SECTION("getter") {
            RejectedJavaScript(R"(const r=horo.import("reference").horo.reference;
const x={};Object.defineProperty(x,"name",{enumerable:true,get(){r.sum(1n);return "x";}});return r.echo(x);)");
        }
        SECTION("exotic") {
            RejectedJavaScript(R"(return horo.import("reference").horo.reference.echo(new Date());)");
        }
        SECTION("sparse byte array") {
            RejectedJavaScript(R"(return await horo.import("reference").horo.reference.digest([0,,1]);)");
        }
        SECTION("array getter") {
            RejectedJavaScript(
                R"(const r=horo.import("reference").horo.reference;const x=[0];Object.defineProperty(x,"0",{get(){r.sum(1n);return 0;}});return await r.digest(x);)");
        }
    }

    TEST_CASE("Actual JavaScript scalar conversion rejects wrapped and out-of-range integers before native entry",
              "[Extensions][ScriptModule][JavaScript][Projection]") {
        const char *programs[]{R"(return horo.import("reference").horo.reference.sum(9223372036854775808n);)",
                               R"(return horo.import("reference").horo.reference.sum(-9223372036854775809n);)",
                               R"(return horo.import("reference").horo.reference.sum(Object(1n));)",
                               R"(return horo.import("reference").horo.reference.sum(41);)"};
        for (const auto source : programs)
            RejectedJavaScript(source);
    }

    TEST_CASE("Actual JavaScript runtime exposes no blocking or ambient host facilities",
              "[Extensions][ScriptModule][JavaScript][Sandbox]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto context = fixture.Context(jobs, cache, JavaScriptModuleRuntimeIdentity("headless"));
        auto vm = Take(CreateJavaScriptModuleRuntime(context));
        Accept(vm->Start(
            R"(if(typeof std!=="undefined"||typeof os!=="undefined"||typeof SharedArrayBuffer!=="undefined"||typeof Atomics!=="undefined")throw new Error("ambient authority");return horo.import("reference").horo.reference.sum(41n);)"));
        auto result = Finish(*vm);
        REQUIRE(result);
        CHECK(result->IsSuccess());
        CHECK(fixture.audit->calls.load() == 1);
        vm->Close();
    }

    TEST_CASE("Actual Lua projection refuses metatables and sparse data before native entry",
              "[Extensions][ScriptModule][Lua][Projection]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto context = fixture.Context(jobs, cache, LuaScriptModuleRuntimeIdentity("headless"));
        auto vm = Take(CreateLuaScriptModuleRuntime(context));
        SECTION("sparse bytes") {
            const auto started = vm->Start(R"(return horo.import("reference").horo.reference.digest({[1]=0,[3]=1}))");
            if (started.HasValue()) {
                const auto result = Finish(*vm);
                CHECK((!result || !result->IsSuccess()));
            }
        }
        SECTION("metatable authority absent") {
            const auto started = vm->Start(
                R"(assert(setmetatable==nil and getmetatable==nil and pcall==nil and xpcall==nil);return horo.import("reference").horo.reference.sum({}))");
            if (started.HasValue()) {
                const auto result = Finish(*vm);
                CHECK((!result || !result->IsSuccess()));
            }
        }
        CHECK(fixture.audit->calls.load() == 0);
        vm->Close();
    }

    TEST_CASE("Runtime factories reject a caller-invented build identity without VM allocation", "[Extensions][ScriptModule][Factory]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto identity = LuaScriptModuleRuntimeIdentity("headless");
        identity.implementation = ComputeSha256(std::span<const std::byte>{});
        auto context = fixture.Context(jobs, cache, identity);
        CHECK(CreateLuaScriptModuleRuntime(context).HasError());
        CHECK(CreateJavaScriptModuleRuntime(context).HasError());
        CHECK(fixture.audit->calls.load() == 0);
    }

    TEST_CASE("Both actual runtime factories contain cold identity allocation failure", "[Extensions][ScriptModule][Factory][Allocation]") {
        using Identity = ScriptModuleRuntimeIdentity (*)(std::string);
        using Factory = Result<std::unique_ptr<IScriptModuleRuntime>> (*)(std::shared_ptr<ScriptModuleContext>);
        const std::array<std::pair<Identity, Factory>, 2> factories{{{LuaScriptModuleRuntimeIdentity, CreateLuaScriptModuleRuntime},
                                                                     {JavaScriptModuleRuntimeIdentity, CreateJavaScriptModuleRuntime}}};
        for (const auto &[identity, create] : factories) {
            Fixture fixture;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto context = fixture.Context(jobs, cache, identity("headless"));
            std::optional<Result<std::unique_ptr<IScriptModuleRuntime>>> result;
            {
                Horo::Tests::AllocationProbe::ScopedFailure failure;
                result.emplace(create(context));
            }
            REQUIRE(result);
            CHECK(result->HasError());
            CHECK(fixture.audit->calls.load() == 0);
        }
    }

    TEST_CASE("Unsupported operation profiles fail canonical context admission", "[Extensions][ScriptModule][Factory]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto identity = LuaScriptModuleRuntimeIdentity("headless");
        identity.targetProfile = "unadmitted";
        std::vector<ScriptModuleImport> imports;
        imports.push_back(Take(BindScriptModuleImport("reference", fixture.Binding())));
        CHECK(ScriptModuleContext::Create(fixture.invocations, jobs, cache, identity, fixture.Policy(), std::move(imports)).HasError());
        CHECK(CreateLuaScriptModuleRuntime({}).HasError());
        CHECK(CreateJavaScriptModuleRuntime({}).HasError());
    }

}  // namespace Horo::Extensions::Tests
