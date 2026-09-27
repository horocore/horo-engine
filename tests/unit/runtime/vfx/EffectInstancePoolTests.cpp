#include "Horo/Vfx/CpuParticleBuffer.h"
#include "Horo/Vfx/EffectInstancePool.h"
#include "Horo/Vfx/VfxErrors.h"
#include "support/AllocationProbe.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Vfx {
    namespace {
        [[nodiscard]] VfxIdentityScope Scope(const std::uint64_t value) {
            return VfxIdentityScope::Create(value).Value();
        }

        [[nodiscard]] EffectPoolDescriptor Descriptor(const std::uint32_t maximumInstances = 8) {
            return {.scene = Scope(10), .maximumInstances = maximumInstances, .emittersPerInstance = 3, .bytesPerInstance = 128};
        }

        [[nodiscard]] EffectPoolBudget Budget(const std::uint32_t maximumInstances = 8) {
            return {.maximumInstances = maximumInstances, .maximumEmitterSlots = maximumInstances * 3U, .maximumBytes = 1'000'000};
        }

        [[nodiscard]] EffectPlaybackRequest Request(const std::uint64_t tick = 0, const std::uint64_t ownerGeneration = 1,
                                                    const VfxRequirementClass requirement = VfxRequirementClass::Cosmetic) {
            return {.owner = Scope(20), .ownerGeneration = ownerGeneration, .tick = tick, .requirement = requirement};
        }

        [[nodiscard]] EffectInstancePool Pool(const EffectPoolDescriptor &descriptor, const EffectPoolBudget &budget,
                                              const EffectPoolPolicy &policy = {}) {
            auto prepared = EffectInstancePool::Prepare(descriptor, budget, policy);
            REQUIRE(prepared.HasValue());
            return std::move(prepared).Value();
        }

        template <typename Value> [[nodiscard]] bool HasErrorCode(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            return result.HasError() && result.ErrorValue().code.Value() == expected.code.Value();
        }
    }  // namespace

    TEST_CASE("Effect pool capacity is derived from every descriptor and reserved budget dimension", "[unit][vfx][effect-pool]") {
        const auto exactThreeBytes = Pool(Descriptor(3), Budget(3)).Statistics().plan.reservedBytes;
        auto budget = Budget(7);
        budget.maximumEmitterSlots = 12;
        budget.maximumBytes = exactThreeBytes;
        budget.requiredReserve = 1;
        auto pool = Pool(Descriptor(9), budget);
        const auto plan = pool.Statistics().plan;
        CHECK(plan.capacity == 3);
        CHECK(plan.reservedEmitterSlots == 9);
        CHECK(plan.reservedBytes == exactThreeBytes);
        CHECK(plan.requiredReserve == 1);
        CHECK(plan.delayedCapacity == 0);
        auto unusedDelayBudget = budget;
        unusedDelayBudget.maximumDelayed = 5;
        CHECK(Pool(Descriptor(9), unusedDelayBudget).Statistics().plan.delayedCapacity == 0);
    }

    TEST_CASE("Effect pool rejects capacity inputs that cannot satisfy the budget", "[unit][vfx][effect-pool]") {
        const auto exactThreeBytes = Pool(Descriptor(3), Budget(3)).Statistics().plan.reservedBytes;
        auto budget = Budget(7);
        budget.maximumEmitterSlots = 12;
        budget.maximumBytes = exactThreeBytes - 1U;
        budget.requiredReserve = 0;
        CHECK(Pool(Descriptor(9), budget).Statistics().plan.capacity == 2);
        budget.maximumBytes = 1;
        CHECK(HasErrorCode(EffectInstancePool::Prepare(Descriptor(9), budget, {}), VfxErrors::EffectPoolInvalid));
        budget = Budget(2);
        budget.requiredReserve = 3;
        CHECK(HasErrorCode(EffectInstancePool::Prepare(Descriptor(9), budget, {}), VfxErrors::EffectPoolInvalid));
        CHECK(HasErrorCode(EffectInstancePool::Prepare(Descriptor(0), Budget(), {}), VfxErrors::EffectPoolInvalid));
        CHECK(HasErrorCode(EffectInstancePool::Prepare(Descriptor(), Budget(), {.overBudget = EffectPoolOverBudgetPolicy::DelayBounded}),
                           VfxErrors::EffectPoolInvalid));
    }

    TEST_CASE("Allocation failure probe unwinds a standard vector allocation", "[unit][vfx][effect-pool]") {
        std::vector<std::uint32_t> values;
        bool allocationFailed = false;
        std::fputs("effect-pool standard vector failure probe begin\n", stderr);
        std::fflush(stderr);
        {
            Tests::AllocationProbe::ScopedFailure failNextAllocation{0};
            try {
                values.resize(8);
            } catch (const std::bad_alloc &) {
                allocationFailed = true;
            }
        }
        std::fputs("effect-pool standard vector failure probe end\n", stderr);
        std::fflush(stderr);
        CHECK(allocationFailed);
        CHECK(values.empty());
    }

    TEST_CASE("Allocation failure probe unwinds vector growth after a prior allocation", "[unit][vfx][effect-pool]") {
        std::vector<std::uint32_t> values;
        void *firstAllocation = nullptr;
        bool allocationFailed = false;
        std::fputs("effect-pool second-allocation vector failure probe begin\n", stderr);
        std::fflush(stderr);
        {
            Tests::AllocationProbe::ScopedFailure failSecondAllocation{1};
            try {
                firstAllocation = ::operator new(sizeof(std::uint32_t));
                values.resize(8);
            } catch (const std::bad_alloc &) {
                allocationFailed = true;
            }
        }
        std::fputs("effect-pool second-allocation vector failure probe end\n", stderr);
        std::fflush(stderr);
        const bool madeFirstAllocation = firstAllocation != nullptr;
        ::operator delete(firstAllocation);
        REQUIRE(madeFirstAllocation);
        CHECK(allocationFailed);
        CHECK(values.empty());
    }

    TEST_CASE("Effect pool reports allocation failure at every required preparation allocation", "[unit][vfx][effect-pool]") {
        const auto descriptor = Descriptor();
        const auto budget = Budget();
        const auto before = Tests::AllocationProbe::Count();
        const auto prepared = EffectInstancePool::Prepare(descriptor, budget, {});
        const auto preparationAllocations = Tests::AllocationProbe::Count() - before;
        REQUIRE(prepared.HasValue());
        REQUIRE(preparationAllocations >= 3);
        REQUIRE(preparationAllocations <= 16);
        for (std::size_t successfulAllocations = 0; successfulAllocations < preparationAllocations; ++successfulAllocations) {
            std::fprintf(stderr, "effect-pool preparation failure probe begin: %zu/%zu\n", successfulAllocations, preparationAllocations);
            std::fflush(stderr);
            bool allocationFailed = false;
            {
                Tests::AllocationProbe::ScopedFailure failOnePreparationAllocation{successfulAllocations};
                allocationFailed = HasErrorCode(EffectInstancePool::Prepare(descriptor, budget, {}), VfxErrors::EffectPoolAllocationFailed);
            }
            std::fprintf(stderr, "effect-pool preparation failure probe end: %zu/%zu\n", successfulAllocations, preparationAllocations);
            std::fflush(stderr);
            CHECK(allocationFailed);
        }
    }

    TEST_CASE("Effect pool reports allocation failure for bounded delay storage", "[unit][vfx][effect-pool]") {
        const auto descriptor = Descriptor();
        auto delayedBudget = Budget();
        delayedBudget.maximumDelayed = 1;
        const EffectPoolPolicy delayedPolicy{.overBudget = EffectPoolOverBudgetPolicy::DelayBounded, .maximumDelayTicks = 2};
        const auto before = Tests::AllocationProbe::Count();
        const auto prepared = EffectInstancePool::Prepare(descriptor, delayedBudget, delayedPolicy);
        const auto preparationAllocations = Tests::AllocationProbe::Count() - before;
        REQUIRE(prepared.HasValue());
        REQUIRE(preparationAllocations >= 4);
        REQUIRE(preparationAllocations <= 16);
        for (std::size_t successfulAllocations = 0; successfulAllocations < preparationAllocations; ++successfulAllocations) {
            std::fprintf(stderr, "effect-pool delayed failure probe begin: %zu/%zu\n", successfulAllocations, preparationAllocations);
            std::fflush(stderr);
            bool allocationFailed = false;
            {
                Tests::AllocationProbe::ScopedFailure failOnePreparationAllocation{successfulAllocations};
                allocationFailed = HasErrorCode(EffectInstancePool::Prepare(descriptor, delayedBudget, delayedPolicy),
                                                VfxErrors::EffectPoolAllocationFailed);
            }
            std::fprintf(stderr, "effect-pool delayed failure probe end: %zu/%zu\n", successfulAllocations, preparationAllocations);
            std::fflush(stderr);
            CHECK(allocationFailed);
        }
    }

    TEST_CASE("Effect pool preserves required reserve and retirement charges without implicit eviction", "[unit][vfx][effect-pool]") {
        auto budget = Budget(2);
        budget.requiredReserve = 1;
        auto pool = Pool(Descriptor(2), budget);
        const auto cosmetic = pool.Play(Request()).instance;
        REQUIRE(cosmetic.IsValid());
        CHECK(pool.Play(Request()).status == EffectPoolStatus::Rejected);
        CHECK(pool.Statistics().rejected == 1);
        const auto required = pool.Play(Request(0, 2, VfxRequirementClass::GameplayRequired)).instance;
        REQUIRE(required.IsValid());
        CHECK(pool.Play(Request(0, 3, VfxRequirementClass::GameplayRequired)).status == EffectPoolStatus::Rejected);

        REQUIRE(pool.Retain(cosmetic) == EffectPoolStatus::Admitted);
        REQUIRE(pool.Stop(cosmetic) == EffectPoolStatus::Admitted);
        CHECK(pool.CompleteRetirement(cosmetic) == EffectPoolStatus::RetentionPending);
        CHECK(pool.Play(Request()).status == EffectPoolStatus::Rejected);
        CHECK(pool.Statistics().retiring == 1);
        REQUIRE(pool.Acknowledge(cosmetic) == EffectPoolStatus::Admitted);
        REQUIRE(pool.CompleteRetirement(cosmetic) == EffectPoolStatus::Admitted);
        EffectInstanceSnapshot retiredSnapshot{};
        CHECK(pool.Inspect(cosmetic, retiredSnapshot) == EffectPoolStatus::StaleHandle);
        const auto reused = pool.Play(Request());
        REQUIRE(reused.status == EffectPoolStatus::Admitted);
        CHECK(reused.instance.slot == cosmetic.slot);
        CHECK(reused.instance.generation > cosmetic.generation);
        CHECK(pool.Statistics().peakActive == 2);
    }

    TEST_CASE("Effect pool bounded delay is FIFO, expires at the declared tick and cannot be bypassed", "[unit][vfx][effect-pool]") {
        auto budget = Budget(1);
        budget.maximumDelayed = 2;
        auto pool = Pool(Descriptor(1), budget, {.overBudget = EffectPoolOverBudgetPolicy::DelayBounded, .maximumDelayTicks = 3});
        const auto first = pool.Play(Request(1));
        REQUIRE(first.status == EffectPoolStatus::Admitted);
        const auto second = pool.Play(Request(1, 2));
        const auto third = pool.Play(Request(1, 3));
        REQUIRE(second.status == EffectPoolStatus::Delayed);
        REQUIRE(third.status == EffectPoolStatus::Delayed);
        CHECK(second.delayedTicket < third.delayedTicket);
        CHECK(pool.Play(Request(2, 4)).status == EffectPoolStatus::DelayQueueFull);
        CHECK(pool.PumpDelayed(2).status == EffectPoolStatus::Waiting);
        REQUIRE(pool.Stop(first.instance) == EffectPoolStatus::Admitted);
        REQUIRE(pool.CompleteRetirement(first.instance) == EffectPoolStatus::Admitted);
        CHECK(pool.Play(Request(2, 5)).status == EffectPoolStatus::DelayQueueFull);
        const auto drained = pool.PumpDelayed(2);
        REQUIRE(drained.status == EffectPoolStatus::Admitted);
        CHECK(drained.delayedTicket == second.delayedTicket);
        EffectInstanceSnapshot snapshot{};
        REQUIRE(pool.Inspect(drained.instance, snapshot) == EffectPoolStatus::Admitted);
        CHECK(snapshot.admittedTick == 2);
        CHECK(snapshot.ownerGeneration == 2);
        const auto expired = pool.PumpDelayed(4);
        CHECK(expired.status == EffectPoolStatus::DelayExpired);
        CHECK(expired.delayedTicket == third.delayedTicket);
        CHECK(pool.Statistics().expired == 1);
        CHECK(pool.Statistics().delayed == 0);
    }

    TEST_CASE("Effect pool selects the lowest reusable slot independent of retirement order", "[unit][vfx][effect-pool]") {
        auto first = Pool(Descriptor(3), Budget(3));
        auto second = Pool(Descriptor(3), Budget(3));
        const auto firstZero = first.Play(Request()).instance;
        const auto firstOne = first.Play(Request()).instance;
        const auto firstTwo = first.Play(Request()).instance;
        const auto secondZero = second.Play(Request()).instance;
        const auto secondOne = second.Play(Request()).instance;
        const auto secondTwo = second.Play(Request()).instance;
        REQUIRE(first.Stop(firstZero) == EffectPoolStatus::Admitted);
        REQUIRE(first.Stop(firstTwo) == EffectPoolStatus::Admitted);
        REQUIRE(second.Stop(secondZero) == EffectPoolStatus::Admitted);
        REQUIRE(second.Stop(secondTwo) == EffectPoolStatus::Admitted);
        REQUIRE(first.CompleteRetirement(firstTwo) == EffectPoolStatus::Admitted);
        REQUIRE(first.CompleteRetirement(firstZero) == EffectPoolStatus::Admitted);
        REQUIRE(second.CompleteRetirement(secondZero) == EffectPoolStatus::Admitted);
        REQUIRE(second.CompleteRetirement(secondTwo) == EffectPoolStatus::Admitted);
        CHECK(first.Play(Request()).instance == second.Play(Request()).instance);
        CHECK(first.Play(Request()).instance == second.Play(Request()).instance);
        CHECK(firstOne == secondOne);
    }

    TEST_CASE("Effect pool cancellation, restart and shutdown retain exact generations", "[unit][vfx][effect-pool]") {
        auto budget = Budget(1);
        budget.maximumDelayed = 1;
        auto pool = Pool(Descriptor(1), budget, {.overBudget = EffectPoolOverBudgetPolicy::DelayBounded, .maximumDelayTicks = 4});
        const auto first = pool.Play(Request(0)).instance;
        REQUIRE(first.IsValid());
        REQUIRE(pool.Retain(first) == EffectPoolStatus::Admitted);
        CHECK(pool.Restart(first, 1).status == EffectPoolStatus::RetentionPending);
        REQUIRE(pool.Acknowledge(first) == EffectPoolStatus::Admitted);
        const auto restarted = pool.Restart(first, 1);
        REQUIRE(restarted.status == EffectPoolStatus::Admitted);
        CHECK(pool.Stop(first) == EffectPoolStatus::StaleHandle);
        const auto queued = pool.Play(Request(1, 2));
        REQUIRE(queued.status == EffectPoolStatus::Delayed);
        const auto cancelled = pool.CancelOwner(Scope(20), 2);
        CHECK(cancelled.stopped == 0);
        CHECK(cancelled.cancelledDelayed == 1);
        CHECK(pool.PumpDelayed(1).status == EffectPoolStatus::Cancelled);
        CHECK(pool.Statistics().cancelled == 1);
        REQUIRE(pool.Retain(restarted.instance) == EffectPoolStatus::Admitted);
        REQUIRE(pool.Shutdown() == EffectPoolStatus::Admitted);
        CHECK(pool.Play(Request(2)).status == EffectPoolStatus::ShutDown);
        CHECK(pool.CompleteRetirement(restarted.instance) == EffectPoolStatus::RetentionPending);
        REQUIRE(pool.Acknowledge(restarted.instance) == EffectPoolStatus::Admitted);
        REQUIRE(pool.CompleteRetirement(restarted.instance) == EffectPoolStatus::Admitted);
        CHECK(pool.Quiescent());
        CHECK(pool.Shutdown() == EffectPoolStatus::Admitted);
    }

    TEST_CASE("Effect pool playback restart delay and retirement allocate nothing after preparation",
              "[unit][vfx][effect-pool][allocation]") {
        auto budget = Budget(1);
        budget.maximumDelayed = 1;
        auto pool = Pool(Descriptor(1), budget, {.overBudget = EffectPoolOverBudgetPolicy::DelayBounded, .maximumDelayTicks = 3});
        const auto before = Tests::AllocationProbe::Count();
        bool passed = true;
        for (std::uint64_t tick = 1; tick <= 64; ++tick) {
            const auto active = pool.Play(Request(tick));
            passed = passed && active.status == EffectPoolStatus::Admitted;
            const auto delayed = pool.Play(Request(tick, 2));
            passed = passed && delayed.status == EffectPoolStatus::Delayed;
            passed = passed && pool.Play(Request(tick, 3)).status == EffectPoolStatus::DelayQueueFull;
            passed = passed && pool.PumpDelayed(tick).status == EffectPoolStatus::Waiting;
            const auto restart = pool.Restart(active.instance, tick);
            passed = passed && restart.status == EffectPoolStatus::Admitted;
            const auto restarted = pool.Play(Request(tick, 4));
            passed = passed && restarted.status == EffectPoolStatus::DelayQueueFull;
            const auto current = pool.Statistics();
            passed = passed && current.active == 1 && current.delayed == 1;
            const auto cancelled = pool.CancelOwner(Scope(20), 2);
            passed = passed && cancelled.cancelledDelayed == 1;
            passed = passed && pool.PumpDelayed(tick).status == EffectPoolStatus::Cancelled;
            passed = passed && pool.Stop(restart.instance) == EffectPoolStatus::Admitted;
            passed = passed && pool.CompleteRetirement(restart.instance) == EffectPoolStatus::Admitted;
        }
        const auto after = Tests::AllocationProbe::Count();
        CHECK(passed);
        CHECK(after == before);
    }

    TEST_CASE("Effect pool default reject path never allocates", "[unit][vfx][effect-pool][allocation]") {
        auto pool = Pool(Descriptor(1), Budget(1));
        const auto before = Tests::AllocationProbe::Count();
        const auto admitted = pool.Play(Request());
        const auto rejected = pool.Play(Request());
        const auto stopped = pool.Stop(admitted.instance);
        const auto stillRejected = pool.Play(Request());
        const auto retired = pool.CompleteRetirement(admitted.instance);
        const auto reused = pool.Play(Request());
        const auto after = Tests::AllocationProbe::Count();
        CHECK(admitted.status == EffectPoolStatus::Admitted);
        CHECK(rejected.status == EffectPoolStatus::Rejected);
        CHECK(stopped == EffectPoolStatus::Admitted);
        CHECK(stillRejected.status == EffectPoolStatus::Rejected);
        CHECK(retired == EffectPoolStatus::Admitted);
        CHECK(reused.status == EffectPoolStatus::Admitted);
        CHECK(after == before);
    }

    TEST_CASE("Prepared CPU particle storage and effect slots replay without heap growth", "[unit][vfx][effect-pool][allocation]") {
        const auto scene = Scope(10);
        const auto bufferId = MakeVfxIdentity<ParticleBufferIdentityTag>(scene, 100, 1).Value();
        auto prepared = CpuParticleBuffer::Create({.buffer = bufferId, .capacity = 4});
        REQUIRE(prepared.HasValue());
        auto particles = std::move(prepared).Value();
        auto descriptor = Descriptor(1);
        descriptor.bytesPerInstance = particles.Statistics().allocatedBytes;
        auto pool = Pool(descriptor, Budget(1));
        const auto before = Tests::AllocationProbe::Count();
        bool passed = true;
        for (std::uint64_t tick = 1; tick <= 64; ++tick) {
            const auto activation = pool.Play(Request(tick));
            passed = passed && activation.status == EffectPoolStatus::Admitted;
            const auto first = particles.Spawn(ParticleSimulationId::Create((2U * tick) - 1U).Value());
            passed = passed && first.HasValue();
            passed = passed && particles.Clear().HasValue();
            const auto restarted = pool.Restart(activation.instance, tick);
            passed = passed && restarted.status == EffectPoolStatus::Admitted;
            const auto second = particles.Spawn(ParticleSimulationId::Create(2U * tick).Value());
            passed = passed && second.HasValue();
            passed = passed && particles.Clear().HasValue();
            passed = passed && pool.Stop(restarted.instance) == EffectPoolStatus::Admitted;
            passed = passed && pool.CompleteRetirement(restarted.instance) == EffectPoolStatus::Admitted;
        }
        const auto after = Tests::AllocationProbe::Count();
        CHECK(passed);
        CHECK(after == before);
        CHECK(pool.Statistics().active == 0);
        CHECK(particles.Statistics().active == 0);
    }

    TEST_CASE("Effect pool rejects foreign threads without mutating owner state", "[unit][vfx][effect-pool]") {
        auto pool = Pool(Descriptor(1), Budget(1));
        std::atomic<EffectPoolStatus> observed{EffectPoolStatus::Empty};
        std::thread worker([&] {
            observed.store(pool.Play(Request()).status);
        });
        worker.join();
        CHECK(observed.load() == EffectPoolStatus::ThreadViolation);
        CHECK(pool.Statistics().active == 0);
    }
}  // namespace Horo::Vfx
