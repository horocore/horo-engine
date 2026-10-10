#include "../../../support/AllocationProbe.h"
#include "SaveSlotLifecycleTestSupport.h"

using namespace Horo::Runtime::SaveSlotLifecycleTest;

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
