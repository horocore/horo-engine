#include "../../../support/AllocationProbe.h"
#include "SaveAllocationDiagnostics.h"
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

    /** @brief Reopens acknowledged publication and proves selected bytes survive before retired bytes are reconciled. */
    void CheckRecoveredPublication(Fixture &fixture, const SlotGenerationId oldGeneration) {
        fixture.fault.failure = {};
        SaveAllocationPhase("[save-allocation] lifecycle-reopen-enter\n");
        fixture.Reopen();
        SaveAllocationPhase("[save-allocation] lifecycle-reopen-returned\n");
        CHECK(fixture.Target(11).generation != oldGeneration);
        CHECK_FALSE(fixture.ExportBytes(11).empty());
        SaveAllocationPhase("[save-allocation] lifecycle-reconcile-enter\n");
        const auto reconciled = fixture.owner->Reconcile(fixture.Access());
        SaveAllocationPhase("[save-allocation] lifecycle-reconcile-returned\n");
        REQUIRE(reconciled.HasValue());
        CHECK_FALSE(reconciled.Value());
        CHECK_FALSE(std::filesystem::exists(fixture.Generation(oldGeneration)));
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
    if (syncFailure)
        SaveAllocationPhase("[save-allocation] lifecycle-sync-failure\n");
    else
        SaveAllocationPhase("[save-allocation] lifecycle-sync-success\n");
    const SaveAllocationFixtureExit fixtureExit;
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
            SaveAllocationPhase("[save-allocation] lifecycle-arm-enter\n");
            allocation.emplace(0, SaveAllocationInjected);
            SaveAllocationPhase("[save-allocation] lifecycle-armed\n");
            if (syncFailure)
                return Result<void>::Failure(std::move(syncError));
        }
        return Result<void>::Success();
    };
    SaveAllocationPhase("[save-allocation] lifecycle-execute-enter\n");
    const auto result = fixture.owner->Execute(request);
    SaveAllocationPhase("[save-allocation] lifecycle-execute-returned\n");
    allocation.reset();
    SaveAllocationPhase("[save-allocation] lifecycle-failure-reset\n");
    CheckSaveAllocationPublication(result, syncFailure);
    CHECK(fixture.host.leases == 0);
    CHECK(DiskBytes(fixture.Generation(oldGeneration)) == oldBytes);
    CheckRecoveredPublication(fixture, oldGeneration);
    SaveAllocationPhase("[save-allocation] lifecycle-fixture-cleanup-next\n");
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
