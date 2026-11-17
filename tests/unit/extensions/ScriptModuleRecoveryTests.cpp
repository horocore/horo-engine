#include "ScriptModuleRecoveryFixture.h"

#include <barrier>

namespace Horo::Extensions::Tests {
    using namespace RecoverySupport;

    TEST_CASE("Both VMs preserve actual locals held functions and accepted async work across additive provider publication",
              "[Extensions][ScriptModule][Recovery][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            host.audit->block = std::make_shared<InvocationBlock>();
            ReleaseOnExit release{host.audit->block};
            Accept(runtime->Start(adapter.pending));
            REQUIRE(host.audit->block->entered.try_acquire_for(std::chrono::seconds{2}));
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            auto report = Take(previous->InspectReplacement(*next));
            CHECK(report.kind == ScriptModuleUpdateKind::Compatible);
            Commit(*runtime, next);
            CHECK_FALSE(previous->IsUsable());
            CHECK(previous->Invoke("reference", "sum", {ScriptValue{std::int64_t{41}}}).HasError());
            CHECK_FALSE(Take(runtime->PollRebindRetirement()));
            CHECK(host.audit->calls.load() == 1);
            CHECK(host.audit->shutdown.load() == 0);
            CHECK(runtime->PrepareRebind(next).HasError());
            release.Release();
            Finish(*runtime);
            CHECK(host.audit->calls.load() == 1);
            CHECK(candidate.audit->calls.load() == 2);
            REQUIRE(Take(runtime->PollRebindRetirement()));
            CHECK(next->IsUsable());
            runtime->Close();
        }
    }

    TEST_CASE("A failed or breaking candidate never closes either VM's last valid provider or executes startup again",
              "[Extensions][ScriptModule][Recovery][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            Candidate candidate{host, true};
            auto inspected = candidate.RawContext(jobs, cache, adapter.identity("headless"));
            auto report = Take(previous->InspectReplacement(*inspected));
            CHECK(report.kind == ScriptModuleUpdateKind::Breaking);
            REQUIRE(report.diagnostic);
            CHECK(report.previousFingerprints.front().fingerprint != report.candidateFingerprints.front().fingerprint);
            CHECK_FALSE(report.diagnostic->message.empty());
            CHECK(runtime->PrepareRebind(inspected).HasError());
            std::vector<ScriptModuleImport> imports;
            imports.push_back(Take(BindScriptModuleImport("reference", candidate.Binding())));
            auto rejected = previous->PrepareReplacement(std::move(imports));
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == ExtensionErrors::ScriptExportDescriptorIncompatible.code.Value());
            CHECK(previous->IsUsable());
            CHECK(runtime->PrepareRebind({}).HasError());
            Accept(runtime->Start(adapter.simple));
            Finish(*runtime);
            CHECK(host.audit->calls.load() == 1);
            CHECK(candidate.audit->calls.load() == 0);
            runtime->Close();
        }
    }

    TEST_CASE("Prepared VM updates abort without publishing and reject changed source text instead of rerunning it",
              "[Extensions][ScriptModule][Recovery][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            Accept(runtime->Start(adapter.simple));
            Finish(*runtime);
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            {
                auto prepared = Take(runtime->PrepareRebind(next));
                Accept(prepared->Validate());
            }
            CHECK(previous->IsUsable());
            CHECK(next->IsUsable());
            CHECK(host.audit->calls.load() == 1);
            CHECK(runtime->ReplaceCompatible(next, "return 999").HasError());
            CHECK(previous->IsUsable());
            CHECK(host.audit->calls.load() == 1);
            Commit(*runtime, next);
            CHECK(host.audit->calls.load() == 1);
            CHECK(candidate.audit->calls.load() == 0);
            runtime->Close();
        }
    }

    TEST_CASE("Concurrent Close before prepared publication cannot let the replacement context escape usable",
              "[Extensions][ScriptModule][Recovery][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            auto prepared = Take(runtime->PrepareRebind(next));
            Accept(prepared->Validate());
            std::thread closer{[&] {
                runtime->Close();
            }};
            closer.join();
            REQUIRE(prepared->Commit());
            CHECK_FALSE(previous->IsUsable());
            CHECK_FALSE(next->IsUsable());
            CHECK(runtime->Start(adapter.simple).HasError());
            CHECK(host.audit->calls.load() == 0);
            CHECK(candidate.audit->calls.load() == 0);
        }
    }

    TEST_CASE("Concurrent Close racing owner commit and reentrant Close during real service invocation remain terminal",
              "[Extensions][ScriptModule][Recovery][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            auto prepared = Take(runtime->PrepareRebind(next));
            Accept(prepared->Validate());
            std::barrier meet{2};
            std::thread closer{[&] {
                meet.arrive_and_wait();
                runtime->Close();
            }};
            meet.arrive_and_wait();
            const bool committed = prepared->Commit();
            closer.join();
            REQUIRE(committed);
            CHECK_FALSE(next->IsUsable());
            CHECK(runtime->Poll().HasError());
            CHECK(candidate.audit->calls.load() == 0);
        }
    }

    TEST_CASE("Reentrant close from an actual rebound member does not revive old or candidate VM admission",
              "[Extensions][ScriptModule][Recovery][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            Commit(*runtime, next);
            candidate.audit->beforeInvoke = [&] {
                runtime->Close();
            };
            auto started = runtime->Start(adapter.simple);
            CHECK((started.HasError() || runtime->ResultSnapshot().has_value()));
            CHECK_FALSE(next->IsUsable());
            CHECK_FALSE(previous->IsUsable());
            CHECK(candidate.audit->calls.load() == 1);
            candidate.audit->beforeInvoke = {};
        }
    }

    TEST_CASE("Both VM locals survive real optional provider disappearance and recovery across two accepted async operations",
              "[Extensions][ScriptModule][Recovery][Optional][TwoVm]") {
        std::size_t adapterIndex{};
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto current = Take(ScriptModuleContext::Create(host.invocations, jobs, cache, adapter.identity("headless"), host.Policy(),
                                                            OptionalImports(host, true)));
            auto runtime = Take(adapter.create(current));
            auto first = std::make_shared<InvocationBlock>(), second = std::make_shared<InvocationBlock>();
            ReleaseOnExit releaseFirst{first}, releaseSecond{second};
            auto calls = std::make_shared<std::atomic<unsigned>>();
            host.audit->beforeInvoke = [calls, first, second] {
                const auto call = ++*calls;
                if (call == 1)
                    first->Wait();
                else if (call == 2)
                    second->Wait();
            };
            const char *source = adapterIndex++ == 0 ? R"(local saved=41;local held=horo.import("reference").horo.reference.sum
horo.import("reference").horo.reference.digest("a")
assert(horo.import("optional").available==false)
horo.import("reference").horo.reference.digest("b")
assert(horo.import("optional").available==true)
return held(saved))"
                                                     : R"(const saved=41n;const held=horo.import("reference").horo.reference.sum;
await horo.import("reference").horo.reference.digest([97]);
if(horo.import("optional").available!==false)throw new Error("optional still present");
await horo.import("reference").horo.reference.digest([98]);
if(horo.import("optional").available!==true)throw new Error("optional not recovered");
return held(saved);)";
            Accept(runtime->Start(source));
            REQUIRE(first->entered.try_acquire_for(std::chrono::seconds{2}));
            auto unavailable = Take(current->PrepareReplacement(OptionalImports(host, false)));
            Commit(*runtime, unavailable);
            releaseFirst.Release();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            bool entered{};
            do {
                Accept(runtime->Poll());
                entered = second->entered.try_acquire();
                if (!entered)
                    std::this_thread::yield();
            } while (!entered && std::chrono::steady_clock::now() < deadline);
            REQUIRE(entered);
            REQUIRE(Take(runtime->PollRebindRetirement()));
            auto recovered = Take(unavailable->PrepareReplacement(OptionalImports(host, true)));
            Commit(*runtime, recovered);
            releaseSecond.Release();
            Finish(*runtime);
            CHECK(calls->load() == 3);
            host.audit->beforeInvoke = {};
            runtime->Close();
        }
    }

    TEST_CASE("Foreign-thread prepared publication is denied before mutable owner stamps are accessed",
              "[Extensions][ScriptModule][Recovery][Thread]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            auto prepared = Take(runtime->PrepareRebind(next));
            Accept(prepared->Validate());
            bool committed = true;
            std::optional<Error> denied;
            std::thread foreign{[&] {
                committed = prepared->Commit();
                auto valid = prepared->Validate();
                if (valid.HasError())
                    denied = valid.ErrorValue();
            }};
            foreign.join();
            CHECK_FALSE(committed);
            REQUIRE(denied);
            CHECK(denied->code.Value() == ExtensionErrors::ScriptInvocationThreadViolation.code.Value());
            CHECK(previous->IsUsable());
            REQUIRE(prepared->Commit());
            CHECK_FALSE(previous->IsUsable());
            CHECK(next->IsUsable());
            runtime->Close();
        }
    }

    TEST_CASE("Explicit Close cancels delivery but retains actual predecessor work until the real worker returns",
              "[Extensions][ScriptModule][Recovery][Close][TwoVm]") {
        for (const auto &adapter : Adapters) {
            Fixture host;
            JobSystem jobs{{.workerCount = 1, .maxQueuedJobs = 16}};
            ScriptModuleBindingCache cache;
            auto previous = host.Context(jobs, cache, adapter.identity("headless"));
            auto runtime = Take(adapter.create(previous));
            host.audit->block = std::make_shared<InvocationBlock>();
            ReleaseOnExit release{host.audit->block};
            Accept(runtime->Start(adapter.pending));
            REQUIRE(host.audit->block->entered.try_acquire_for(std::chrono::seconds{2}));
            Candidate candidate{host};
            auto next = candidate.Context(*previous);
            Commit(*runtime, next);
            runtime->Close();
            CHECK_FALSE(Take(runtime->PollRebindRetirement()));
            CHECK_FALSE(next->IsUsable());
            release.Release();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            bool retired{};
            do {
                retired = Take(runtime->PollRebindRetirement());
                if (!retired)
                    std::this_thread::yield();
            } while (!retired && std::chrono::steady_clock::now() < deadline);
            CHECK(retired);
            CHECK(candidate.audit->calls.load() == 0);
            CHECK(runtime->Poll().HasError());
        }
    }

}  // namespace Horo::Extensions::Tests
