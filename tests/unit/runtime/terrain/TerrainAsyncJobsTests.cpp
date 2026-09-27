#include "Horo/Terrain/TerrainAsyncJobs.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace Horo::Terrain {
    namespace {
        using namespace std::chrono_literals;

        TerrainDatasetId Dataset() {
            SerializedTerrainIdentity bytes{};
            bytes[0] = 1;
            return TerrainDatasetId::Create(bytes).Value();
        }

        TerrainAsyncWorkFence Fence(const std::uint64_t content = 1) {
            return {
                .runtime = {Dataset(), {2, 3}},
                .revisions = {TerrainContentRevision::Create(content).Value(), TerrainResidencyRevision::Create(1).Value(),
                              TerrainMutationRevision::Create(1).Value(), TerrainCapabilityRevision::Create(1).Value()},
                .registry = {TerrainFoliageRegistryInstanceId::Create(1).Value(), TerrainFoliageRegistryRevision::Create(1).Value()},
            };
        }

        TerrainFoliageCapabilitySet Capabilities() {
            constexpr std::array granted{TerrainFoliageCapability::TerrainRuntime, TerrainFoliageCapability::TerrainQuery};
            return TerrainFoliageCapabilitySet::Create(granted).Value();
        }

        TerrainAsyncWorkRequest Request(std::function<Result<void>(const JobExecutionContext &)> prepare,
                                        std::function<Result<void>(const CancellationToken &)> publish) {
            return {.workUnits = 4,
                    .requiredCapabilities =
                        TerrainFoliageCapabilitySet::Create(std::array{TerrainFoliageCapability::TerrainRuntime}).Value(),
                    .prepare = std::move(prepare),
                    .publish = std::move(publish)};
        }

        TerrainAsyncWorkSnapshot AdvanceUntilTerminal(TerrainAsyncJobs &jobs, const TerrainAsyncWorkId id) {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto snapshot = jobs.Advance(id);
                REQUIRE(snapshot.HasValue());
                if (snapshot.Value().IsTerminal())
                    return snapshot.Value();
                std::this_thread::sleep_for(1ms);
            }
            FAIL("Terrain work did not reach a terminal state within three seconds");
        }

        struct WorkerGate final {
            std::mutex mutex;
            std::condition_variable condition;
            bool entered{};
            bool released{};

            void WaitUntilEntered() {
                std::unique_lock lock{mutex};
                REQUIRE(condition.wait_for(lock, 3s, [this] {
                    return entered;
                }));
            }

            void Release() {
                {
                    std::lock_guard lock{mutex};
                    released = true;
                }
                condition.notify_all();
            }

            Result<void> Prepare(const JobExecutionContext &context, const bool failAfterRelease = false) {
                std::unique_lock lock{mutex};
                entered = true;
                condition.notify_all();
                condition.wait(lock, [this] {
                    return released;
                });
                if (failAfterRelease)
                    return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
                return context.Cancellation().IsCancellationRequested() ? JobCancelled() : Result<void>::Success();
            }

            ~WorkerGate() {
                Release();
            }
        };

        TEST_CASE("Terrain async jobs publish once on the owner lane after durable worker completion", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto created = TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities());
            REQUIRE(created.HasValue());
            auto jobs = std::move(created).Value();
            const std::thread::id owner = std::this_thread::get_id();
            std::atomic<int> prepared{};
            int publications{};
            auto submitted = jobs->SubmitCook(Request([&](const JobExecutionContext &) {
                ++prepared;
                return Result<void>::Success();
            }, [&](const CancellationToken &cancellation) {
                REQUIRE_FALSE(cancellation.IsCancellationRequested());
                REQUIRE(std::this_thread::get_id() == owner);
                ++publications;
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Succeeded);
            REQUIRE(terminal.kind == TerrainAsyncWorkKind::Cook);
            REQUIRE(terminal.jobId != 0);
            REQUIRE(prepared == 1);
            REQUIRE(publications == 1);
            REQUIRE(jobs->Advance(submitted.Value()).Value().state == TerrainAsyncWorkState::Succeeded);
            REQUIRE(publications == 1);
            REQUIRE(jobs->Forget(submitted.Value()).HasValue());
            REQUIRE(jobs->Snapshot(submitted.Value()).ErrorValue().code.Value() == TerrainErrors::WorkUnknown.code.Value());
        }

        TEST_CASE("Terrain async admission is bounded and preserves exact capability requirements", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto created = TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities(), {.maximumTracked = 1, .maximumWorkUnits = 4});
            REQUIRE(created.HasValue());
            auto jobs = std::move(created).Value();
            auto normal = [](const JobExecutionContext &) {
                return Result<void>::Success();
            };
            auto publish = [](const CancellationToken &) {
                return Result<void>::Success();
            };
            auto invalid = Request(normal, publish);
            invalid.workUnits = 5;
            REQUIRE(jobs->SubmitCook(std::move(invalid)).ErrorValue().code.Value() == TerrainErrors::WorkInvalid.code.Value());
            auto unsupported = Request(normal, publish);
            unsupported.requiredCapabilities =
                TerrainFoliageCapabilitySet::Create(std::array{TerrainFoliageCapability::GpuIndirectCulling}).Value();
            REQUIRE(jobs->SubmitCook(std::move(unsupported)).ErrorValue().code.Value() ==
                    TerrainErrors::CapabilityUnsupported.code.Value());
            auto accepted = jobs->SubmitCook(Request(normal, publish));
            REQUIRE(accepted.HasValue());
            REQUIRE(jobs->SubmitCook(Request(normal, publish)).ErrorValue().code.Value() == TerrainErrors::CapacityExceeded.code.Value());
            REQUIRE(AdvanceUntilTerminal(*jobs, accepted.Value()).state == TerrainAsyncWorkState::Succeeded);
            REQUIRE(jobs->Forget(accepted.Value()).HasValue());
            REQUIRE(jobs->SubmitCook(Request(normal, publish)).HasValue());
        }

        TEST_CASE("Terrain worker failure retains its typed error and never publishes", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            bool published{};
            auto submitted = jobs->SubmitLoad(Request([](const JobExecutionContext &) {
                return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            }, [&](const CancellationToken &) {
                published = true;
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Failed);
            REQUIRE(terminal.kind == TerrainAsyncWorkKind::Load);
            REQUIRE(terminal.error.has_value());
            REQUIRE(terminal.error->code.Value() == TerrainErrors::DescriptorInvalid.code.Value());
            REQUIRE_FALSE(published);
        }

        TEST_CASE("Terrain load preparation publishes only after its worker succeeds", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            std::atomic<bool> decoded{};
            bool published{};
            auto submitted = jobs->SubmitLoad(Request([&](const JobExecutionContext &) {
                decoded = true;
                return Result<void>::Success();
            }, [&](const CancellationToken &token) {
                REQUIRE(decoded);
                REQUIRE_FALSE(token.IsCancellationRequested());
                published = true;
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.kind == TerrainAsyncWorkKind::Load);
            REQUIRE(terminal.state == TerrainAsyncWorkState::Succeeded);
            REQUIRE(published);
        }

        TEST_CASE("Terrain publication failure retains the typed error and one terminal result", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            int publications{};
            auto submitted = jobs->SubmitLoad(Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [&](const CancellationToken &) {
                ++publications;
                return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
            }));
            REQUIRE(submitted.HasValue());
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Failed);
            REQUIRE(terminal.error->code.Value() == TerrainErrors::DescriptorInvalid.code.Value());
            REQUIRE(jobs->Advance(submitted.Value()).Value().state == TerrainAsyncWorkState::Failed);
            REQUIRE(publications == 1);
        }

        TEST_CASE("Terrain owner publication cancellation retains its typed cause and never retries", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            int publications{};
            auto submitted = jobs->SubmitEditPreview(Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [&](const CancellationToken &) {
                ++publications;
                return JobCancelled(MakeError(TerrainErrors::RevisionStale));
            }));
            REQUIRE(submitted.HasValue());
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(terminal.error.has_value());
            REQUIRE(IsJobCancelled(*terminal.error));
            REQUIRE(terminal.error->cause.Get() != nullptr);
            REQUIRE(terminal.error->cause.Get()->code.Value() == TerrainErrors::RevisionStale.code.Value());
            REQUIRE(jobs->Advance(submitted.Value()).Value().state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(publications == 1);
        }

        TEST_CASE("Terrain owner publication cannot reenter fence replacement or advance", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            TerrainAsyncWorkId id;
            std::string replaceCode;
            std::string advanceCode;
            auto submitted = jobs->SubmitLoad(Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [&](const CancellationToken &) {
                replaceCode = jobs->ReplaceFence(Fence(2), Capabilities()).ErrorValue().code.Value();
                advanceCode = jobs->Advance(id).ErrorValue().code.Value();
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            id = submitted.Value();
            REQUIRE(AdvanceUntilTerminal(*jobs, id).state == TerrainAsyncWorkState::Succeeded);
            REQUIRE(replaceCode == TerrainErrors::WorkNotReady.code.Value());
            REQUIRE(advanceCode == TerrainErrors::WorkNotReady.code.Value());
            REQUIRE(jobs->ReplaceFence(Fence(2), Capabilities()).HasValue());
        }

        TEST_CASE("Terrain owner shutdown during publication cancels before commit", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            bool published{};
            auto submitted = jobs->SubmitEditPreview(Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [&](const CancellationToken &token) {
                jobs->BeginShutdown();
                if (token.IsCancellationRequested())
                    return JobCancelled();
                published = true;
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(IsJobCancelled(*terminal.error));
            REQUIRE_FALSE(published);
            REQUIRE(jobs->IsDrained());
        }

        TEST_CASE("Terrain pre-cancelled parent rejects admission and foreign lane cannot publish", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            CancellationSource parent;
            parent.RequestCancellation();
            auto request = Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [](const CancellationToken &) {
                return Result<void>::Success();
            });
            request.parentCancellation = parent.Token();
            REQUIRE(IsJobCancelled(jobs->SubmitCook(std::move(request)).ErrorValue()));
            auto submitted = jobs->SubmitCook(Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [](const CancellationToken &) {
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            std::string foreignCode;
            std::thread foreign([&] {
                foreignCode = jobs->Advance(submitted.Value()).ErrorValue().code.Value();
            });
            foreign.join();
            REQUIRE(foreignCode == TerrainErrors::WorkWrongThread.code.Value());
            REQUIRE(AdvanceUntilTerminal(*jobs, submitted.Value()).state == TerrainAsyncWorkState::Succeeded);
        }

        TEST_CASE("Terrain replacement cancels stale generation and preserves the last publication", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            WorkerGate gate;
            int publication{};
            auto stale = jobs->SubmitEditPreview(Request([&](const JobExecutionContext &context) {
                return gate.Prepare(context);
            }, [&](const CancellationToken &) {
                publication = 1;
                return Result<void>::Success();
            }));
            REQUIRE(stale.HasValue());
            gate.WaitUntilEntered();
            REQUIRE(jobs->ReplaceFence(Fence(2), Capabilities()).HasValue());
            gate.Release();
            const auto cancelled = AdvanceUntilTerminal(*jobs, stale.Value());
            REQUIRE(cancelled.state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(cancelled.kind == TerrainAsyncWorkKind::EditPreview);
            REQUIRE(cancelled.error.has_value());
            REQUIRE(IsJobCancelled(*cancelled.error));
            REQUIRE(cancelled.error->cause.Get() != nullptr);
            REQUIRE(cancelled.error->cause.Get()->code.Value() == TerrainErrors::RevisionStale.code.Value());
            REQUIRE(publication == 0);

            auto current = jobs->SubmitEditPreview(Request([](const JobExecutionContext &) {
                return Result<void>::Success();
            }, [&](const CancellationToken &) {
                publication = 2;
                return Result<void>::Success();
            }));
            REQUIRE(current.HasValue());
            REQUIRE(AdvanceUntilTerminal(*jobs, current.Value()).state == TerrainAsyncWorkState::Succeeded);
            REQUIRE(jobs->Snapshot(current.Value()).Value().kind == TerrainAsyncWorkKind::EditPreview);
            REQUIRE(publication == 2);
        }

        TEST_CASE("Terrain running cancellation dominates a late preparation failure", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            WorkerGate gate;
            int publications{};
            auto submitted = jobs->SubmitCook(Request([&](const JobExecutionContext &context) {
                return gate.Prepare(context, true);
            }, [&](const CancellationToken &) {
                ++publications;
                return Result<void>::Success();
            }));
            REQUIRE(submitted.HasValue());
            gate.WaitUntilEntered();
            const TerrainAsyncJobs &owner = *jobs;
            REQUIRE(owner.RequestCancel(submitted.Value()).HasValue());
            gate.Release();
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(terminal.error.has_value());
            REQUIRE(IsJobCancelled(*terminal.error));
            REQUIRE(terminal.error->cause.Get() != nullptr);
            REQUIRE(terminal.error->cause.Get()->code.Value() == TerrainErrors::DescriptorInvalid.code.Value());
            REQUIRE(jobs->Advance(submitted.Value()).Value().state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(publications == 0);
        }

        TEST_CASE("Terrain fence replacement rejects revision rollback and unversioned capability drift", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(2), Capabilities())).Value();
            REQUIRE(jobs->ReplaceFence(Fence(1), Capabilities()).ErrorValue().code.Value() == TerrainErrors::RevisionStale.code.Value());
            const auto changed = TerrainFoliageCapabilitySet::Create(std::array{TerrainFoliageCapability::TerrainRuntime}).Value();
            REQUIRE(jobs->ReplaceFence(Fence(2), changed).ErrorValue().code.Value() == TerrainErrors::WorkInvalid.code.Value());
            auto advanced = Fence(2);
            advanced.revisions.capability = TerrainCapabilityRevision::Create(2).Value();
            REQUIRE(jobs->ReplaceFence(advanced, changed).HasValue());
        }

        TEST_CASE("Terrain parent cancellation and shutdown remain cancellation, not failure", "[unit][terrain][jobs]") {
            JobSystem scheduler({.workerCount = 1, .maxQueuedJobs = 4});
            auto jobs = std::move(TerrainAsyncJobs::Create(scheduler, Fence(), Capabilities())).Value();
            WorkerGate gate;
            CancellationSource parent;
            bool published{};
            auto request = Request([&](const JobExecutionContext &context) {
                return gate.Prepare(context);
            }, [&](const CancellationToken &) {
                published = true;
                return Result<void>::Success();
            });
            request.parentCancellation = parent.Token();
            auto submitted = jobs->SubmitCook(std::move(request));
            REQUIRE(submitted.HasValue());
            gate.WaitUntilEntered();
            parent.RequestCancellation();
            jobs->BeginShutdown();
            gate.Release();
            const auto terminal = AdvanceUntilTerminal(*jobs, submitted.Value());
            REQUIRE(terminal.state == TerrainAsyncWorkState::Cancelled);
            REQUIRE(terminal.error.has_value());
            REQUIRE(IsJobCancelled(*terminal.error));
            REQUIRE_FALSE(published);
            REQUIRE(jobs->IsDrained());
            REQUIRE(jobs->SubmitCook(Request(
                                         [](const JobExecutionContext &) {
                return Result<void>::Success();
            },
                                         [](const CancellationToken &) {
                return Result<void>::Success();
            }))
                        .ErrorValue()
                        .code.Value() == TerrainErrors::LifecycleUnavailable.code.Value());
            jobs->BeginShutdown();
        }
    }  // namespace
}  // namespace Horo::Terrain
