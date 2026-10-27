#include "Horo/Runtime/RuntimeHost.h"
#include "OpenXRInputClock.h"
#include "support/OpenXRClockTestSupport.h"

#include <bit>
#include <optional>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

namespace Horo::Tests::OpenXR {
    namespace {
        using Application::Internal::OpenXRInputClock;
        static_assert(!std::is_copy_constructible_v<OpenXRInputClock>);
        static_assert(!std::is_copy_assignable_v<OpenXRInputClock>);

        /** @brief Reads only inside a real host callback and retains replay evidence for rejection tests. */
        class ClockReader final : public Runtime::RuntimeLifecycleParticipant {
        public:
            explicit ClockReader(OpenXRInputClock &owner) : clock(owner) {}

            Result<void> Startup(const CancellationToken &) override {
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {}

            Result<void> OnFixedUpdate(const Runtime::FixedStepContext &) override {
                return Result<void>::Success();
            }

            Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &context) override {
                if (phase == Runtime::RuntimePhase::BuildInputSnapshot) {
                    saved.emplace(context);
                    observed.emplace(clock.Read(context));
                    auto alteredObservations = context;
                    alteredObservations.completedSimulationTick = 999;
                    altered.emplace(clock.Read(alteredObservations));
                }
                if (phase == Runtime::RuntimePhase::VariableUpdate)
                    wrongPhaseRejected = clock.Read(context).HasError();
                return Result<void>::Success();
            }

            OpenXRInputClock &clock;
            std::optional<Runtime::FrameContext> saved;
            std::optional<Result<XRTimeEvidence>> observed;
            std::optional<Result<XRTimeEvidence>> altered;
            bool wrongPhaseRejected{};
        };

        /** @brief Owns native/script/scheduler parents until the actual clock and every callback have retired. */
        struct ClockFixture final {
            Fixture native;
            DeterministicClock schedulerClock;
            OpenXRInputClock clock{native.owner};
            ClockScript script{&clock};
            std::unique_ptr<Runtime::RuntimeHost> host; /**< Retires callbacks before the borrowed clock/script parents. */
            static constexpr Duration Step = Duration::FromMilliseconds(10);

            explicit ClockFixture(bool extension = true) {
                auto created = Runtime::RuntimeHost::Create(schedulerClock, {.fixedStep = Step});
                REQUIRE(created.HasValue());
                host = std::move(created).Value();
                native.script.clockExtension = extension;
                native.script.extensionCount = extension ? 2U : 1U;
                auto request = native.Request();
                const std::array extensions{NativeExtensionRequest{ClockExtension, true}};
                if (extension)
                    request.extensions = extensions;
                REQUIRE(native.owner.Create(request).HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Production XR clock reads the same host cutoff and rejects phase and replay substitutions", "[xr][host][clock]") {
        ClockFixture fixture;
        REQUIRE(fixture.clock.Bind(fixture.host->DispatchSource(), ClockFixture::Step, {&ClockScript::Resolve}).HasValue());
        auto participant = std::make_unique<ClockReader>(fixture.clock);
        auto *reader = participant.get();
        REQUIRE(fixture.host->AddParticipant(std::move(participant)).HasValue());
        REQUIRE(fixture.host->Startup().HasValue());
        REQUIRE(fixture.host->RunFrame().HasValue());
        REQUIRE(reader->observed.has_value());
        REQUIRE(reader->observed->HasValue());
        REQUIRE(reader->observed->Value().simulation.has_value());
        CHECK(reader->observed->Value().runtimeSample.Nanoseconds() == fixture.script.sample);
        CHECK(reader->observed->Value().simulation->Nanoseconds() == 0);
        REQUIRE(reader->altered->HasValue());
        CHECK(reader->altered->Value().simulation == reader->observed->Value().simulation);
        CHECK(reader->wrongPhaseRejected);
        const auto calls = fixture.script.conversions;
        CHECK(fixture.clock.Read(*reader->saved).HasError());
        CHECK(fixture.script.conversions == calls);
        fixture.schedulerClock.Advance(ClockFixture::Step);
        REQUIRE(fixture.host->RunFrame().HasValue());
        REQUIRE(reader->observed.has_value());
        REQUIRE(reader->observed->HasValue());
        REQUIRE(reader->observed->Value().simulation.has_value());
        // Input precedes this frame's fixed dispatch; the next cutoff observes its committed tick.
        CHECK(reader->observed->Value().simulation->Nanoseconds() == 0);
        REQUIRE(fixture.host->RunFrame().HasValue());
        REQUIRE(reader->observed.has_value());
        REQUIRE(reader->observed->HasValue());
        REQUIRE(reader->observed->Value().simulation.has_value());
        CHECK(reader->observed->Value().simulation->Nanoseconds() == ClockFixture::Step.ToNanoseconds());
    }

    TEST_CASE("Production XR clock preserves prediction purpose and rejects reentry and foreign lanes", "[xr][host][clock]") {
        ClockFixture fixture;
        REQUIRE(fixture.clock.Bind(fixture.host->DispatchSource(), ClockFixture::Step, {&ClockScript::Resolve}).HasValue());
        fixture.script.reenter = true;
        const auto prediction = IdentityValue<XRRenderPredictionTime>(50'000'000);
        const auto sampled = fixture.clock.PresentationTime(prediction);
        REQUIRE(sampled.HasValue());
        CHECK(sampled.Value().renderPrediction == prediction);
        CHECK_FALSE(sampled.Value().simulation.has_value());
        CHECK(fixture.script.nestedRejected);
        const auto calls = fixture.script.conversions;
        CHECK(fixture.clock.PresentationTime({}).HasError());
        bool rejected{};
        std::thread foreign{[&] {
            rejected = fixture.clock.PresentationNow().HasError();
        }};
        foreign.join();
        CHECK(rejected);
        CHECK(fixture.script.conversions == calls);
        fixture.native.script.stale = true;
        CHECK(fixture.clock.PresentationNow().HasError());
        CHECK(fixture.script.conversions == calls);
        fixture.native.script.stale = false;
    }

    TEST_CASE("Production XR clock rejects failed conversion and invalid native time without replacing its source", "[xr][host][clock]") {
        ClockFixture fixture;
        REQUIRE(fixture.clock.Bind(fixture.host->DispatchSource(), ClockFixture::Step, {&ClockScript::Resolve}).HasValue());
        SECTION("native failure") {
            fixture.script.result = XR_ERROR_RUNTIME_FAILURE;
        }
        SECTION("invalid native time") {
            fixture.script.sample = 0;
        }
        CHECK(fixture.clock.PresentationNow().HasError());
        fixture.script.result = XR_SUCCESS;
        fixture.script.sample = 42'000'000;
        REQUIRE(fixture.clock.PresentationNow().HasValue());
        auto foreign = fixture.native.Request().candidate;
        ++foreign.slot.generation;
        CHECK(fixture.clock.RetireNative(foreign).HasError());
        REQUIRE(fixture.clock.PresentationNow().HasValue());
        REQUIRE(fixture.clock.RetireNative(fixture.native.Request().candidate).HasValue());
        CHECK(fixture.clock.PresentationNow().HasError());
        REQUIRE(fixture.clock.BindNative({&ClockScript::Resolve}).HasValue());
    }

    TEST_CASE("Production XR clock binding fails closed and rolls back a failed combined binding", "[xr][host][clock]") {
        ClockFixture fixture;
        CHECK(fixture.clock.Bind({}, ClockFixture::Step, {&ClockScript::Resolve}).HasError());
        CHECK(fixture.clock.Bind(fixture.host->DispatchSource(), Duration{}, {&ClockScript::Resolve}).HasError());
        fixture.script.missing = true;
        CHECK(fixture.clock.Bind(fixture.host->DispatchSource(), ClockFixture::Step, {&ClockScript::Resolve}).HasError());
        CHECK(fixture.clock.PresentationNow().HasError());
        fixture.script.missing = false;
        REQUIRE(fixture.clock.Bind(fixture.host->DispatchSource(), ClockFixture::Step, {&ClockScript::Resolve}).HasValue());
        CHECK(fixture.clock.BindNative({&ClockScript::Resolve}).HasError());
    }

    TEST_CASE("Production XR clock requires the extension enabled on its exact native owner", "[xr][host][clock]") {
        ClockFixture fixture{false};
        CHECK(fixture.clock.Bind(fixture.host->DispatchSource(), ClockFixture::Step, {&ClockScript::Resolve}).HasError());
        CHECK(fixture.script.conversions == 0);
    }
}  // namespace Horo::Tests::OpenXR
