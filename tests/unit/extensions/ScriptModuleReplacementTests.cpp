#include "Horo/Extensions/JavaScriptModuleRuntime.h"
#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleRuntimeFixture.h"

#include <chrono>
#include <thread>

namespace Horo::Extensions::Tests {
    using namespace ModuleSupport;

    namespace {
        void Finish(IScriptModuleRuntime &runtime) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            while (!runtime.ResultSnapshot() && std::chrono::steady_clock::now() < deadline) {
                Accept(runtime.Poll());
                std::this_thread::yield();
            }
            REQUIRE(runtime.ResultSnapshot());
        }

        ScriptModuleRuntimeIdentity Identity() {
            return LuaScriptModuleRuntimeIdentity("headless");
        }

        auto Context(Fixture &fixture, const JobSystem &jobs, ScriptModuleBindingCache &cache, ScriptCapabilityContextDescriptor policy,
                     std::shared_ptr<ScriptInvocationRegistry> registry = {}) {
            std::vector<ScriptModuleImport> imports;
            imports.push_back(Take(BindScriptModuleImport("reference", fixture.Binding())));
            return ScriptModuleContext::Create(registry ? std::move(registry) : fixture.invocations, jobs, cache, Identity(),
                                               std::move(policy), std::move(imports));
        }
    }  // namespace

    TEST_CASE("Module scope admission rejects duplicates and cannot confuse distinct same-kind owners",
              "[Extensions][ScriptModule][Replacement]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto current = Take(Context(fixture, jobs, cache, fixture.Policy()));
        ScriptCapabilityScope other{ScriptCapabilityScopeKind::Project};
        auto policy = fixture.Policy();
        policy.scopes = {fixture.project.Lease(), fixture.project.Lease()};
        CHECK(Context(fixture, jobs, cache, policy).HasError());
        policy.scopes = {fixture.project.Lease(), other.Lease()};
        CHECK(Context(fixture, jobs, cache, policy).HasError());
        policy.scopes = {other.Lease(), fixture.operation.Lease()};
        auto next = Take(Context(fixture, jobs, cache, std::move(policy)));
        CHECK(current->ValidateReplacement(*next).HasError());
        CHECK_FALSE(fixture.project.Lease().Matches(other.Lease()));
        CHECK(fixture.project.Lease().Matches(fixture.project.Lease()));
    }

    TEST_CASE("Module replacement rejects foreign registry ownership and closed or dead actual scopes",
              "[Extensions][ScriptModule][Replacement]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto current = Take(Context(fixture, jobs, cache, fixture.Policy()));
        auto foreign = Take(Context(fixture, jobs, cache, fixture.Policy(), std::make_shared<ScriptInvocationRegistry>()));
        CHECK(current->ValidateReplacement(*foreign).HasError());
        auto next = Take(Context(fixture, jobs, cache, fixture.Policy()));
        current->Close();
        CHECK(current->ValidateReplacement(*next).HasError());
        fixture.project.Revoke();
        CHECK(next->ValidateReplacement(*foreign).HasError());
    }

    TEST_CASE("Compatible actual provider generation update revokes old calls and invalidates its descriptor plan",
              "[Extensions][ScriptModule][Replacement]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto current = Take(Context(fixture, jobs, cache, fixture.Policy()));
        const auto fingerprint = fixture.schema->Fingerprint();
        CHECK(cache.Stats().entries == 1);
        fixture.ReplaceProvider();
        CHECK(current->Invoke("reference", "sum", {ScriptValue{std::int64_t{1}}}).HasError());
        auto next = Take(Context(fixture, jobs, cache, fixture.Policy()));
        CHECK(fixture.schema->Fingerprint() != fingerprint);
        CHECK(cache.Stats().entries == 2);
        Accept(current->ValidateReplacement(*next));
        Accept(current->CommitReplacement(*next));
        CHECK(cache.Stats().entries == 1);
        auto call = Take(next->Invoke("reference", "sum", {ScriptValue{std::int64_t{41}}}));
        auto snapshot = call.Snapshot();
        REQUIRE(snapshot);
        REQUIRE(snapshot->terminalResult);
        CHECK(*snapshot->terminalResult->values[0].AsSignedInteger() == 42);
    }

    TEST_CASE("Both actual VM owners prepare compatible generation replacement before revoking old delivery",
              "[Extensions][ScriptModule][Replacement][TwoVm]") {
        using IdentityFactory = ScriptModuleRuntimeIdentity (*)(std::string);
        using Factory = Result<std::unique_ptr<IScriptModuleRuntime>> (*)(std::shared_ptr<ScriptModuleContext>);
        const std::array<std::pair<IdentityFactory, Factory>, 2> factories{
            {{LuaScriptModuleRuntimeIdentity, CreateLuaScriptModuleRuntime},
             {JavaScriptModuleRuntimeIdentity, CreateJavaScriptModuleRuntime}}};
        const char *programs[]{R"(return horo.import("reference").horo.reference.sum(41))",
                               R"(return horo.import("reference").horo.reference.sum(41n);)"};
        for (std::size_t index = 0; index < factories.size(); ++index) {
            Fixture fixture;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto old = fixture.Context(jobs, cache, factories[index].first("headless"));
            auto runtime = Take(factories[index].second(old));
            Accept(runtime->Start(programs[index]));
            Finish(*runtime);
            fixture.ReplaceProvider();
            auto next = fixture.Context(jobs, cache, factories[index].first("headless"));
            Accept(runtime->ReplaceCompatible(next, programs[index]));
            Finish(*runtime);
            REQUIRE(runtime->ResultSnapshot());
            CHECK(runtime->ResultSnapshot()->IsSuccess());
            CHECK_FALSE(old->IsUsable());
            runtime->Close();
        }
    }

    TEST_CASE("Reentrant backend closure may destroy the context while its synchronous call finishes",
              "[Extensions][ScriptModule][Reentrant]") {
        Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto context = fixture.Context(jobs, cache, Identity());
        fixture.audit->beforeInvoke = [&context] {
            context->Close();
            context.reset();
        };
        auto call = Take(context->Invoke("reference", "sum", {ScriptValue{std::int64_t{41}}}));
        CHECK_FALSE(context);
        CHECK(fixture.audit->calls.load() == 1);
        const auto result = call.Snapshot();
        REQUIRE(result);
        REQUIRE(result->terminalResult);
        CHECK(result->terminalResult->IsFailure());
        CHECK(result->cancellationRequested);
        fixture.audit->beforeInvoke = {};
    }

}  // namespace Horo::Extensions::Tests
