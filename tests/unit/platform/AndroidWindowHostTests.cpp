#include "AllocationProbe.h"
#include "Horo/Platform/Internal/AndroidLifecycleAdapter.h"
#include "Horo/Platform/Internal/AndroidWindowHost.h"
#include "Horo/Platform/PlatformErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <utility>

namespace Horo::Platform::Android::Tests {
    namespace {
        /** @brief Contains fixture assertions outside allocation-measurement intervals. */
        template <class T> T Value(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "Owner operation accepted."));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Preserves exact typed diagnostics in ordinary fixture setup. */
        void Accepted(const Result<void> &result) {
            INFO((result.HasError() ? result.ErrorValue().message : "Owner operation accepted."));
            REQUIRE(result.HasValue());
        }

        /** @brief Compares both domain and code, never any-error success. */
        template <class T> void Rejected(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        /** @brief Injected native-reference ledger; this is platform ownership protocol evidence, not an Android device. */
        struct Window final {
            AndroidWindowExtent extent{640, 480};
            std::size_t references{}, acquisitions{}, releases{};
        };

        /** @brief Records a trusted private test acquire without allocation or callbacks into the host. */
        void Acquire(void *, void *native) noexcept {
            auto &window = *static_cast<Window *>(native);
            ++window.references;
            ++window.acquisitions;
        }

        /** @brief Records exact private test reference release without pretending GPU completion. */
        void Release(void *, void *native) noexcept {
            auto &window = *static_cast<Window *>(native);
            --window.references;
            ++window.releases;
        }

        /** @brief Reads only the fixture's admitted native facts. */
        AndroidWindowExtent Extent(void *, void *native) noexcept {
            return static_cast<Window *>(native)->extent;
        }

        /** @brief Uses real lifecycle issuance/validation and the actual window owner with an explicit private native ledger. */
        struct Fixture final {
            std::array<Window, 10> windows;
            std::unique_ptr<AndroidLifecycleController> lifecycle;
            AndroidLifecycleAdapter adapter;
            AndroidActivityGeneration activity;
            std::unique_ptr<AndroidWindowHost> host;

            /** @brief Commits each bootstrap observation separately so capacity-one fixtures remain valid. */
            explicit Fixture(std::size_t capacity = AndroidLifecycleController::MaximumQueueCapacity,
                             AndroidWindowMode mode = AndroidWindowMode::Interactive)
                : lifecycle(Value(AndroidLifecycleController::Create(std::this_thread::get_id(), capacity))), adapter(*lifecycle) {
                Accepted(adapter.OnProcessCreated());
                Value(lifecycle->Drain());
                activity = Value(adapter.OnActivityCreated());
                Value(lifecycle->Drain());
                Accepted(adapter.OnActivityStarted(activity));
                Value(lifecycle->Drain());
                Accepted(adapter.OnActivityResumed(activity));
                Value(lifecycle->Drain());
                host = Value(AndroidWindowHost::Create(*lifecycle, adapter, mode, {nullptr, &Acquire, &Release, &Extent}));
            }

            /** @brief Commits the existing sole lifecycle before exposing actual window facts. */
            void Commit() {
                Value(lifecycle->Drain());
                Accepted(host->Refresh());
            }

            /** @brief Admits a genuine generated occurrence, without interpreting a pointer as identity. */
            AndroidWindowGeneration Available(std::size_t index = 0) {
                const auto generation = Value(host->ObserveAvailable(activity, &windows[index]));
                Commit();
                return generation;
            }
        };
    }  // namespace

    TEST_CASE("Android window factory rejects foreign authority and malformed private operations", "[unit][platform][android][window]") {
        Fixture fixture;
        auto foreign = Value(AndroidLifecycleController::Create(std::this_thread::get_id()));
        Rejected(AndroidWindowHost::Create(*foreign, fixture.adapter, AndroidWindowMode::Interactive), AndroidWindowErrors::InvalidInput);
        Rejected(AndroidWindowHost::Create(*fixture.lifecycle, fixture.adapter, static_cast<AndroidWindowMode>(255)),
                 AndroidWindowErrors::InvalidInput);
        Rejected(AndroidWindowHost::Create(*fixture.lifecycle, fixture.adapter, AndroidWindowMode::Interactive, {nullptr, &Acquire}),
                 AndroidWindowErrors::InvalidInput);
#if !defined(__ANDROID__)
        Rejected(AndroidWindowHost::Create(*fixture.lifecycle, fixture.adapter, AndroidWindowMode::Interactive),
                 AndroidWindowErrors::Unsupported);
#endif
    }

    TEST_CASE("Android headless window profile cannot admit native presentation", "[unit][platform][android][window]") {
        Fixture fixture{64, AndroidWindowMode::Headless};
        Rejected(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[0]), AndroidWindowErrors::Headless);
        Rejected(fixture.host->Acquire({1}), AndroidWindowErrors::Headless);
        CHECK_FALSE(Value(fixture.host->Snapshot()).available);
        CHECK(fixture.windows[0].acquisitions == 0);
        CHECK(Value(fixture.host->Close()));
    }

    TEST_CASE("Android window admission waits for actual lifecycle commit and native extent", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto generation = Value(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[0]));
        Rejected(fixture.host->Acquire(generation), AndroidWindowErrors::Pending);
        Rejected(fixture.host->Refresh(), AndroidWindowErrors::Pending);
        CHECK_FALSE(Value(fixture.host->Snapshot()).available);
        fixture.Commit();
        const auto actual = Value(fixture.lifecycle->Snapshot());
        auto lease = Value(fixture.host->Acquire(generation));
        CHECK(lease.IsCurrent());
        CHECK(lease.NativeHandle() == &fixture.windows[0]);
        CHECK(lease.Snapshot().window == actual.windowGeneration);
        CHECK(lease.Snapshot().activity == actual.activityGeneration);
        CHECK(lease.Snapshot().extent == fixture.windows[0].extent);
        CHECK_FALSE(actual.hasPresentation);
    }

    TEST_CASE("Android duplicate live pointer does not issue another lifecycle generation or native reference",
              "[unit][platform][android][window]") {
        Fixture fixture;
        const auto generation = fixture.Available();
        const auto revision = Value(fixture.host->Snapshot()).revision;
        CHECK(Value(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[0])) == generation);
        CHECK(Value(fixture.lifecycle->Snapshot()).queuedObservationCount == 0);
        CHECK(Value(fixture.host->Snapshot()).revision == revision);
        CHECK(fixture.windows[0].acquisitions == 1);
    }

    TEST_CASE("Android lost then same pointer creates a distinct retained native occurrence", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto first = fixture.Available();
        auto old = Value(fixture.host->Acquire(first));
        Accepted(fixture.host->ObserveLost(first));
        CHECK_FALSE(old.IsCurrent());
        CHECK(old.NativeHandle() == &fixture.windows[0]);
        CHECK(fixture.windows[0].releases == 0);
        fixture.Commit();
        const auto next = fixture.Available();
        REQUIRE(next > first);
        auto current = Value(fixture.host->Acquire(next));
        CHECK(current.IsCurrent());
        CHECK_FALSE(old.IsCurrent());
        CHECK(fixture.windows[0].references == 2);
        old = {};
        CHECK(fixture.windows[0].references == 1);
        CHECK(fixture.windows[0].releases == 1);
    }

    TEST_CASE("Android stale loss and Activity callbacks cannot retire a replacement", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto first = fixture.Available();
        auto old = Value(fixture.host->Acquire(first));
        const auto replacement = fixture.Available(1);
        auto current = Value(fixture.host->Acquire(replacement));
        Rejected(fixture.host->ObserveLost(first), AndroidWindowErrors::StaleGeneration);
        Rejected(fixture.host->ObserveAvailable({fixture.activity.value + 1}, &fixture.windows[2]), AndroidWindowErrors::StaleGeneration);
        CHECK(current.IsCurrent());
        CHECK_FALSE(old.IsCurrent());
        CHECK(fixture.windows[2].acquisitions == 0);
    }

    TEST_CASE("Android batched native loss and replacement admit only the final committed occurrence",
              "[unit][platform][android][window]") {
        Fixture fixture;
        const auto first = fixture.Available();
        auto old = Value(fixture.host->Acquire(first));
        Accepted(fixture.host->ObserveLost(first));
        const auto next = Value(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[1]));
        CHECK(Value(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[1])) == next);
        Rejected(fixture.host->Acquire(next), AndroidWindowErrors::Pending);
        CHECK_FALSE(old.IsCurrent());
        fixture.Commit();
        auto current = Value(fixture.host->Acquire(next));
        CHECK(current.IsCurrent());
        CHECK(current.NativeHandle() == &fixture.windows[1]);
        CHECK(fixture.windows[1].acquisitions == 1);
    }

    TEST_CASE("Android committed lifecycle loss cannot be mistaken for a duplicate same-pointer arrival",
              "[unit][platform][android][window]") {
        Fixture fixture;
        const auto first = fixture.Available();
        auto old = Value(fixture.host->Acquire(first));
        Accepted(fixture.adapter.OnWindowLost(first));
        Value(fixture.lifecycle->Drain());
        const auto next = Value(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[0]));
        REQUIRE(next > first);
        fixture.Commit();
        CHECK_FALSE(old.IsCurrent());
        CHECK(fixture.windows[0].acquisitions == 2);
        auto current = Value(fixture.host->Acquire(next));
        CHECK(current.IsCurrent());
    }

    TEST_CASE("Android resize fences retained facts and zero extent is never drawable", "[unit][platform][android][window]") {
        Fixture fixture;
        fixture.windows[0].extent = {};
        const auto generation = fixture.Available();
        REQUIRE(Value(fixture.host->Snapshot()).available);
        Rejected(fixture.host->Acquire(generation), AndroidWindowErrors::Pending);
        fixture.windows[0].extent = {320, 200};
        Accepted(fixture.host->Refresh());
        auto old = Value(fixture.host->Acquire(generation));
        const auto oldFacts = old.Snapshot();
        fixture.windows[0].extent = {200, 320};
        Accepted(fixture.host->Refresh());
        CHECK_FALSE(old.IsCurrent());
        CHECK(old.Snapshot().extent == oldFacts.extent);
        auto current = Value(fixture.host->Acquire(generation));
        CHECK(current.Snapshot().revision > oldFacts.revision);
        CHECK(current.IsCurrent());
    }

    TEST_CASE("Android lifecycle retirement independently revokes window admission", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto generation = fixture.Available();
        auto lease = Value(fixture.host->Acquire(generation));
        Accepted(fixture.adapter.OnActivityPaused(fixture.activity));
        Accepted(fixture.adapter.OnActivityStopped(fixture.activity));
        Accepted(fixture.adapter.OnActivityDestroyed(fixture.activity));
        Value(fixture.lifecycle->Drain());
        CHECK_FALSE(lease.IsCurrent());
        Accepted(fixture.host->Refresh());
        CHECK_FALSE(Value(fixture.host->Snapshot()).available);
        CHECK(lease.NativeHandle() == &fixture.windows[0]);
        lease = {};
        CHECK(fixture.windows[0].references == 0);
    }

    TEST_CASE("Android native occurrence exhaustion never overwrites retained generations", "[unit][platform][android][window]") {
        Fixture fixture;
        std::array<AndroidWindowLease, AndroidWindowHost::MaximumOccurrences> leases;
        for (std::size_t i = 0; i < leases.size(); ++i)
            leases[i] = Value(fixture.host->Acquire(fixture.Available(i)));
        Rejected(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[8]), AndroidWindowErrors::CapacityExceeded);
        CHECK_FALSE(Value(fixture.host->Snapshot()).available);
        CHECK(fixture.windows[8].acquisitions == 0);
        CHECK(leases[0].NativeHandle() == &fixture.windows[0]);
        leases[0] = {};
        const auto next = fixture.Available(8);
        auto current = Value(fixture.host->Acquire(next));
        CHECK(current.IsCurrent());
        CHECK(fixture.windows[0].references == 0);
    }

    TEST_CASE("Android bounded renderer lease capacity recovers only after an actual lease release", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto generation = fixture.Available();
        std::array<AndroidWindowLease, AndroidWindowHost::MaximumLeasesPerOccurrence> leases;
        for (auto &lease : leases)
            lease = Value(fixture.host->Acquire(generation));
        Rejected(fixture.host->Acquire(generation), AndroidWindowErrors::CapacityExceeded);
        CHECK(fixture.windows[0].acquisitions == 1);
        leases[0] = {};
        leases[0] = Value(fixture.host->Acquire(generation));
        CHECK(leases[0].IsCurrent());
    }

    TEST_CASE("Android callback queue saturation cannot claim a committed replacement", "[unit][platform][android][window]") {
        Fixture fixture{1};
        const auto first = fixture.Available();
        auto old = Value(fixture.host->Acquire(first));
        Accepted(fixture.adapter.OnActivityResumed(fixture.activity));
        Rejected(fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[1]), PlatformErrors::LifecycleQueueSaturated);
        CHECK_FALSE(old.IsCurrent());
        CHECK_FALSE(Value(fixture.host->Snapshot()).available);
        CHECK(Value(fixture.lifecycle->Snapshot()).windowGeneration == first);
        CHECK(fixture.windows[1].acquisitions == 0);
        fixture.Commit();
        const auto next = fixture.Available(1);
        CHECK(next.value > first.value + 1);  // Failed enqueue burns its issuance; no reused capability.
    }

    TEST_CASE("Android window owner rejects wrong-lane operations without changing retained storage", "[unit][platform][android][window]") {
        Fixture fixture;
        const auto generation = fixture.Available();
        auto lease = Value(fixture.host->Acquire(generation));
        bool snapshotDenied{}, acquireDenied{}, current{}, hidden{};
        std::thread other{[&] {
            const auto snapshot = fixture.host->Snapshot();
            snapshotDenied = snapshot.HasError() && snapshot.ErrorValue().code.Value() == AndroidWindowErrors::WrongThread.code.Value();
            const auto borrowed = fixture.host->Acquire(generation);
            acquireDenied = borrowed.HasError() && borrowed.ErrorValue().code.Value() == AndroidWindowErrors::WrongThread.code.Value();
            current = lease.IsCurrent();
            hidden = lease.NativeHandle() == nullptr;
        }};
        other.join();
        CHECK(snapshotDenied);
        CHECK(acquireDenied);
        CHECK_FALSE(current);
        CHECK(hidden);
        CHECK(lease.IsCurrent());
        CHECK(fixture.windows[0].references == 1);
    }

    TEST_CASE("Android close waits only for renderer leases and never for an invented GPU idle", "[unit][platform][android][window]") {
        Fixture fixture;
        auto lease = Value(fixture.host->Acquire(fixture.Available()));
        CHECK_FALSE(Value(fixture.host->Close()));
        CHECK_FALSE(lease.IsCurrent());
        CHECK(lease.NativeHandle() == &fixture.windows[0]);
        Rejected(fixture.host->Acquire(lease.Snapshot().window), AndroidWindowErrors::Closed);
        lease = {};
        CHECK(Value(fixture.host->Close()));
        CHECK(Value(fixture.host->Close()));
        CHECK(fixture.windows[0].acquisitions == 1);
        CHECK(fixture.windows[0].releases == 1);
    }

    TEST_CASE("Android window successful observation publication and lease retirement allocate no C++ heap",
              "[unit][platform][android][window]") {
        Fixture fixture;
        bool accepted{};
        std::size_t requests{};
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement measurement;
            auto observed = fixture.host->ObserveAvailable(fixture.activity, &fixture.windows[0]);
            auto drained = fixture.lifecycle->Drain();
            auto refreshed = fixture.host->Refresh();
            auto snapshot = fixture.host->Snapshot();
            auto acquired = fixture.host->Acquire(observed.Value());
            auto lease = std::move(acquired).Value();
            auto closed = fixture.host->Close();
            lease = {};
            auto complete = fixture.host->Close();
            accepted = observed.HasValue() && drained.HasValue() && refreshed.HasValue() && snapshot.HasValue() && closed.HasValue() &&
                       !closed.Value() && complete.HasValue() && complete.Value();
            requests = measurement.Snapshot().requests;
        }
        REQUIRE(accepted);
        CHECK(requests == 0);
        CHECK(fixture.windows[0].references == 0);
    }
}  // namespace Horo::Platform::Android::Tests
