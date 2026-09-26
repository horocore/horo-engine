#include "Horo/PCG/PCGAsyncOperation.h"
#include "Horo/PCG/PCGErrors.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <future>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Horo::PCG {
    namespace {
        template <typename Identity> [[nodiscard]] Identity Id(const std::uint64_t value) {
            auto created = Identity::Create(value);
            REQUIRE(created.HasValue());
            return created.Value();
        }

        [[nodiscard]] Sha256Digest Digest(const std::uint8_t value) {
            Sha256Digest digest{};
            digest.bytes.fill(value);
            return digest;
        }

        [[nodiscard]] PCGAsyncFence Fence(const std::uint64_t revision = 1, const std::uint64_t runtime = 1, const std::uint8_t digest = 1,
                                          const std::uint64_t scene = 1, const std::uint64_t cell = 1, const std::uint64_t graph = 1) {
            return {Id<PCGAsyncSceneId>(scene),
                    Id<GenerationCellId>(cell),
                    {Id<GraphId>(graph), Id<GraphRevision>(revision)},
                    Digest(digest),
                    runtime,
                    1,
                    1,
                    1};
        }

        [[nodiscard]] PCGAsyncRequest Request(const PCGAsyncFence &fence, std::atomic<int> &publications) {
            PCGAsyncRequest request;
            request.fence = fence;
            request.workUnits = 7;
            request.prepare = [](PCGAsyncPrepareContext &) {
                return Result<void>::Success();
            };
            request.publish = [&publications](const CancellationToken &) {
                ++publications;
                return Result<void>::Success();
            };
            return request;
        }

        [[nodiscard]] PCGAsyncSnapshot AwaitTerminal(PCGAsyncOperations &operations, const PCGAsyncOperationId id) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto result = operations.Advance(id);
                REQUIRE(result.HasValue());
                if (result.Value().terminal.has_value())
                    return result.Value();
                std::this_thread::yield();
            }
            FAIL("PCG async operation did not reach a terminal result");
            return {};
        }

        void AwaitAll(PCGAsyncOperations &operations) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                auto pending = operations.AdvanceAll();
                REQUIRE(pending.HasValue());
                if (pending.Value() == 0)
                    return;
                std::this_thread::yield();
            }
            FAIL("PCG async completion sweep did not drain");
        }

        template <typename T> void RequireCode(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }

        [[nodiscard]] JobSystem Jobs() {
            return JobSystem({.workerCount = 2, .maxQueuedJobs = 16, .maxRetainedTerminalJobs = 16});
        }

        [[nodiscard]] std::unique_ptr<PCGAsyncOperations> Operations(JobSystem &jobs) {
            auto created = PCGAsyncOperations::Create(jobs);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }
    }  // namespace

    TEST_CASE("PCG async success accounts structured children before immutable owner publication", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> children{0};
        std::atomic<int> publications{0};
        auto request = Request(fence, publications);
        request.prepare = [&children](PCGAsyncPrepareContext &context) {
            for (int index = 0; index < 2; ++index) {
                auto spawned = context.SpawnChild([&children](const CancellationToken &) {
                    ++children;
                    return Result<void>::Success();
                });
                if (spawned.HasError())
                    return Result<void>::Failure(spawned.ErrorValue());
            }
            return Result<void>::Success();
        };
        auto submitted = operations->Submit(PCGAsyncKind::Evaluate, std::move(request));
        REQUIRE(submitted.HasValue());
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Succeeded);
        CHECK(terminal.terminal->acceptedChildren == 2);
        CHECK(terminal.terminal->workUnits == 7);
        CHECK(children == 2);
        CHECK(publications == 1);
        REQUIRE(operations->RequestCancel(submitted.Value()).HasValue());
        const auto again = operations->Advance(submitted.Value());
        REQUIRE(again.HasValue());
        CHECK(again.Value().state == PCGAsyncState::Succeeded);
        CHECK(publications == 1);
        CHECK(operations->IsDrained());
        REQUIRE(operations->Forget(submitted.Value()).HasValue());
        RequireCode(operations->Snapshot(submitted.Value()), PCGErrors::AsyncUnknown);
    }

    TEST_CASE("PCG async child failure drains siblings and never publishes", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        auto request = Request(fence, publications);
        request.prepare = [](PCGAsyncPrepareContext &context) {
            auto spawned = context.SpawnChild([](const CancellationToken &) {
                return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
            });
            return spawned.HasValue() ? Result<void>::Success() : Result<void>::Failure(spawned.ErrorValue());
        };
        auto submitted = operations->Submit(PCGAsyncKind::Evaluate, std::move(request));
        REQUIRE(submitted.HasValue());
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Failed);
        CHECK(terminal.terminal->acceptedChildren == 1);
        REQUIRE(terminal.terminal->error.has_value());
        CHECK(terminal.terminal->error->code.Value() == PCGErrors::AsyncInvalid.code.Value());
        CHECK(publications == 0);
        CHECK(operations->IsDrained());
    }

    TEST_CASE("PCG async parent cancellation cannot hide an accepted child failure", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        std::promise<void> childStarted;
        std::promise<void> release;
        const auto gate = release.get_future().share();
        auto request = Request(fence, publications);
        request.prepare = [&childStarted, &release, gate](PCGAsyncPrepareContext &context) {
            auto spawned = context.SpawnChild([&childStarted, gate](const CancellationToken &) {
                childStarted.set_value();
                gate.wait();
                return Result<void>::Failure(MakeError(PCGErrors::AsyncInvalid));
            });
            if (spawned.HasError())
                return Result<void>::Failure(spawned.ErrorValue());
            childStarted.get_future().wait();
            release.set_value();
            return JobCancelled();
        };
        auto submitted = operations->Submit(PCGAsyncKind::Evaluate, std::move(request));
        REQUIRE(submitted.HasValue());
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Failed);
        CHECK(terminal.terminal->acceptedChildren == 1);
        REQUIRE(terminal.terminal->error.has_value());
        CHECK(terminal.terminal->error->code.Value() == PCGErrors::AsyncInvalid.code.Value());
        CHECK(publications == 0);
    }

    TEST_CASE("PCG async cancellation at owner publication is terminal cancellation without retry", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> attempts{0};
        std::atomic<int> publications{0};
        auto request = Request(fence, publications);
        request.publish = [&attempts](const CancellationToken &) {
            ++attempts;
            return JobCancelled(MakeError(PCGErrors::AsyncClosed));
        };
        auto submitted = operations->Submit(PCGAsyncKind::Preview, std::move(request));
        REQUIRE(submitted.HasValue());
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Cancelled);
        REQUIRE(terminal.terminal->error.has_value());
        CHECK(IsJobCancelled(*terminal.terminal->error));
        CHECK(terminal.terminal->error->cause.Get() != nullptr);
        CHECK(attempts == 1);
        CHECK(publications == 0);
        const auto again = operations->Advance(submitted.Value());
        REQUIRE(again.HasValue());
        CHECK(again.Value().state == PCGAsyncState::Cancelled);
        CHECK(attempts == 1);
    }

    TEST_CASE("PCG async running cancellation drains work without publishing", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        auto request = Request(fence, publications);
        std::promise<void> started;
        std::promise<void> release;
        const auto gate = release.get_future().share();
        request.prepare = [&started, gate](PCGAsyncPrepareContext &context) {
            started.set_value();
            gate.wait();
            return context.Cancellation().IsCancellationRequested() ? JobCancelled() : Result<void>::Success();
        };
        auto submitted = operations->Submit(PCGAsyncKind::Evaluate, std::move(request));
        REQUIRE(submitted.HasValue());
        started.get_future().wait();
        REQUIRE(operations->RequestCancel(submitted.Value()).HasValue());
        CHECK_FALSE(operations->IsDrained());
        release.set_value();
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Cancelled);
        CHECK(publications == 0);
        CHECK(operations->IsDrained());
    }

    TEST_CASE("PCG async exact source digest and replacement fence reject stale publication", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto oldFence = Fence();
        REQUIRE(operations->RegisterFence(oldFence).HasValue());
        std::atomic<int> publications{0};
        auto request = Request(oldFence, publications);
        std::promise<void> started;
        std::promise<void> release;
        const auto gate = release.get_future().share();
        request.prepare = [&started, gate](PCGAsyncPrepareContext &) {
            started.set_value();
            gate.wait();
            return Result<void>::Success();
        };
        auto submitted = operations->Submit(PCGAsyncKind::Evaluate, std::move(request));
        REQUIRE(submitted.HasValue());
        started.get_future().wait();
        auto sameRevisionDifferentDigest = oldFence;
        sameRevisionDifferentDigest.sourceDigest = Digest(2);
        RequireCode(operations->ReplaceFence(sameRevisionDifferentDigest), PCGErrors::AsyncStale);
        auto replacement = Fence(2, 1, 2);
        REQUIRE(operations->ReplaceFence(replacement).HasValue());
        release.set_value();
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        CHECK(terminal.state != PCGAsyncState::Succeeded);
        CHECK(publications == 0);
        auto staleRequest = Request(oldFence, publications);
        RequireCode(operations->Submit(PCGAsyncKind::Evaluate, std::move(staleRequest)), PCGErrors::AsyncStale);
        auto fresh = operations->Submit(PCGAsyncKind::Evaluate, Request(replacement, publications));
        REQUIRE(fresh.HasValue());
        CHECK(AwaitTerminal(*operations, fresh.Value()).state == PCGAsyncState::Succeeded);
        CHECK(publications == 1);
    }

    TEST_CASE("PCG async graph cell scene and host invalidation close publication until drained", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto first = Fence(1, 1, 1, 1, 1, 1);
        const auto second = Fence(1, 1, 1, 1, 1, 2);
        const auto otherCell = Fence(1, 1, 1, 1, 2, 1);
        const auto otherScene = Fence(1, 1, 1, 2, 1, 1);
        for (const auto &fence : {first, second, otherCell, otherScene})
            REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        const auto one = operations->Submit(PCGAsyncKind::Cook, Request(first, publications));
        const auto two = operations->Submit(PCGAsyncKind::Load, Request(second, publications));
        const auto three = operations->Submit(PCGAsyncKind::Evaluate, Request(otherCell, publications));
        const auto four = operations->Submit(PCGAsyncKind::Preview, Request(otherScene, publications));
        REQUIRE(one.HasValue());
        REQUIRE(two.HasValue());
        REQUIRE(three.HasValue());
        REQUIRE(four.HasValue());

        REQUIRE(operations->InvalidateGraph(first.scene, first.cell, first.graph.graph).HasValue());
        CHECK_FALSE(operations->IsGraphDrained(first.scene, first.cell, first.graph.graph));
        CHECK(AwaitTerminal(*operations, one.Value()).state != PCGAsyncState::Succeeded);
        CHECK(operations->IsGraphDrained(first.scene, first.cell, first.graph.graph));
        REQUIRE(operations->InvalidateCell(first.scene, first.cell).HasValue());
        CHECK_FALSE(operations->IsCellDrained(first.scene, first.cell));
        CHECK(AwaitTerminal(*operations, two.Value()).state != PCGAsyncState::Succeeded);
        CHECK(operations->IsCellDrained(first.scene, first.cell));
        REQUIRE(operations->InvalidateScene(first.scene).HasValue());
        CHECK_FALSE(operations->IsSceneDrained(first.scene));
        CHECK(AwaitTerminal(*operations, three.Value()).state != PCGAsyncState::Succeeded);
        CHECK(operations->IsSceneDrained(first.scene));
        REQUIRE(operations->BeginShutdown().HasValue());
        CHECK(AwaitTerminal(*operations, four.Value()).state != PCGAsyncState::Succeeded);
        CHECK(publications == 0);
        CHECK(operations->IsDrained());
        RequireCode(operations->Submit(PCGAsyncKind::Cook, Request(first, publications)), PCGErrors::AsyncClosed);
    }

    TEST_CASE("PCG async reentrant replacement or shutdown cannot change a publication fence", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        std::atomic<bool> reentrantChecksPassed{false};
        auto request = Request(fence, publications);
        request.publish = [&operations, fence, &publications, &reentrantChecksPassed](const CancellationToken &) {
            auto newer = fence;
            newer.inputGeneration = 2;
            const auto replacement = operations->ReplaceFence(newer);
            const auto shutdown = operations->BeginShutdown();
            reentrantChecksPassed = replacement.HasError() && shutdown.HasError() &&
                                    replacement.ErrorValue().code.Value() == PCGErrors::AsyncNotReady.code.Value() &&
                                    shutdown.ErrorValue().code.Value() == PCGErrors::AsyncNotReady.code.Value();
            ++publications;
            return Result<void>::Success();
        };
        auto submitted = operations->Submit(PCGAsyncKind::Evaluate, std::move(request));
        REQUIRE(submitted.HasValue());
        CHECK(AwaitTerminal(*operations, submitted.Value()).state == PCGAsyncState::Succeeded);
        CHECK(publications == 1);
        CHECK(reentrantChecksPassed);
    }

    TEST_CASE("PCG async closed scope reopens only after old completion drains with a newer fence", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        auto submitted = operations->Submit(PCGAsyncKind::Load, Request(fence, publications));
        REQUIRE(submitted.HasValue());
        REQUIRE(operations->InvalidateCell(fence.scene, fence.cell).HasValue());
        auto newer = fence;
        newer.runtimeGeneration = 2;
        RequireCode(operations->RegisterFence(newer), PCGErrors::AsyncNotReady);
        CHECK(AwaitTerminal(*operations, submitted.Value()).state != PCGAsyncState::Succeeded);
        RequireCode(operations->RegisterFence(fence), PCGErrors::AsyncStale);
        REQUIRE(operations->RegisterFence(newer).HasValue());
        auto fresh = operations->Submit(PCGAsyncKind::Load, Request(newer, publications));
        REQUIRE(fresh.HasValue());
        CHECK(AwaitTerminal(*operations, fresh.Value()).state == PCGAsyncState::Succeeded);
        CHECK(publications == 1);
    }

    TEST_CASE("PCG async publication exception becomes one typed failure", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> publications{0};
        std::atomic<int> attempts{0};
        auto request = Request(fence, publications);
        request.publish = [&attempts](const CancellationToken &) -> Result<void> {
            ++attempts;
            throw std::runtime_error("owner failure");
        };
        auto submitted = operations->Submit(PCGAsyncKind::Cook, std::move(request));
        REQUIRE(submitted.HasValue());
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Failed);
        REQUIRE(terminal.terminal->error.has_value());
        CHECK(terminal.terminal->error->code.Value() == PCGErrors::AsyncPublicationFailed.code.Value());
        CHECK(operations->Advance(submitted.Value()).Value().state == PCGAsyncState::Failed);
        CHECK(attempts == 1);
        CHECK(publications == 0);
    }

    TEST_CASE("PCG async non-standard publication exception remains a typed terminal failure", "[unit][pcg][async]") {
        auto jobs = Jobs();
        auto operations = Operations(jobs);
        const auto fence = Fence();
        REQUIRE(operations->RegisterFence(fence).HasValue());
        std::atomic<int> attempts{0};
        std::atomic<int> publications{0};
        auto request = Request(fence, publications);
        request.publish = [&attempts](const CancellationToken &) -> Result<void> {
            ++attempts;
            throw 7;
        };
        auto submitted = operations->Submit(PCGAsyncKind::Cook, std::move(request));
        REQUIRE(submitted.HasValue());
        const auto terminal = AwaitTerminal(*operations, submitted.Value());
        REQUIRE(terminal.terminal.has_value());
        CHECK(terminal.state == PCGAsyncState::Failed);
        REQUIRE(terminal.terminal->error.has_value());
        CHECK(terminal.terminal->error->code.Value() == PCGErrors::AsyncPublicationFailed.code.Value());
        CHECK(operations->Advance(submitted.Value()).Value().state == PCGAsyncState::Failed);
        CHECK(attempts == 1);
        CHECK(publications == 0);
    }

    TEST_CASE("PCG async completion sweep and closed scope retirement restore bounded admission", "[unit][pcg][async]") {
        auto jobs = Jobs();
        PCGAsyncLimits limits;
        limits.maximumScopes = 1;
        auto created = PCGAsyncOperations::Create(jobs, limits);
        REQUIRE(created.HasValue());
        auto operations = std::move(created).Value();
        limits.maximumScopes = 2;  // The coordinator retains its validated admission snapshot.
        const auto first = Fence();
        const auto second = Fence(1, 1, 1, 1, 2);
        REQUIRE(operations->RegisterFence(first).HasValue());
        RequireCode(operations->RegisterFence(second), PCGErrors::AsyncCapacityExceeded);
        std::atomic<int> publications{0};
        auto submitted = operations->Submit(PCGAsyncKind::Load, Request(first, publications));
        REQUIRE(submitted.HasValue());
        RequireCode(operations->RetireScope(first.scene, first.cell, first.graph.graph), PCGErrors::AsyncNotReady);
        REQUIRE(operations->InvalidateGraph(first.scene, first.cell, first.graph.graph).HasValue());
        RequireCode(operations->RetireScope(first.scene, first.cell, first.graph.graph), PCGErrors::AsyncNotReady);
        AwaitAll(*operations);
        CHECK(operations->IsGraphDrained(first.scene, first.cell, first.graph.graph));
        CHECK(operations->Snapshot(submitted.Value()).Value().state != PCGAsyncState::Succeeded);
        REQUIRE(operations->Forget(submitted.Value()).HasValue());
        REQUIRE(operations->RetireScope(first.scene, first.cell, first.graph.graph).HasValue());
        REQUIRE(operations->RegisterFence(second).HasValue());
        CHECK(publications == 0);
    }
}  // namespace Horo::PCG
