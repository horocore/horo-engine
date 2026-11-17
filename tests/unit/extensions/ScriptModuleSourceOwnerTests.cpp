#include "Horo/Extensions/LuaScriptModuleRuntime.h"
#include "ScriptModuleHostTestSupport.h"
#include "ScriptModuleRuntimeFixture.h"

namespace Horo::Extensions::Tests {
    namespace {
        auto Empty(ModuleSupport::Fixture &fixture, const JobSystem &jobs, ScriptModuleBindingCache &cache,
                   std::optional<ScriptModuleExecutionOwner> source = {}) {
            return ScriptModuleContext::Create(fixture.invocations, jobs, cache, LuaScriptModuleRuntimeIdentity("headless"),
                                               fixture.Policy(), {}, {}, {}, std::move(source));
        }

        ScriptModuleExecutionOwner Source(ModuleSupport::Fixture &fixture, const ExtensionCapabilityAdmission &admission) {
            return {ModuleSupport::Take(admission.Grant({"horo.script.reference"})), BackendServiceCodeLease::Retain(fixture.audit)};
        }
    }  // namespace

    TEST_CASE("Empty-import contexts still observe actual registry shutdown", "[Extensions][ScriptModule][SourceOwner]") {
        ModuleSupport::Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto context = ModuleSupport::Take(Empty(fixture, jobs, cache));
        REQUIRE(context->IsUsable());
        fixture.invocations->BeginShutdown();
        CHECK_FALSE(context->IsUsable());
    }

    TEST_CASE("Actual source revocation fences even a context with no backend imports", "[Extensions][ScriptModule][SourceOwner]") {
        ModuleSupport::Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto context = ModuleSupport::Take(Empty(fixture, jobs, cache, Source(fixture, fixture.consumer)));
        REQUIRE(context->IsUsable());
        fixture.consumer.Revoke();
        CHECK_FALSE(context->IsUsable());
        CHECK(context->Invoke("anything", "anything", {}).HasError());
        CHECK(context->IssueHandle("anything", "horo.test.handle").HasError());
    }

    TEST_CASE("Equal source identity text from a distinct admission cannot authorize compatible replacement",
              "[Extensions][ScriptModule][SourceOwner]") {
        ModuleSupport::Fixture fixture;
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;
        auto current = ModuleSupport::Take(Empty(fixture, jobs, cache, Source(fixture, fixture.consumer)));
        auto same = ModuleSupport::Take(Empty(fixture, jobs, cache, Source(fixture, fixture.consumer)));
        ModuleSupport::Accept(current->ValidateReplacement(*same));
        auto duplicate = ModuleSupport::Admission("horo.reference.consumer", 3);
        auto other = ModuleSupport::Take(Empty(fixture, jobs, cache, Source(fixture, duplicate)));
        CHECK(current->ValidateReplacement(*other).HasError());
        auto absent = ModuleSupport::Take(Empty(fixture, jobs, cache));
        CHECK(current->ValidateReplacement(*absent).HasError());
        fixture.consumer.Revoke();
        CHECK(current->ValidateReplacement(*same).HasError());
    }

    TEST_CASE("Real zero-native-import source retirement suppresses pending builtin delivery while its native host remains open",
              "[Extensions][ScriptModule][SourceOwner][Native]") {
        using namespace ModuleHostSupport;
        SignedPackage package{ScriptModuleRuntimeKind::JavaScriptNg,
                              R"(const r=horo.import("horo.reference").horo.reference;await r.digest([0,1,2]);return r.sum(41n);)", true,
                              true};
        package.Capture();
        Interactive host{package, ScriptCapabilityProfile::Tooling};
        ScriptCapabilityScope project{ScriptCapabilityScopeKind::Project}, scope{ScriptCapabilityScopeKind::Operation};
        auto policy = OperationPolicy();
        policy.scopes = {project.Lease(), scope.Lease()};
        auto operation = Take(host.modules->Begin(package.Program(), policy));
        REQUIRE_FALSE(operation->ResultSnapshot());
        const auto actual = Take(ModuleScriptHostAuthority::Create(host.native));
        const auto receipt = host.manager->Retirement(package.id);
        REQUIRE(receipt);
        const auto retired = host.manager->RetireExtension(package.id);
        CHECK(retired.disposition != ExtensionRetirementDisposition::Active);
        Accept(actual->RequireOpenOwner());  // The actual source activation, not global host closure, is the revocation authority.
        const auto advanced = operation->Poll();
        CHECK(advanced.HasError());
        const auto result = operation->ResultSnapshot();
        REQUIRE(result);
        CHECK_FALSE(result->IsSuccess());
        CHECK(host.modules->Begin(package.Program(), policy).HasError());
        operation.reset();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (receipt->Inspect().disposition != ExtensionRetirementDisposition::Complete && std::chrono::steady_clock::now() < deadline) {
            host.manager->FinalizeRetirements();
            std::this_thread::yield();
        }
        CHECK(receipt->Inspect().disposition == ExtensionRetirementDisposition::Complete);
    }

    TEST_CASE("An actual in-flight AnyThread call retains its source code lease after owner cancellation",
              "[Extensions][ScriptModule][SourceOwner][Async]") {
        ModuleSupport::Fixture fixture;
        fixture.audit->block = std::make_shared<ModuleSupport::InvocationBlock>();
        JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
        ScriptModuleBindingCache cache;

        struct ReleaseOnExit final {
            std::shared_ptr<ModuleSupport::InvocationBlock> block;
            bool released{};

            void Open() noexcept {
                if (!released) {
                    released = true;
                    block->release.release();
                }
            }

            ~ReleaseOnExit() {
                Open();
            }
        } release{fixture.audit->block};

        auto pin = std::make_shared<int>(7);
        std::weak_ptr<int> observed = pin;
        ScriptModuleExecutionOwner source{ModuleSupport::Take(fixture.consumer.Grant({"horo.script.reference"})),
                                          BackendServiceCodeLease::Retain(pin)};
        std::vector<ScriptModuleImport> imports;
        imports.push_back(ModuleSupport::Take(BindScriptModuleImport("reference", fixture.Binding())));
        auto context =
            ModuleSupport::Take(ScriptModuleContext::Create(fixture.invocations, jobs, cache, LuaScriptModuleRuntimeIdentity("headless"),
                                                            fixture.Policy(), std::move(imports), {}, {}, std::move(source)));
        pin.reset();
        auto call = ModuleSupport::Take(context->Invoke("reference", "digest", {ScriptValue::BytesValue(std::array{std::byte{1}})}));
        const bool entered = fixture.audit->block->entered.try_acquire_for(std::chrono::seconds{3});
        REQUIRE(entered);
        context->Close();
        context.reset();
        CHECK_FALSE(observed.expired());
        fixture.consumer.Revoke();
        release.Open();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        while (!observed.expired() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        CHECK(observed.expired());
        const auto result = call.Snapshot();
        REQUIRE(result);
        CHECK(result->cancellationRequested);
    }

}  // namespace Horo::Extensions::Tests
