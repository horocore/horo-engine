#include "PlatformProviderLifecycleTestSupport.h"

TEST_CASE("Configuration cannot reuse a provider admitted under another host profile", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
    const auto started = rig.Start({}, PlatformServicesHostProfile::InteractiveDevelopment);
    REQUIRE(started.HasError());
    CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::InvalidSelection.code.Value());
    CHECK(horo_test_provider_event_count(rig.audit.get()) == 0);
}

TEST_CASE("Exact selection never substitutes another provider", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
    rig.identity.providerId = "example.other";
    const auto started = rig.Start();
    REQUIRE(started.HasError());
    CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::InvalidSelection.code.Value());
    CHECK(horo_test_provider_event_count(rig.audit.get()) == 0);
}

TEST_CASE("Factory-only provider remains loadable but cannot start operation host", "[platform-services][lifecycle][abi]") {
    Rig rig;
    auto candidate = Candidate(rig.audit);
    candidate.operations = {};
    auto published = rig.admission.Commit(std::move(candidate));
    REQUIRE(published.HasValue());
    rig.publication = std::move(published).Value();
    const auto started = rig.Start();
    REQUIRE(started.HasError());
    CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::UnsupportedProfile.code.Value());
    CHECK(Events(*rig.audit) == "CRZ");
}

TEST_CASE("Every failed provider initialization stage rolls back without publication", "[platform-services][lifecycle]") {
    for (const unsigned stage : {1U, 2U, 3U}) {
        Rig rig;
        rig.Publish();
        horo_test_provider_fail_at(rig.audit.get(), stage);
        const auto started = rig.Start();
        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::InitializationFailed.code.Value());
        const auto events = Events(*rig.audit);
        if (stage == 1)
            CHECK(events == "CIAURZ");
        else if (stage == 2)
            CHECK(events == "CISADTURZ");
        else
            CHECK(events == "CISGAKDTURZ");
        CHECK(horo_test_provider_destroyed(rig.audit.get()) == 1);
    }
}

TEST_CASE("Native completion is copied and dispatched after provider callback returns", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    CHECK(host->Session().revision == 1);
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    unsigned observed{};
    auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    std::atomic_uint callbackStatus{HORO_EXTENSION_ERROR_INIT_FAILED};
    std::thread callback([&] {
        callbackStatus = horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value);
    });
    callback.join();
    CHECK(callbackStatus == HORO_EXTENSION_SUCCESS);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Running);
    CHECK(observed == 0);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(observed == 1);
    const auto completed = host->Query(request.Value());
    REQUIRE(completed.HasValue());
    CHECK(completed.Value().state == PlatformRequestState::Succeeded);
    REQUIRE(host->Close().HasValue());
    CHECK(Events(*rig.audit) == "CISGOAKDTURZ");
}

TEST_CASE("BUSY drain retains sink and code until held callback exits", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
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
    horo_test_provider_set_busy(rig.audit.get(), 1);
    std::atomic_uint callbackStatus{HORO_EXTENSION_SUCCESS};
    std::thread callback([&] {
        callbackStatus = horo_test_provider_emit_held(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value);
    });
    while (!horo_test_provider_held_ready(rig.audit.get()))
        std::this_thread::yield();
    const auto firstClose = host->Close();
    REQUIRE(firstClose.HasError());
    CHECK(firstClose.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::DrainBusy.code.Value());
    CHECK(horo_test_provider_destroyed(rig.audit.get()) == 0);
    CHECK(host->UnlockAchievement({1}).HasError());
    horo_test_provider_release_held(rig.audit.get());
    callback.join();
    CHECK(callbackStatus == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
    horo_test_provider_set_busy(rig.audit.get(), 0);
    REQUIRE(host->Close().HasValue());
    CHECK(horo_test_provider_destroyed(rig.audit.get()) == 1);
    CHECK(horo_test_provider_post_destroy_callbacks(rig.audit.get()) == 0);
    CHECK(observed == 0);
}

TEST_CASE("Host translates every provider result to a safe application meaning", "[platform-services][errors]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    const std::array<std::pair<std::uint32_t, PlatformProviderFailureCategory>, 10> cases{
        std::pair{HORO_PLATFORM_PROVIDER_OFFLINE, PlatformProviderFailureCategory::Offline},
        std::pair{HORO_PLATFORM_PROVIDER_NOT_SIGNED_IN, PlatformProviderFailureCategory::NotSignedIn},
        std::pair{HORO_PLATFORM_PROVIDER_FORBIDDEN, PlatformProviderFailureCategory::Forbidden},
        std::pair{HORO_PLATFORM_PROVIDER_RATE_LIMITED, PlatformProviderFailureCategory::RateLimited},
        std::pair{HORO_PLATFORM_PROVIDER_PRECONDITION_FAILED, PlatformProviderFailureCategory::PreconditionFailed},
        std::pair{HORO_PLATFORM_PROVIDER_QUOTA_EXCEEDED, PlatformProviderFailureCategory::QuotaExceeded},
        std::pair{HORO_PLATFORM_PROVIDER_INVALID_RESPONSE, PlatformProviderFailureCategory::InvalidResponse},
        std::pair{HORO_PLATFORM_PROVIDER_TRANSIENT_FAILURE, PlatformProviderFailureCategory::TransientFailure},
        std::pair{HORO_PLATFORM_PROVIDER_PERMANENT_FAILURE, PlatformProviderFailureCategory::PermanentFailure},
        std::pair{999U, PlatformProviderFailureCategory::Unknown},
    };
    for (const auto &[code, category] : cases) {
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(0) == 0);
        REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value, code) ==
                HORO_EXTENSION_SUCCESS);
        REQUIRE(host->DispatchCompletions(1) == 1);
        const auto snapshot = host->Query(request.Value());
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().state == PlatformRequestState::Failed);
        REQUIRE(snapshot.Value().terminal);
        REQUIRE(snapshot.Value().terminal->HasError());
        const Error &error = *snapshot.Value().terminal->ErrorValue();
        const auto meaning = category == PlatformProviderFailureCategory::NotSignedIn ? PlatformServiceErrorKind::AuthenticationRequired
                             : category == PlatformProviderFailureCategory::Forbidden ? PlatformServiceErrorKind::AccessDenied
                                                                                      : PlatformServiceErrorKind::ProviderFailed;
        CHECK(ClassifyPlatformServiceError(error) == meaning);
        CHECK(PlatformProviderCategory(error) == category);
        REQUIRE(error.cause.Get());
        REQUIRE(error.cause.Get()->diagnostics.size() == 1);
        const auto &message = error.cause.Get()->diagnostics.front().message;
        CHECK(message.find(std::to_string(request.Value().Id().value)) != std::string::npos);
        CHECK(message.find("private-account") == std::string::npos);
        CHECK(error.message.find("private-account") == std::string::npos);
    }
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Host preserves cancellation timeout and capability failure states", "[platform-services][errors]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    for (const auto &[code, kind, state] :
         std::array{std::tuple{HORO_PLATFORM_PROVIDER_CANCELLED, PlatformServiceErrorKind::Cancelled, PlatformRequestState::Cancelled},
                    std::tuple{HORO_PLATFORM_PROVIDER_TIMED_OUT, PlatformServiceErrorKind::TimedOut, PlatformRequestState::TimedOut},
                    std::tuple{HORO_PLATFORM_PROVIDER_CAPABILITY_UNAVAILABLE, PlatformServiceErrorKind::CapabilityUnavailable,
                               PlatformRequestState::Failed}}) {
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(0) == 0);
        REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value, code) ==
                HORO_EXTENSION_SUCCESS);
        REQUIRE(host->DispatchCompletions(1) == 1);
        const auto snapshot = host->Query(request.Value());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().state == state);
        REQUIRE(snapshot.Value().terminal);
        REQUIRE(snapshot.Value().terminal->HasError());
        CHECK(ClassifyPlatformServiceError(*snapshot.Value().terminal->ErrorValue()) == kind);
    }
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Synchronous provider submission failure keeps cancellation distinct from unknown failure", "[platform-services][errors]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    for (const auto [status, expectedState, expectedMeaning] :
         std::array{std::tuple{HORO_EXTENSION_ERROR_CANCELLED, PlatformRequestState::Cancelled, PlatformServiceErrorKind::Cancelled},
                    std::tuple{HORO_EXTENSION_ERROR_INIT_FAILED, PlatformRequestState::Failed, PlatformServiceErrorKind::ProviderFailed}}) {
        horo_test_provider_submit_status(rig.audit.get(), status);
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(0) == 0);
        const auto snapshot = host->Query(request.Value());
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().state == expectedState);
        REQUIRE(snapshot.Value().terminal);
        REQUIRE(snapshot.Value().terminal->HasError());
        const Error &error = *snapshot.Value().terminal->ErrorValue();
        CHECK(ClassifyPlatformServiceError(error) == expectedMeaning);
        if (expectedMeaning == PlatformServiceErrorKind::ProviderFailed)
            CHECK(PlatformProviderCategory(error) == PlatformProviderFailureCategory::Unknown);
    }
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Session revision change fences an in-flight native completion", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 2) == HORO_EXTENSION_SUCCESS);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    const auto result = host->Query(request.Value());
    REQUIRE(result.HasValue());
    CHECK(result.Value().state == PlatformRequestState::Failed);
    REQUIRE(result.Value().terminal.has_value());
    REQUIRE(result.Value().terminal->HasError());
    CHECK(result.Value().terminal->ErrorValue()->code.Value() == PlatformSessionErrors::StaleSession.code.Value());
    REQUIRE(host->Close().HasValue());
}

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
    auto subscription = host->OnComplete(request.Value(), [&](const auto &snapshot) {
        REQUIRE(snapshot.terminal);
        REQUIRE(snapshot.terminal->HasError());
        CHECK(snapshot.terminal->ErrorValue()->code.Value() == PlatformSessionErrors::StaleSession.code.Value());
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 3) == HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 0);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Failed);
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
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Failed);
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
    CHECK(host->DispatchCompletions(0) == 0);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
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
    CHECK(host->DispatchCompletions(0) == 0);
    REQUIRE(horo_test_provider_emit_revision(rig.audit.get(), current.Value().Id().value, current.Value().Generation().value, 3) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
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
    unsigned observed{};
    auto subscription = host->OnComplete(current, [&](const auto &) {
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), current.Id().value, current.Generation().value) == HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(observed == 1);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Queued caller cancellation is terminal once without native submission or callback reentry",
          "[platform-services][lifecycle][cancellation]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Queued);
    unsigned observed{};
    auto subscription = host->OnComplete(request.Value(), [&](const auto &snapshot) {
        CHECK(snapshot.state == PlatformRequestState::Cancelled);
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    REQUIRE(host->RequestCancel(request.Value()).HasValue());
    REQUIRE(host->RequestCancel(request.Value()).HasValue());
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Cancelled);
    CHECK(observed == 0);
    CHECK(host->DispatchCompletions(64) == 0);
    CHECK(observed == 1);
    CHECK(std::ranges::count(Events(*rig.audit), 'O') == 0);
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 0);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Queued work from an old session revision never reaches the provider", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 2) == HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 0);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Failed);
    CHECK(std::ranges::count(Events(*rig.audit), 'O') == 0);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Executing cancellation is sent once and completion wins or acknowledges deterministically",
          "[platform-services][lifecycle][cancellation]") {
    Rig rig;
    rig.Publish();
    auto started = rig.Start();
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    for (const auto result : {HORO_PLATFORM_PROVIDER_SUCCESS, HORO_PLATFORM_PROVIDER_CANCELLED}) {
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(0) == 0);
        REQUIRE(host->RequestCancel(request.Value()).HasValue());
        REQUIRE(host->RequestCancel(request.Value()).HasValue());
        CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Cancelling);
        CHECK(host->DispatchCompletions(0) == 0);
        REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value, result) ==
                HORO_EXTENSION_SUCCESS);
        CHECK(host->DispatchCompletions(64) == 1);
        CHECK(host->Query(request.Value()).Value().state ==
              (result == HORO_PLATFORM_PROVIDER_SUCCESS ? PlatformRequestState::Succeeded : PlatformRequestState::Cancelled));
        REQUIRE(host->RequestCancel(request.Value()).HasValue());
    }
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 2);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Timeout releases caller and observer despite unsupported native cancellation; late evidence only retires lease",
          "[platform-services][lifecycle][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{5};
    auto started = rig.Start(policy);
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    horo_test_provider_cancel_status(rig.audit.get(), HORO_EXTENSION_ERROR_INVALID_ARGS);
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    unsigned observed{};
    auto callbackOwner = std::make_shared<int>(1);
    std::weak_ptr<int> callbackLifetime = callbackOwner;
    auto subscription = host->OnComplete(request.Value(), [&, callbackOwner](const auto &snapshot) {
        CHECK(*callbackOwner == 1);
        CHECK(snapshot.state == PlatformRequestState::TimedOut);
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    callbackOwner.reset();
    CHECK(!callbackLifetime.expired());
    const auto deadline = host->Query(request.Value()).Value().timing.admittedAt + std::chrono::seconds{5};
    unsigned expectedObserved = 1;
    SECTION("Session remains current at the deadline") {}
    SECTION("Session invalidation cannot replace an expired admission deadline") {
        expectedObserved = 0;
        REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 3) == HORO_EXTENSION_SUCCESS);
    }
    CHECK(host->DispatchCompletions(64, deadline + std::chrono::milliseconds{1}) == 0);
    CHECK(observed == expectedObserved);
    CHECK(callbackLifetime.expired());
    auto timedOut = host->Query(request.Value());
    REQUIRE(timedOut.HasValue());
    REQUIRE(timedOut.Value().terminal);
    CHECK(timedOut.Value().terminal->ErrorValue()->code.Value() == RequestErrors::TimedOut.code.Value());
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 1);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value + 1) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::TimedOut);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::TimedOut);
    CHECK(observed == expectedObserved);
    REQUIRE(host->Close().HasValue());
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 1);
}

TEST_CASE("Ingress timestamp wins deadline sweep and wrong generation cannot finalize a request",
          "[platform-services][lifecycle][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{5};
    auto started = rig.Start(policy);
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    const auto deadline = host->Query(request.Value()).Value().timing.admittedAt + std::chrono::seconds{5};
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value + 1) ==
            HORO_EXTENSION_SUCCESS);
    REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value) ==
            HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(0, deadline + std::chrono::milliseconds{1}) == 0);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Running);
    CHECK(host->DispatchCompletions(64, deadline + std::chrono::milliseconds{1}) == 2);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Cancellation and timeout race publishes timeout once when acknowledgement arrives afterward",
          "[platform-services][lifecycle][cancellation][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{5};
    auto started = rig.Start(policy);
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    horo_test_provider_cancel_status(rig.audit.get(), HORO_EXTENSION_ERROR_INVALID_ARGS);
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    REQUIRE(host->RequestCancel(request.Value()).HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 1);
    const auto deadline = host->Query(request.Value()).Value().timing.admittedAt + std::chrono::seconds{5};
    CHECK(host->DispatchCompletions(64, deadline + std::chrono::milliseconds{1}) == 0);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::TimedOut);
    REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value,
                                            HORO_PLATFORM_PROVIDER_CANCELLED) == HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64) == 1);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::TimedOut);
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 1);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Provider failure observed before the deadline remains failure when dispatch runs late",
          "[platform-services][lifecycle][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{5};
    auto started = rig.Start(policy);
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    CHECK(host->DispatchCompletions(0) == 0);
    const auto deadline = host->Query(request.Value()).Value().timing.admittedAt + std::chrono::seconds{5};
    REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value,
                                            HORO_PLATFORM_PROVIDER_RATE_LIMITED) == HORO_EXTENSION_SUCCESS);
    CHECK(host->DispatchCompletions(64, deadline + std::chrono::milliseconds{1}) == 1);
    const auto snapshot = host->Query(request.Value());
    REQUIRE(snapshot.HasValue());
    CHECK(snapshot.Value().state == PlatformRequestState::Failed);
    REQUIRE(snapshot.Value().terminal);
    CHECK(PlatformProviderCategory(*snapshot.Value().terminal->ErrorValue()) == PlatformProviderFailureCategory::RateLimited);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Queued deadline takes precedence over a late caller cancellation", "[platform-services][lifecycle][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{5};
    auto started = rig.Start(policy);
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    const auto deadline = host->Query(request.Value()).Value().timing.admittedAt + std::chrono::seconds{5};
    REQUIRE(host->RequestCancel(request.Value(), deadline).HasValue());
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::TimedOut);
    CHECK(host->DispatchCompletions(64) == 0);
    CHECK(std::ranges::count(Events(*rig.audit), 'O') == 0);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Queued work expires without native submission or cancellation", "[platform-services][lifecycle][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{5};
    auto started = rig.Start(policy);
    REQUIRE(started.HasValue());
    auto host = std::move(started).Value();
    auto request = host->UnlockAchievement({1});
    REQUIRE(request.HasValue());
    unsigned observed{};
    auto subscription = host->OnComplete(request.Value(), [&](const auto &snapshot) {
        CHECK(snapshot.state == PlatformRequestState::TimedOut);
        ++observed;
    });
    REQUIRE(subscription.HasValue());
    const auto deadline = host->Query(request.Value()).Value().timing.admittedAt + std::chrono::seconds{5};
    CHECK(host->DispatchCompletions(64, deadline) == 0);
    CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::TimedOut);
    CHECK(observed == 1);
    CHECK(std::ranges::count(Events(*rig.audit), 'O') == 0);
    CHECK(std::ranges::count(Events(*rig.audit), 'X') == 0);
    REQUIRE(host->Close().HasValue());
}

TEST_CASE("Invalid per-service deadline policy fails before native provider creation", "[platform-services][lifecycle][timeout]") {
    Rig rig;
    rig.Publish();
    PlatformProviderRequestPolicy policy;
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::milliseconds::zero();
    auto started = rig.Start(policy);
    REQUIRE(started.HasError());
    CHECK(started.ErrorValue().code.Value() == RequestErrors::InvalidConfiguration.code.Value());
    CHECK(Events(*rig.audit).empty());
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = std::chrono::seconds{30};
    policy.timeouts[static_cast<std::size_t>(PlatformServiceKind::Friends)] = std::chrono::hours{25};
    const auto tooLong = rig.Start(policy);
    REQUIRE(tooLong.HasError());
    CHECK(Events(*rig.audit).empty());
}

TEST_CASE("Repeated start and stop releases every native and module lease", "[platform-services][lifecycle]") {
    Rig rig;
    rig.Publish();
    std::weak_ptr<FixtureAudit> code = rig.audit;
    for (unsigned cycle = 0; cycle < 2; ++cycle) {
        auto started = rig.Start();
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        REQUIRE(host->Close().HasValue());
        CHECK(horo_test_provider_destroyed(rig.audit.get()) == 1);
    }
    CHECK(Events(*rig.audit) == "CISGAKDTURZCISGAKDTURZ");
    rig.publication.reset();
    rig.audit.reset();
    CHECK(rig.admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
    CHECK(code.expired());
}
}  // namespace Horo::PlatformServices::Tests
