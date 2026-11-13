#include "PlatformProviderLifecycleTestSupport.h"

namespace {
    /** @brief Launches one synthetic native request and drains its successful copied completion. */
    void CompleteNativeRequest(PlatformProviderLifecycleHost &host, FixtureAudit *audit,
                               const PlatformProviderLifecycleHost::RequestHandle &request, const std::uint64_t revision) {
        CHECK(host.DispatchCompletions(0) == 0);
        REQUIRE(horo_test_provider_emit_revision(audit, request.Id().value, request.Generation().value, revision) ==
                HORO_EXTENSION_SUCCESS);
        CHECK(host.DispatchCompletions(64) == 1);
    }
}  // namespace

TEST_CASE("Session invalidation terminates old work before callbacks and cancels native work once",
          "[platform-services][lifecycle][session]") {
    Rig rig;
    rig.Publish();
    SECTION("Provider accepts cancellation") {}
    SECTION("Provider rejects cancellation") {
        horo_test_provider_cancel_status(rig.audit.get(), HORO_EXTENSION_ERROR_INIT_FAILED);
    }
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    unsigned observed{};
    auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 3) == HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 0);
    CheckTerminalFailure(*host, request.Value(), PlatformRequestState::Failed, PlatformSessionErrors::StaleSession);
    CHECK(observed == 0);
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 1);
    CHECK(horo_test_provider_destroyed(rig.audit.get()) == 0);
    REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 3, 2) == HORO_EXTENSION_SUCCESS);
    auto current = host->UnlockAchievement({2});
    REQUIRE(current.HasValue());
    CHECK(host->DispatchCompletions(64) == 0);
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 1);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(observed == 0);
    CheckTerminalFailure(*host, request.Value(), PlatformRequestState::Failed, PlatformSessionErrors::StaleSession);
    CHECK(host->Query(current.Value()).Value().state == PlatformRequestState::Running);
    REQUIRE(horo_test_provider_emit_revision(rig.audit.get(), current.Value().Id().value, current.Value().Generation().value, 3) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(host->Query(current.Value()).Value().state == PlatformRequestState::Succeeded);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Native session observers cannot deliver retained old success after replacement",
          "[platform-services][lifecycle][session][observer]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CompleteNativeRequest(*host, rig.audit.get(), request.Value(), 1);
    REQUIRE(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
    unsigned observed{};
    const auto switchAccount = [&] {
        REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 3) == HORO_EXTENSION_SUCCESS);
        REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 3, 2) == HORO_EXTENSION_SUCCESS);
    };
    SECTION("Late subscription rejects its original admission revision") {
        switchAccount();
        auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
            ++observed;
        });
        REQUIRE(subscription.HasError());
        CHECK(subscription.ErrorValue().code.Value() == PlatformSessionErrors::StaleSession.code.Value());
        CHECK(host->DispatchCompletions(64) == 0);
    }
    SECTION("An already queued retained-success callback loses delivery authority") {
        auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
            ++observed;
        });
        REQUIRE(subscription.HasValue());
        switchAccount();
        CHECK(host->DispatchCompletions(64) == 0);
    }
    CHECK(observed == 0);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
    auto current = host->UnlockAchievement({2});
    REQUIRE(current.HasValue());
    auto currentSubscription = host->OnComplete(current.Value(), [&](const auto &snapshot) {
        CHECK(snapshot.state == PlatformRequestState::Succeeded);
        ++observed;
    });
    REQUIRE(currentSubscription.HasValue());
    CompleteNativeRequest(*host, rig.audit.get(), current.Value(), 3);
    CHECK(observed == 1);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Native session observers revalidate each delivery after a callback changes account",
          "[platform-services][lifecycle][session][observer]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto first = host->UnlockAchievement({1});
    auto second = host->UnlockAchievement({2});
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    unsigned firstObserved{};
    unsigned secondObserved{};
    auto firstSubscription = host->OnComplete(first.Value(), [&](const auto &snapshot) {
        CHECK(snapshot.state == PlatformRequestState::Succeeded);
        CHECK(host->Session().revision == 1);
        CHECK(horo_test_provider_session_changed(rig.audit.get(), 2, 3) == HORO_EXTENSION_SUCCESS);
        CHECK(horo_test_provider_session_changed(rig.audit.get(), 3, 2) == HORO_EXTENSION_SUCCESS);
        CHECK(host->Session().revision == 3);
        ++firstObserved;
    });
    auto secondSubscription = host->OnComplete(second.Value(), [&](const auto &) {
        ++secondObserved;
    });
    REQUIRE(firstSubscription.HasValue());
    REQUIRE(secondSubscription.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), first.Value().Id().value, first.Value().Generation().value) == HORO_EXTENSION_SUCCESS);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), second.Value().Id().value, second.Value().Generation().value) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 2);
    CHECK(firstObserved == 1);
    CHECK(secondObserved == 0);
    CHECK(host->Query(first.Value()).Value().state == PlatformRequestState::Succeeded);
    CHECK(host->Query(second.Value()).Value().state == PlatformRequestState::Succeeded);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Native session observers preserve moved admission evidence and reject foreign store handles",
          "[platform-services][lifecycle][session][observer]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    auto moved = std::move(request).Value();
    PlatformProviderLifecycleHost::RequestHandle current;
    current = std::move(moved);
    CHECK_FALSE(moved.IsValid());
    PlatformRequestStore foreign({.generation = current.Generation()});
    auto foreignRequest = foreign.Admit<void>();
    REQUIRE(foreignRequest.HasValue());
    REQUIRE(foreignRequest.Value().Id() == current.Id());
    REQUIRE(foreignRequest.Value().Generation() == current.Generation());
    auto rejected = host->OnComplete(foreignRequest.Value(), [](const auto &) {
    });
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == RequestErrors::Stale.code.Value());
    auto empty = host->OnComplete(current, {});
    REQUIRE(empty.HasError());
    CHECK(empty.ErrorValue().code.Value() == FrontendErrors::InvalidRequest.code.Value());
    unsigned observed{};
    auto subscription = host->OnComplete(current, [&](const auto &) {
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    CompleteNativeRequest(*host, rig.audit.get(), current, 1);
    CHECK(observed == 1);
    REQUIRE(host->Close().HasValue());
}
