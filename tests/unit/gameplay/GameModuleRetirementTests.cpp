#include "AllocationProbe.h"
#include "GameModuleHostTestHelpers.h"
#include "GameplayRetirementTestSupport.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <cstring>
using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Gameplay::HostTest;

using NativeFailureControl = Horo::Tests::NativeGameplayFailureControl;

TEST_CASE("Failed native registration retains an explicit candidate until owner-lane rollback", "[gameplay][retirement]") {
    NativeFailureControl native{1};
    GameModuleHost loader;
    auto result = loader.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(result.HasValue());
    auto candidate = std::move(result).Value();
    REQUIRE(candidate.diagnostic);
    REQUIRE(candidate.owner);
    CHECK(native.control.destroyed == 0);
    CHECK(native.control.stopped == 0);
    REQUIRE(candidate.owner->TryRetire().HasValue());
    CHECK(native.control.destroyed == 1);
    CHECK(native.control.stopped == 0);
}

TEST_CASE("Failed native startup callback drains after loader destruction on its original owner lane", "[gameplay][retirement]") {
    NativeFailureControl native{2};
    GameModuleRetirementQueue retirement{1};
    auto admitted = retirement.Reserve();
    REQUIRE(admitted.HasValue());
    auto admission = std::move(admitted).Value();
    const auto ownerThread = std::this_thread::get_id();
    {
        GameModuleHost loader;
        auto result = loader.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
        REQUIRE(result.HasValue());
        auto candidate = std::move(result).Value();
        REQUIRE(candidate.diagnostic);
        REQUIRE(candidate.owner);
        REQUIRE(native.callback);
        admission.Retain(std::move(candidate.owner));
    }
    CHECK(retirement.Close().HasError());
    CHECK(retirement.Reserve().HasError());
    CHECK(native.control.destroyed == 0);
    CHECK(native.control.stopped == 0);
    CHECK(native.callback->Invoke({}) == GameplayEventOutcome::CapabilityUnavailable);
    std::async(std::launch::async, [callback = std::move(native.callback)]() mutable {
        callback.reset();
    }).get();
    CHECK(native.control.destroyed == 0);
    CHECK(native.control.stopped == 0);
    REQUIRE(retirement.Close().HasValue());
    CHECK(native.control.destroyed == 1);
    CHECK(native.control.stopped == 1);
    CHECK(native.control.destroyedOn == ownerThread);
    CHECK(retirement.Reserve().HasValue());
}

TEST_CASE("Ordinary native retirement never requires reload snapshot support", "[gameplay][retirement]") {
    for (const unsigned phase : {3U, 4U}) {
        NativeFailureControl native{phase};
        GameModuleRetirementQueue retirement{1};
        auto admitted = retirement.Reserve();
        REQUIRE(admitted.HasValue());
        GameModuleHost loader;
        auto loaded = loader.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
        REQUIRE(loaded.HasValue());
        auto candidate = std::move(loaded).Value();
        REQUIRE_FALSE(candidate.diagnostic);
        auto admission = std::move(admitted).Value();
        admission.Retain(std::move(candidate.owner));
        REQUIRE(retirement.Close().HasValue());
        CHECK(native.control.prepared == 0);
        CHECK(native.control.stopped == 1);
        CHECK(native.control.destroyed == 1);
    }
}

TEST_CASE("Gameplay retirement rejects foreign-thread drain before reading reserved state", "[gameplay][retirement]") {
    GameModuleRetirementQueue retirement{1};
    auto admitted = retirement.Reserve();
    REQUIRE(admitted.HasValue());
    auto failure = std::async(std::launch::async, [&retirement] {
        return retirement.Drain();
    }).get();
    REQUIRE(failure.HasError());
    CHECK(failure.ErrorValue().code.Value() == GameplayErrors::GameplayReloadRestartRequired.code.Value());
    CHECK(retirement.Close().HasError());
}

TEST_CASE("Native build validation shares descriptor admission without creating runtime work", "[gameplay][retirement]") {
    NativeFailureControl native{2};
    GameModuleHost host;
    REQUIRE(host.ValidateArtifact(HORO_TEST_GAME_MODULE_PATH, Expectation()).HasValue());
    auto fingerprint = Expectation();
    fingerprint.buildFingerprint = "wrong-sdk";
    auto wrongFingerprint = host.ValidateArtifact(HORO_TEST_GAME_MODULE_PATH, fingerprint);
    REQUIRE(wrongFingerprint.HasError());
    CHECK(wrongFingerprint.ErrorValue().code.Value() == GameplayErrors::IncompatibleGameModule.code.Value());
    auto revision = Expectation();
    ++revision.descriptorRevision;
    REQUIRE(host.ValidateArtifact(HORO_TEST_GAME_MODULE_PATH, revision).HasError());
    CHECK(native.control.created == 0);
    CHECK(native.control.registered == 0);
    CHECK(native.control.started == 0);
    CHECK(native.control.stopped == 0);
    CHECK(native.control.destroyed == 0);
    CHECK_FALSE(native.callback);
}

TEST_CASE("Native Stop cannot reenter the application retirement authority", "[gameplay][retirement]") {
    NativeFailureControl native{3};
    GameModuleRetirementQueue retirement{1};

    struct Reentry {
        GameModuleRetirementQueue *queue;
        bool rejected{};
    } reentry{&retirement};

    native.control.stopOwner = &reentry;
    native.control.onStop = [](void *context) {
        auto &state = *static_cast<Reentry *>(context);
        state.rejected = state.queue->Drain().HasError() && state.queue->Reserve().HasError();
    };
    auto reserved = retirement.Reserve();
    REQUIRE(reserved.HasValue());
    GameModuleHost loader;
    auto loaded = loader.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    REQUIRE(loaded.HasValue());
    auto candidate = std::move(loaded).Value();
    REQUIRE_FALSE(candidate.diagnostic);
    auto admission = std::move(reserved).Value();
    admission.Retain(std::move(candidate.owner));
    REQUIRE(retirement.Close().HasValue());
    CHECK(reentry.rejected);
    CHECK(native.control.destroyed == 1);
}

TEST_CASE("Failed startup diagnostic allocation preserves its live native callback owner", "[gameplay][retirement][allocation]") {
    NativeFailureControl native{5};
    GameModuleRetirementQueue retirement{1};
    auto reserved = retirement.Reserve();
    REQUIRE(reserved.HasValue());
    auto admission = std::move(reserved).Value();
    static thread_local bool allocationFailed{};
    allocationFailed = false;
    std::optional<Horo::Tests::AllocationProbe::ScopedFailure> failure;
    native.control.allocationOwner = &failure;
    native.control.armAllocationFailure = [](void *context) noexcept {
        static_cast<decltype(failure) *>(context)->emplace(0, [](std::size_t) noexcept {
            allocationFailed = true;
        });
    };
    GameModuleHost loader;
    auto result = loader.Load(HORO_TEST_GAME_MODULE_PATH, Expectation());
    failure.reset();
    REQUIRE(allocationFailed);
    REQUIRE(result.HasValue());
    auto candidate = std::move(result).Value();
    REQUIRE(candidate.diagnostic);
    REQUIRE(candidate.owner);
    REQUIRE(native.callback);
    CHECK(candidate.diagnostic->code.Value() == GameplayErrors::GameplayFactoryFailed.code.Value());
    admission.Retain(std::move(candidate.owner));
    CHECK(retirement.Close().HasError());
    CHECK(native.control.stopped == 0);
    CHECK(native.control.destroyed == 0);
    const auto ownerThread = std::this_thread::get_id();
    std::async(std::launch::async, [callback = std::move(native.callback)]() mutable {
        callback.reset();
    }).get();
    CHECK(native.control.destroyed == 0);
    REQUIRE(retirement.Close().HasValue());
    CHECK(native.control.stopped == 1);
    CHECK(native.control.destroyed == 1);
    CHECK(native.control.destroyedOn == ownerThread);
}
