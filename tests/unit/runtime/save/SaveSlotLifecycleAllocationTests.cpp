#include "../../../support/AllocationProbe.h"
#include "SaveSlotLifecycleTestSupport.h"

#include <atomic>
#include <chrono>
#include <thread>

using namespace Horo::Runtime::SaveSlotLifecycleTest;

namespace {
    /** @brief Attempts one ordinary allocation without framework reporting inside the failure window. */
    bool TryAllocation() noexcept {
        try {
            void *storage = ::operator new(64);
            ::operator delete(storage);
            return true;
        } catch (const std::bad_alloc &) {
            return false;
        }
    }
}  // namespace

TEST_CASE("Scoped allocation failure remains on its owning thread and fires once", "[save][lifecycle][allocation]") {
    std::atomic<bool> start{};
    std::atomic<bool> done{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    bool workerAllocated{};
    std::jthread worker{[&] {
        while (!start.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        if (start.load())
            workerAllocated = TryAllocation();
        done.store(true);
    }};
    bool workerCompletedWithinDeadline{};
    bool ownerFailed{};
    bool secondOwnerAllocationSucceeded{};
    {
        const Horo::Tests::AllocationProbe::ScopedFailure allocation;
        start.store(true);
        // Avoid allocator-using synchronization inside the owner's armed failure window.
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        workerCompletedWithinDeadline = done.load();
        ownerFailed = !TryAllocation();
        secondOwnerAllocationSucceeded = TryAllocation();
    }
    worker.join();
    CHECK(workerCompletedWithinDeadline);
    CHECK(workerAllocated);
    CHECK(ownerFailed);
    CHECK(secondOwnerAllocationSucceeded);
}

TEST_CASE("Lifecycle allocation failure after visibility preserves publication knowledge", "[save][lifecycle]") {
    const bool syncFailure = GENERATE(false, true);
    Fixture fixture;
    fixture.Import(10);
    const auto previous = fixture.Import(11);
    const auto oldGeneration = previous.entry->publication.generation;
    const auto oldBytes = DiskBytes(fixture.Generation(oldGeneration));
    const auto request = fixture.Copy(10, 11);
    Error syncError = MakeError(SaveErrors::StoragePermanentIo);
    std::optional<Horo::Tests::AllocationProbe::ScopedFailure> allocation;
    fixture.fault.failure = [&](const SaveSlotLifecycleIoStage stage, const SaveSlotLifecycleFileKind kind) {
        if (!allocation && stage == SaveSlotLifecycleIoStage::DirectorySync && kind == SaveSlotLifecycleFileKind::Catalog) {
            allocation.emplace();
            if (syncFailure)
                return Result<void>::Failure(std::move(syncError));
        }
        return Result<void>::Success();
    };
    const auto result = fixture.owner->Execute(request);
    allocation.reset();
    REQUIRE(result.HasError() == syncFailure);
    if (syncFailure)
        CHECK(result.ErrorValue().code.Value() == SaveErrors::SlotCommitOutcomeUnknown.code.Value());
    else
        CHECK(result.Value().cleanupDeferred);
    CHECK(fixture.host.leases == 0);
    CHECK(DiskBytes(fixture.Generation(oldGeneration)) == oldBytes);
    fixture.fault.failure = {};
    fixture.Reopen();
    CHECK(fixture.Target(11).generation != oldGeneration);
    CHECK_FALSE(fixture.ExportBytes(11).empty());
    const auto reconciled = fixture.owner->Reconcile(fixture.Access());
    REQUIRE(reconciled.HasValue());
    CHECK_FALSE(reconciled.Value());
    CHECK_FALSE(std::filesystem::exists(fixture.Generation(oldGeneration)));
}

TEST_CASE("Lifecycle provider adapters translate allocation exceptions before returning", "[save][lifecycle]") {
    Fault observer;
    observer.failure = [](SaveSlotLifecycleIoStage, SaveSlotLifecycleFileKind) -> Result<void> {
        throw std::bad_alloc{};
    };
    const auto observed = observer.Before(SaveSlotLifecycleIoStage::DirectorySync, SaveSlotLifecycleFileKind::Catalog);
    REQUIRE(observed.HasError());
    CHECK(observed.ErrorValue().code.Value() == SaveErrors::StorageAllocationFailed.code.Value());
    Host host;
    host.onRecycle = []() -> Result<void> {
        throw std::bad_alloc{};
    };
    const auto recycled = host.Recycle({}, {});
    REQUIRE(recycled.HasError());
    CHECK(recycled.ErrorValue().code.Value() == SaveErrors::StorageAllocationFailed.code.Value());
}
