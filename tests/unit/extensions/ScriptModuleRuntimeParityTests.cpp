#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleRuntimeFixture.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace Horo::Extensions::Tests {
    using namespace ModuleSupport;

    namespace {
        struct Adapter final {
            const char *name;
            ScriptModuleRuntimeIdentity (*identity)(std::string);
            Result<std::unique_ptr<IScriptModuleRuntime>> (*create)(std::shared_ptr<ScriptModuleContext>);
            const char *record;
            const char *async;
            const char *sum;
        };

        constexpr Adapter Adapters[]{{"Lua54", LuaScriptModuleRuntimeIdentity, CreateLuaScriptModuleRuntime,
                                      R"(local r=horo.import("reference").horo.reference
local answer=r.echo({id=horo.u64("18446744073709551615"),name="horo\0x",payload="\0\255A",active=true,ratio=-0.25,names={"a","b"},scores={{"a",1.5},{"b",-2.0}},mode=r.on,token=horo.null})
assert(answer.values[1].id.decimal=="18446744073709551615")
return answer)",
                                      R"(local r=horo.import("reference").horo.reference
return r.digest("\0\255A"))",
                                      R"(return horo.import("reference").horo.reference.sum(41))"},
                                     {"QuickJS-NG", JavaScriptModuleRuntimeIdentity, CreateJavaScriptModuleRuntime,
                                      R"(const r=horo.import("reference").horo.reference;
const answer=r.echo({id:18446744073709551615n,name:"horo\u0000x",payload:[0,255,65],active:true,ratio:-0.25,names:["a","b"],scores:[["a",1.5],["b",-2.0]],mode:r.on,token:null});
if(answer.values[0].id!==18446744073709551615n)throw new Error("uint64 mismatch");
return answer;)",
                                      R"(return await horo.import("reference").horo.reference.digest([0,255,65]);)",
                                      R"(return horo.import("reference").horo.reference.sum(41n);)"}};

        ScriptCallResult Await(IScriptModuleRuntime &runtime) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (!runtime.ResultSnapshot() && std::chrono::steady_clock::now() < deadline) {
                Accept(runtime.Poll());
                std::this_thread::yield();
            }
            auto result = runtime.ResultSnapshot();
            REQUIRE(result);
            return std::move(*result);
        }

        ScriptCallResult Run(const Adapter &adapter, const char *source) {
            Fixture fixture;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto context = fixture.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(std::move(context)));
            Accept(runtime->Start(source));
            auto result = Await(*runtime);
            runtime->Close();
            return result;
        }
    }  // namespace

    TEST_CASE("The genuine reference API has distinct parameter/result identities", "[Extensions][ScriptModule][Descriptor]") {
        const auto schema = Take(BuildScriptModuleReferenceApi("horo.reference.provider"));
        const auto *api = schema->Find("horo.reference.api");
        REQUIRE(api);
        const auto echo = std::ranges::find(api->functions, std::string{"echo"}, &ScriptExportFunctionDescriptor::id);
        REQUIRE(echo != api->functions.end());
        REQUIRE(echo->parameters.size() == 1);
        REQUIRE(echo->results.size() == 1);
        CHECK(echo->parameters[0].id == "record");
        CHECK(echo->results[0].id == "echoed");
    }

    TEST_CASE("Both real VMs project the same complete named record and uint64 without narrowing", "[Extensions][ScriptModule][TwoVm]") {
        std::optional<std::vector<std::byte>> previous;
        for (const auto &adapter : Adapters) {
            INFO(adapter.name);
            auto result = Run(adapter, adapter.record);
            REQUIRE(result.IsSuccess());
            REQUIRE(result.values.size() == 1);
            auto encoded = Take(EncodeScriptCallResult(result));
            if (previous)
                CHECK(encoded == *previous);
            previous = std::move(encoded);
        }
    }

    TEST_CASE("Both real VMs suspend and resume the same actual JobSystem-backed typed digest", "[Extensions][ScriptModule][TwoVm]") {
        std::optional<ScriptCallResult> previous;
        for (const auto &adapter : Adapters) {
            INFO(adapter.name);
            auto result = Run(adapter, adapter.async);
            REQUIRE(result.IsSuccess());
            REQUIRE(result.values.size() == 1);
            REQUIRE(result.values[0].AsUnsignedInteger());
            if (previous)
                CHECK(result == *previous);
            previous = std::move(result);
        }
    }

    TEST_CASE("Both real VMs preserve absent optional defaults through one canonical invocation", "[Extensions][ScriptModule][TwoVm]") {
        for (const auto &adapter : Adapters) {
            INFO(adapter.name);
            auto result = Run(adapter, adapter.sum);
            REQUIRE(result.IsSuccess());
            REQUIRE(result.values.size() == 1);
            REQUIRE(result.values[0].AsSignedInteger());
            CHECK(*result.values[0].AsSignedInteger() == 42);
        }
    }

    TEST_CASE("Both real VMs preserve actual native semantic errors rather than throwing native objects",
              "[Extensions][ScriptModule][TwoVm]") {
        const char *programs[]{
            R"(local out=horo.import("reference").horo.reference.reject("bounded");assert(out.error.code=="horo.reference.rejected" and out.error.message=="bounded");return out)",
            R"(const out=horo.import("reference").horo.reference.reject("bounded");if(out.error.code!=="horo.reference.rejected"||out.error.message!=="bounded")throw new Error("error projection");return out;)"};
        for (std::size_t index = 0; index < std::size(Adapters); ++index) {
            auto result = Run(Adapters[index], programs[index]);
            REQUIRE(result.error);
            CHECK(result.error->code == "horo.reference.rejected");
            CHECK(result.error->message == "bounded");
        }
    }

    TEST_CASE("Malformed actual provider results become canonical terminal errors rather than abandoned calls",
              "[Extensions][ScriptModule][ResultValidation]") {
        auto mixed = ScriptCallResult::Failure({.domain = "horo.reference", .code = "horo.reference.rejected", .message = "bad envelope"});
        mixed.values.push_back(ScriptValue{std::int64_t{1}});
        const std::array<ScriptCallResult, 3> invalid{ScriptCallResult::Success({ScriptValue::String("wrong")}),
                                                      ScriptCallResult::Failure({.domain = "", .code = "malformed", .message = "rejected"}),
                                                      std::move(mixed)};
        for (const auto &adapter : Adapters) {
            for (std::size_t index = 0; index < invalid.size(); ++index) {
                INFO(adapter.name);
                INFO(index);
                Fixture fixture;
                fixture.audit->providerResult = invalid[index];
                JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
                ScriptModuleBindingCache cache;
                auto context = fixture.Context(jobs, cache, adapter.identity("headless"));
                auto runtime = Take(adapter.create(context));
                Accept(runtime->Start(adapter.sum));
                const auto result = Await(*runtime);
                REQUIRE(result.error);
                CHECK(result.error->code != ExtensionErrors::ScriptInvocationAbandoned.code.Value());
                CHECK_FALSE(result.IsSuccess());
                CHECK(fixture.audit->calls.load() == 1);
                runtime->Close();
            }
        }
    }
}  // namespace Horo::Extensions::Tests
