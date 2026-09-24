#include "Horo/PlatformServices/PlatformSessionObserver.h"
#include "PlatformServicesTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    using TestSupport::RequireError;

    namespace {
        PlatformSessionSnapshot Snapshot(const std::uint64_t generation, const std::uint64_t accessRevision = 1,
                                         const std::byte nonce = std::byte{1}) {
            PlatformSessionCandidate candidate{.phase = PlatformSessionPhase::Active,
                                               .generation = {generation},
                                               .providerGeneration = {7},
                                               .accessRevision = {accessRevision}};
            candidate.capabilities.services.fill(PlatformSessionAccessState::Granted);
            PlatformSubjectNonce subject;
            subject.bytes.back() = nonce;
            candidate.subjectNonce = subject;
            auto result = BuildPlatformSessionSnapshot(candidate);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        PlatformSessionNotification Notification(const std::uint64_t revision, const std::uint64_t generation,
                                                 const std::uint64_t accessRevision = 1, const std::byte nonce = std::byte{1}) {
            return {.revision = revision, .snapshot = Snapshot(generation, accessRevision, nonce)};
        }
    }  // namespace

    TEST_CASE("Session observer configuration and subscriptions remain bounded", "[platform-services][session][observer]") {
        RequireError(PlatformSessionObserver::Create({.maxObservers = 0}), SessionObserverErrors::InvalidConfiguration);
        RequireError(PlatformSessionObserver::Create({.maxObservers = MaximumPlatformSessionObservers + 1}),
                     SessionObserverErrors::InvalidConfiguration);

        auto created = PlatformSessionObserver::Create({.maxObservers = 1, .maxPendingNotifications = 1});
        REQUIRE(created.HasValue());
        auto observer = std::move(created).Value();
        auto first = observer.Subscribe([](const PlatformSessionNotification &) {
        });
        REQUIRE(first.HasValue());
        CHECK(observer.ObserverCount() == 1);
        RequireError(observer.Subscribe([](const PlatformSessionNotification &) {
        }),
                     SessionObserverErrors::CapacityExceeded);
        auto firstSubscription = std::move(first).Value();
        firstSubscription.Reset();
        CHECK(observer.ObserverCount() == 0);
    }

    TEST_CASE("Provider notifications are deferred and dispatched only on the owner engine thread",
              "[platform-services][session][observer][threading]") {
        auto created = PlatformSessionObserver::Create({.maxObservers = 4, .maxPendingNotifications = 4});
        REQUIRE(created.HasValue());
        auto observer = std::move(created).Value();
        const auto ownerThread = std::this_thread::get_id();
        std::vector<std::thread::id> callbackThreads;
        std::vector<std::uint64_t> revisions;
        auto subscription = observer.Subscribe([&](const PlatformSessionNotification &notification) {
            callbackThreads.push_back(std::this_thread::get_id());
            revisions.push_back(notification.revision);
        });
        REQUIRE(subscription.HasValue());

        std::atomic<bool> accepted{false};
        std::thread provider([&] {
            accepted.store(observer.Enqueue(Notification(3, 4, 3)).HasValue());
            static_cast<void>(observer.Enqueue(Notification(2, 4, 2)));
        });
        provider.join();

        CHECK(accepted.load());
        CHECK(callbackThreads.empty());
        const auto dispatched = observer.Dispatch();
        REQUIRE(dispatched.HasValue());
        CHECK(dispatched.Value() == 2);
        REQUIRE(revisions == std::vector<std::uint64_t>{2, 3});
        REQUIRE(callbackThreads == std::vector<std::thread::id>{ownerThread, ownerThread});
    }

    TEST_CASE("New session generations evict pending old-user notifications and reject late stale evidence",
              "[platform-services][session][observer][stale]") {
        auto created = PlatformSessionObserver::Create({.maxObservers = 2, .maxPendingNotifications = 4});
        REQUIRE(created.HasValue());
        auto observer = std::move(created).Value();
        std::vector<std::uint64_t> generations;
        auto subscription = observer.Subscribe([&](const PlatformSessionNotification &notification) {
            generations.push_back(notification.snapshot.Generation().value);
        });
        REQUIRE(subscription.HasValue());

        REQUIRE(observer.Enqueue(Notification(1, 10)).Value() == PlatformSessionNotificationAdmission::Queued);
        REQUIRE(observer.Enqueue(Notification(2, 11, 1, std::byte{2})).Value() == PlatformSessionNotificationAdmission::Queued);
        CHECK(observer.PendingCount() == 1);
        REQUIRE(observer.Dispatch().HasValue());
        REQUIRE(generations == std::vector<std::uint64_t>{11});

        const auto stale = observer.Enqueue(Notification(3, 10));
        REQUIRE(stale.HasValue());
        CHECK(stale.Value() == PlatformSessionNotificationAdmission::IgnoredStale);
    }

    TEST_CASE("Session revisions are deterministic under duplicates stale arrivals and reentrant dispatch",
              "[platform-services][session][observer][ordering]") {
        auto created = PlatformSessionObserver::Create({.maxObservers = 2, .maxPendingNotifications = 4});
        REQUIRE(created.HasValue());
        auto observer = std::move(created).Value();
        std::vector<std::uint64_t> revisions;
        std::optional<Result<std::size_t>> nestedDispatch;
        auto subscription = observer.Subscribe([&](const PlatformSessionNotification &notification) {
            revisions.push_back(notification.revision);
            if (notification.revision == 2) {
                nestedDispatch = observer.Dispatch();
                static_cast<void>(observer.Enqueue(Notification(4, 5, 4)));
            }
        });
        REQUIRE(subscription.HasValue());

        REQUIRE(observer.Enqueue(Notification(3, 5, 3)).Value() == PlatformSessionNotificationAdmission::Queued);
        REQUIRE(observer.Enqueue(Notification(2, 5, 2)).Value() == PlatformSessionNotificationAdmission::Queued);
        CHECK(observer.Enqueue(Notification(2, 5, 2)).Value() == PlatformSessionNotificationAdmission::IgnoredDuplicate);
        REQUIRE(observer.Dispatch().Value() == 2);
        REQUIRE(nestedDispatch.has_value());
        RequireError(*nestedDispatch, SessionObserverErrors::ReentrantDispatch);
        REQUIRE(revisions == std::vector<std::uint64_t>{2, 3});
        REQUIRE(observer.Dispatch().Value() == 1);

        const auto stale = observer.Enqueue(Notification(1, 5, 1));
        REQUIRE(stale.HasValue());
        CHECK(stale.Value() == PlatformSessionNotificationAdmission::IgnoredStale);
        CHECK(observer.LastPublishedRevision() == 4);
    }

    TEST_CASE("Wrong-thread dispatch and shutdown fail closed without invoking callbacks",
              "[platform-services][session][observer][lifecycle]") {
        auto created = PlatformSessionObserver::Create({.maxObservers = 2, .maxPendingNotifications = 2});
        REQUIRE(created.HasValue());
        auto observer = std::move(created).Value();
        std::atomic<std::uint32_t> callbackCount{0};
        auto subscription = observer.Subscribe([&](const PlatformSessionNotification &) {
            ++callbackCount;
        });
        REQUIRE(subscription.HasValue());
        REQUIRE(observer.Enqueue(Notification(1, 1)).HasValue());

        std::optional<Result<std::size_t>> wrongThread;
        std::thread worker([&] {
            wrongThread = observer.Dispatch();
        });
        worker.join();
        REQUIRE(wrongThread.has_value());
        RequireError(*wrongThread, SessionObserverErrors::WrongThread);
        CHECK(callbackCount.load() == 0);

        REQUIRE(observer.Close().HasValue());
        CHECK(observer.IsClosed());
        CHECK_FALSE(subscription.Value().IsActive());
        RequireError(observer.Enqueue(Notification(2, 2, 1, std::byte{2})), SessionObserverErrors::Closed);
        RequireError(observer.Dispatch(), SessionObserverErrors::Closed);
        CHECK(callbackCount.load() == 0);
    }
}  // namespace Horo::PlatformServices
