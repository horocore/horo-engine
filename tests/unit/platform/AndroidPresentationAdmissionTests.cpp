#include "Horo/Apps/AndroidQualification/PresentationAdmission.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace Horo::AndroidQualification::Tests {
    namespace {
        using namespace Horo::Platform;
        using namespace Horo::Platform::Android;

        /** @brief Preserves canonical diagnostics in protocol fixture setup. */
        template <class T> T Value(Result<T> result) {
            INFO((result.HasError() ? result.ErrorValue().message : "Actual owner operation accepted."));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        /** @brief Requires real authority calls to succeed without flattening typed errors. */
        void Accepted(const Result<void> &result) {
            INFO((result.HasError() ? result.ErrorValue().message : "Actual owner operation accepted."));
            REQUIRE(result.HasValue());
        }

        /** @brief Compares both parts of an exact typed failure. */
        void Rejected(const Result<void> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == error.domain.Value());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        /** @brief Explicit native-reference protocol fixture; no Android device or GPU readiness is simulated as qualification. */
        struct NativeWindow final {
            AndroidWindowExtent extent{640, 480};
            std::size_t references{};
        };

        /** @brief Records actual private window-owner acquisition in this protocol-only ledger. */
        void Acquire(void *, void *window) noexcept {
            ++static_cast<NativeWindow *>(window)->references;
        }

        /** @brief Records the exact matching protocol reference release. */
        void Release(void *, void *window) noexcept {
            --static_cast<NativeWindow *>(window)->references;
        }

        /** @brief Supplies explicit protocol window dimensions to the genuine owner. */
        AndroidWindowExtent Extent(void *, void *window) noexcept {
            return static_cast<NativeWindow *>(window)->extent;
        }

        /** @brief Drives real lifecycle issuance/window ownership and the production app helper, with explicit native protocol operations.
         */
        struct Fixture final {
            std::array<NativeWindow, 2> native;
            std::unique_ptr<AndroidLifecycleController> lifecycle{Value(AndroidLifecycleController::Create(std::this_thread::get_id()))};
            AndroidLifecycleAdapter adapter{*lifecycle};
            std::unique_ptr<AndroidWindowHost> windows{
                Value(AndroidWindowHost::Create(*lifecycle, adapter, AndroidWindowMode::Interactive, {nullptr, Acquire, Release, Extent}))};
            PresentationAdmission admission{*lifecycle, adapter};
            AndroidActivityGeneration activity;
            AndroidWindowGeneration window;

            Fixture() {
                Accepted(adapter.OnProcessCreated());
                activity = Value(adapter.OnActivityCreated());
                Commit();
                window = Value(windows->ObserveAvailable(activity, &native[0]));
                Commit();
            }

            ~Fixture() {
                CHECK(Value(windows->Close()));
                CHECK(native[0].references == 0);
                CHECK(native[1].references == 0);
            }

            /** @brief Commits genuine queued observations before refreshing native facts. */
            void Commit() {
                static_cast<void>(Value(lifecycle->Drain()));
                Accepted(windows->Refresh());
            }

            /** @brief Borrows the current authentic derived window facts. */
            AndroidWindowSnapshot Facts() {
                return Value(windows->Snapshot());
            }

            /** @brief Reads canonical presentation publication. */
            AndroidLifecycleSnapshot State() {
                return Value(lifecycle->Snapshot());
            }

            /** @brief Replaces the actual occurrence through a single batched cutoff. */
            void Replace() {
                Accepted(windows->ObserveLost(window));
                window = Value(windows->ObserveAvailable(activity, &native[1]));
                Commit();
            }
        };
    }  // namespace

    TEST_CASE("Android presentation admission retains duplicate ready receipt and retires exact lost generation",
              "[platform][android][presentation]") {
        Fixture fixture;
        const auto facts = fixture.Facts();
        Accepted(fixture.admission.Reconcile(facts, true));
        const auto first = fixture.State();
        REQUIRE(first.hasPresentation);
        Accepted(fixture.admission.Reconcile(facts, true));
        CHECK(fixture.State().presentationGeneration == first.presentationGeneration);
        Accepted(fixture.admission.Reconcile(facts, false));
        CHECK_FALSE(fixture.State().hasPresentation);
        CHECK(fixture.State().hasWindow);
        Accepted(fixture.admission.Reconcile(facts, true));
        CHECK(fixture.State().presentationGeneration > first.presentationGeneration);
    }

    TEST_CASE("Android presentation admission refreshes resize generation without a false readiness poll",
              "[platform][android][presentation]") {
        Fixture fixture;
        const auto before = fixture.Facts();
        Accepted(fixture.admission.Reconcile(before, true));
        const auto first = fixture.State().presentationGeneration;
        fixture.native[0].extent = {800, 600};
        Accepted(fixture.windows->Refresh());
        const auto resized = fixture.Facts();
        REQUIRE(resized.window == before.window);
        REQUIRE(resized.revision > before.revision);
        Accepted(fixture.admission.Reconcile(resized, true));
        const auto current = fixture.State();
        CHECK(current.hasPresentation);
        CHECK(current.windowGeneration == before.window);
        CHECK(current.presentationGeneration > first);
        Rejected(fixture.admission.Reconcile(before, true), PresentationAdmissionErrors::InvalidReceipt);
        CHECK(fixture.State().presentationGeneration == current.presentationGeneration);
        Accepted(fixture.admission.Reconcile(resized, true));
        CHECK(fixture.State().presentationGeneration == current.presentationGeneration);
    }

    TEST_CASE("Android presentation admission follows genuine replacement and rejects stale ready receipt",
              "[platform][android][presentation]") {
        Fixture fixture;
        const auto old = fixture.Facts();
        Accepted(fixture.admission.Reconcile(old, true));
        const auto first = fixture.State().presentationGeneration;
        fixture.Replace();
        const auto replacement = fixture.Facts();
        REQUIRE(replacement.window > old.window);
        Accepted(fixture.admission.Reconcile(replacement, true));
        const auto current = fixture.State();
        REQUIRE(current.hasPresentation);
        CHECK(current.presentationGeneration > first);
        Rejected(fixture.admission.Reconcile(old, true), PresentationAdmissionErrors::InvalidReceipt);
        Rejected(fixture.admission.Reconcile(old, false), PresentationAdmissionErrors::InvalidReceipt);
        CHECK(fixture.State().presentationGeneration == current.presentationGeneration);
        CHECK(fixture.State().hasPresentation);
    }

    TEST_CASE("Android presentation admission refuses an uncommitted lifecycle cutoff and recovers after real commit",
              "[platform][android][presentation]") {
        Fixture fixture;
        const auto facts = fixture.Facts();
        Accepted(fixture.admission.Reconcile(facts, true));
        const auto first = fixture.State().presentationGeneration;
        Accepted(fixture.adapter.OnActivityStarted(fixture.activity));
        Rejected(fixture.admission.Reconcile(facts, false), AndroidWindowErrors::Pending);
        CHECK(fixture.State().presentationGeneration == first);
        CHECK(fixture.State().hasPresentation);
        fixture.Commit();
        Accepted(fixture.admission.Reconcile(fixture.Facts(), true));
        CHECK(fixture.State().presentationGeneration == first);
    }

    TEST_CASE("Android presentation admission rejects malformed ready facts without erasing accepted receipt",
              "[platform][android][presentation]") {
        Fixture fixture;
        auto facts = fixture.Facts();
        Accepted(fixture.admission.Reconcile(facts, true));
        const auto first = fixture.State().presentationGeneration;
        facts.extent.width = 0;
        Rejected(fixture.admission.Reconcile(facts, true), PresentationAdmissionErrors::InvalidReceipt);
        facts = fixture.Facts();
        facts.revision = 0;
        Rejected(fixture.admission.Reconcile(facts, true), PresentationAdmissionErrors::InvalidReceipt);
        CHECK(fixture.State().hasPresentation);
        CHECK(fixture.State().presentationGeneration == first);
        Accepted(fixture.admission.Reconcile(fixture.Facts(), true));
        CHECK(fixture.State().presentationGeneration == first);
    }
}  // namespace Horo::AndroidQualification::Tests
