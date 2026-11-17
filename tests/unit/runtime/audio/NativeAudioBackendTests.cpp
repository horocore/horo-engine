#include "AllocationProbe.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/Internal/AudioDeviceSwitch.h"
#include "Horo/Audio/Internal/AudioRenderEnvironment.h"
#include "Horo/Audio/Internal/NativeAudioBackend.h"
#include "Horo/Audio/Internal/NativeAudioFacts.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <limits>
#include <new>
#include <thread>

using namespace Horo;
using namespace Horo::Audio;
using namespace Horo::Audio::Backend;

namespace {
    const auto Owner = AudioRuntimeId::Create(17).Value();
    constexpr AudioMonotonicTimestamp Deadline{7, std::numeric_limits<std::uint64_t>::max()};

    /** @brief Deterministic native primitive fixture; it does not qualify any OS or device. */
    class Driver final : public NativeAudioDriver {
    public:
        explicit Driver(const AudioBackendKind peer) : peer_(peer) {}

        AudioBackendKind Kind() const noexcept override {
            return peer_;
        }

        ResourceDisposition Resources() const noexcept override {
            return running ? ResourceDisposition::Retained : opened ? ResourceDisposition::CallbackDetached : ResourceDisposition::Closed;
        }

        bool HasUndrainedRequiredFacts() const noexcept override {
            return lost;
        }

        Result<AudioBackendProbe> Probe() override {
            AudioBackendProbe probe{.backend = peer_,
                                    .compiled = true,
                                    .hostSupported = true,
                                    .availability = AudioBackendAvailability::Available,
                                    .revision = 1,
                                    .backendVersion = "fixture"};
            probe.features.fill(AudioCapabilitySupport::Unsupported);
            return Result<AudioBackendProbe>::Success(std::move(probe));
        }

        Result<AudioDeviceSnapshot> Enumerate() override {
            if (failEnumeration)
                return Result<AudioDeviceSnapshot>::Failure(MakeError(AudioErrors::DeviceUnavailable));
            return Result<AudioDeviceSnapshot>::Success(
                {.owner = Owner,
                 .backend = peer_,
                 .revision = catalogRevision,
                 .devices = {{catalogDevice, "Fixture", AudioDeviceClass::Virtual}},
                 .defaults = {catalogDevice, multimediaAvailable ? std::optional(catalogDevice) : std::nullopt, std::nullopt}});
        }

        Result<Opened> Open(const Backend::Open &request) override {
            if (failOpen)
                return Result<Opened>::Failure(MakeError(AudioErrors::DeviceUnavailable));
            opened = true;
            if (exhaustOpen)
                throw std::bad_alloc{};
            epoch = request.plannedEpoch;
            auto format = NegotiateNativeFloatOutput(request, nativeFormat ? *nativeFormat : request.format.preferred, catalogRevision,
                                                     request.format.period.preferredFrames);
            if (format.HasError())
                return Result<Opened>::Failure(format.ErrorValue());
            auto negotiated = std::move(format).Value();
            if (badFormat)
                ++negotiated.formatRevision;
            return Result<Opened>::Success(
                {std::move(negotiated), {.epoch = epoch, .capturedAt = {badTimingDomain ? 8u : 7u, 0}}, AccessMode::Shared});
        }

        Result<void> Start(const NativeAudioRenderPort port) override {
            ++starts;
            render = port;
            running = true;
            return Result<void>::Success();
        }

        NativeRetirementResult Stop(AudioMonotonicTimestamp) override {
            ++stops;
            if (failStop)
                return NativeRetirementResult::Failure(MakeError(AudioErrors::BackendFailed));
            if (pendingStops != 0) {
                --pendingStops;
                return NativeRetirementResult::Success(NativeRetirementProgress::Pending);
            }
            render = {};
            running = false;
            return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
        }

        NativeRetirementResult Close() override {
            ++closes;
            if (failClose)
                return NativeRetirementResult::Failure(MakeError(AudioErrors::BackendFailed));
            if (pendingCloses != 0) {
                --pendingCloses;
                return NativeRetirementResult::Success(NativeRetirementProgress::Pending);
            }
            opened = false;
            return NativeRetirementResult::Success(NativeRetirementProgress::Complete);
        }

        std::size_t DrainEvents(const std::span<Event> output) noexcept override {
            if (!lost || output.empty())
                return 0;
            output[0] = {Owner, DeviceLost{epoch.device, DeviceLossCause::Disconnected}};
            lost = false;
            return 1;
        }

        void Emit(std::span<float> output) noexcept {
            if (render.process)
                render.process(render.context, output, static_cast<std::uint32_t>(output.size() / 2));
        }

        AudioBackendKind peer_;
        NativeAudioRenderPort render;
        AudioDeviceEpoch epoch;
        std::optional<AudioProcessingFormat> nativeFormat;
        bool opened{}, running{}, failOpen{}, failStop{}, failClose{}, exhaustOpen{}, badFormat{}, badTimingDomain{}, lost{};
        std::uint32_t starts{}, stops{}, closes{};
        std::uint32_t pendingStops{}, pendingCloses{};
        bool failEnumeration{}, multimediaAvailable{true};
        std::uint64_t catalogRevision{1};
        AudioDeviceId catalogDevice{Owner, 1, 1};
    };

    struct Core final {
        std::uint32_t calls{}, quiesced{}, failOnCall{};
        bool fail{}, nonfinite{}, fade{};

        static RenderResult Process(void *context, const RenderInvocation &invocation) noexcept {
            auto &self = *static_cast<Core *>(context);
            ++self.calls;
            for (auto *plane : invocation.output.planes)
                std::fill_n(plane, invocation.output.validFrames, self.nonfinite ? std::numeric_limits<float>::infinity() : 2.0F);
            if (self.fade)
                for (auto *plane : invocation.output.planes)
                    for (std::uint32_t frame = 0; frame < invocation.output.validFrames; ++frame)
                        plane[frame] = invocation.output.validFrames <= 1
                                           ? 0.0F
                                           : 1.0F - static_cast<float>(frame) / static_cast<float>(invocation.output.validFrames - 1);
            if (self.fail || (self.failOnCall != 0 && self.calls == self.failOnCall))
                return {};
            if (invocation.phase == RenderPhase::Priming)
                return {RenderDisposition::Ready, AudioCallbackFaultCode::None};
            if (invocation.phase == RenderPhase::Quiescing) {
                ++self.quiesced;
                return {RenderDisposition::Quiesced, AudioCallbackFaultCode::None};
            }
            return {RenderDisposition::Rendered, AudioCallbackFaultCode::None};
        }
    };

    struct Fixture final {
        Driver *driver{};
        std::unique_ptr<NativeAudioBackend> backend;
        Core core;
        Backend::Open open{.plannedEpoch = {{Owner, 1, 1}, 1, 1},
                           .format = {.device = AudioDefaultDeviceRole::Multimedia,
                                      .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                                      .period = {8, 16, 64}}};

        explicit Fixture(const AudioBackendKind peer = AudioBackendKind::WASAPI) {
            auto primitive = std::make_unique<Driver>(peer);
            driver = primitive.get();
            backend = std::move(CreateNativeAudioBackend({Owner, 7}, std::move(primitive))).Value();
        }

        Completion Complete(const Request &request) {
            const auto id = backend->Begin(request, Deadline).Value();
            REQUIRE(backend->AdvanceControl().HasValue());
            auto result = backend->Poll(id);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            const auto completion = *result.Value();
            REQUIRE(backend->AcknowledgeCompletion(id).HasValue());
            return completion;
        }

        void Start() {
            Complete(Enumerate{});
            Complete(open);
            Complete(Backend::Start{open.plannedEpoch, {&core, Core::Process}});
        }

        void Quiesce() {
            const auto id = backend->Begin(Backend::Quiesce{open.plannedEpoch}, Deadline).Value();
            REQUIRE(backend->AdvanceControl().HasValue());
            std::array<float, 64> output{};
            driver->Emit(output);
            REQUIRE(backend->AdvanceControl().HasValue());
            REQUIRE(backend->Poll(id).Value().has_value());
            REQUIRE(backend->AcknowledgeCompletion(id).HasValue());
        }
    };

    TEST_CASE("Native period selection retains legal quantum multiples at the upper request boundary", "[audio][native-contract]") {
        struct PeriodCase final {
            AudioDevicePeriodRequest request;
            std::uint32_t minimum, maximum, quantum;
            std::optional<std::uint32_t> expected;
        };

        constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
        const std::array cases{
            PeriodCase{{128, 500, 500}, 128, 1024, 128, 384},
            PeriodCase{{128, 257, 500}, 128, 1024, 128, 384},
            PeriodCase{{128, 256, 500}, 128, 1024, 128, 256},
            PeriodCase{{129, 129, 384}, 1, 1024, 128, 256},
            PeriodCase{{128, 128, 512}, 257, 512, 128, 384},
            PeriodCase{{128, 500, 500}, 128, 256, 128, 256},
            PeriodCase{{129, 130, 255}, 128, 1024, 128, std::nullopt},
            PeriodCase{{128, 256, 512}, 1024, 2048, 128, std::nullopt},
            PeriodCase{{128, 256, 512}, 128, 1024, 0, std::nullopt},
            PeriodCase{{0, 128, 512}, 128, 1024, 128, std::nullopt},
            PeriodCase{{256, 128, 512}, 128, 1024, 128, std::nullopt},
            PeriodCase{{128, 513, 512}, 128, 1024, 128, std::nullopt},
            PeriodCase{{128, 256, 512}, 1024, 128, 128, std::nullopt},
            PeriodCase{{128, 256, 512}, 0, 1024, 128, std::nullopt},
            PeriodCase{{maximum - 5, maximum, maximum}, 1, maximum, 8, std::nullopt},
            PeriodCase{{1, maximum, maximum}, 1, maximum, 8, maximum - 7},
            PeriodCase{{1, maximum, maximum}, 1, maximum, maximum, maximum},
            PeriodCase{{1, 1, maximum}, 1, maximum, maximum, maximum},
        };
        for (const auto &value : cases) {
            INFO(value.request.minimumFrames << "/" << value.request.preferredFrames << "/" << value.request.maximumFrames);
            CHECK(SelectNativeAudioPeriod(value.request, value.minimum, value.maximum, value.quantum) == value.expected);
        }
    }

    TEST_CASE("Every native peer uses exact readiness quiescence detach and retained terminals", "[audio][native-contract]") {
        for (const auto peer : {AudioBackendKind::WASAPI, AudioBackendKind::CoreAudio, AudioBackendKind::PipeWire}) {
            Fixture fixture(peer);
            fixture.Start();
            std::array<float, 64> output;
            fixture.driver->Emit(output);
            REQUIRE(std::all_of(output.begin(), output.end(), [](const float sample) {
                return sample == 0.0F;
            }));
            REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
            Tests::AllocationProbe::Measurement measured;
            {
                Tests::AllocationProbe::ScopedMeasurement probe;
                fixture.driver->Emit(output);
                measured = probe.Snapshot();
            }
            REQUIRE(measured.requests == 0);
            REQUIRE(std::all_of(output.begin(), output.end(), [](const float sample) {
                return sample == 1.0F;
            }));
            fixture.Quiesce();
            const auto calls = fixture.core.calls;
            fixture.driver->Emit(output);
            REQUIRE(fixture.core.calls == calls);
            REQUIRE(fixture.core.quiesced == 1);
            const auto stopped = fixture.Complete(Stop{fixture.open.plannedEpoch});
            REQUIRE(std::holds_alternative<Stopped>(stopped.outcome));
            fixture.Complete(Stop{fixture.open.plannedEpoch});
            REQUIRE(fixture.driver->stops == 1);
            fixture.Complete(Close{});
        }
    }

    TEST_CASE("Native quiescence preserves a rendered fade across smaller native packets", "[audio][native-contract]") {
        for (const auto peer : {AudioBackendKind::WASAPI, AudioBackendKind::CoreAudio, AudioBackendKind::PipeWire}) {
            Fixture fixture(peer);
            fixture.Start();
            std::array<float, 32> prime{};
            fixture.driver->Emit(prime);
            REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
            fixture.core.fade = true;
            std::array<float, 10> prefix{};
            fixture.driver->Emit(prefix);
            REQUIRE(prefix[0] == 1.0F);
            REQUIRE(fixture.core.calls == 2);
            const auto quiesce = fixture.backend->Begin(Quiesce{fixture.open.plannedEpoch}, Deadline).Value();
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            std::array<float, 22> tail{};
            const auto freesBefore = Tests::AllocationProbe::FreeCount();
            Tests::AllocationProbe::Measurement measured;
            {
                Tests::AllocationProbe::ScopedMeasurement probe;
                fixture.driver->Emit(tail);
                measured = probe.Snapshot();
            }
            const auto freesAfter = Tests::AllocationProbe::FreeCount();
            REQUIRE(measured.requests == 0);
            REQUIRE(freesAfter == freesBefore);
            for (std::uint32_t frame = 0; frame < 11; ++frame) {
                const auto expected = 1.0F - static_cast<float>(frame + 5) / 15.0F;
                REQUIRE(tail[frame * 2] == expected);
                REQUIRE(tail[frame * 2 + 1] == expected);
            }
            REQUIRE(tail.back() == 0.0F);
            REQUIRE(fixture.core.calls == 2);
            REQUIRE(fixture.core.quiesced == 0);
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            REQUIRE_FALSE(fixture.backend->Poll(quiesce).Value().has_value());
            std::array<float, 2> acknowledgement{};
            fixture.driver->Emit(acknowledgement);
            REQUIRE(acknowledgement[0] == 0.0F);
            REQUIRE(acknowledgement[1] == 0.0F);
            REQUIRE(fixture.core.quiesced == 1);
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            REQUIRE(std::holds_alternative<Quiesced>(fixture.backend->Poll(quiesce).Value()->outcome));
            REQUIRE(fixture.backend->AcknowledgeCompletion(quiesce).HasValue());
            fixture.Complete(Stop{fixture.open.plannedEpoch});
            fixture.Complete(Close{});
        }
    }

    TEST_CASE("Native candidate rejection closes acquired resources and cancellation performs no acquisition", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        const auto id = fixture.backend->Begin(fixture.open, Deadline).Value();
        REQUIRE(fixture.backend->Cancel(id).HasValue());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(std::holds_alternative<Cancelled>(fixture.backend->Poll(id).Value()->outcome));
        REQUIRE_FALSE(fixture.driver->opened);
        REQUIRE(fixture.backend->Poll(id).Value()->operation == id);
        REQUIRE(fixture.backend->AcknowledgeCompletion(id).HasValue());
        SECTION("Foreign format revision") {
            fixture.driver->badFormat = true;
        }
        SECTION("Foreign timing clock domain") {
            fixture.driver->badTimingDomain = true;
        }
        const auto rejected = fixture.Complete(fixture.open);
        REQUIRE(std::get<Failed>(rejected.outcome).resources == ResourceDisposition::Closed);
        REQUIRE_FALSE(fixture.driver->opened);
        REQUIRE(fixture.driver->closes == 1);
    }

    TEST_CASE("A new native epoch cannot overwrite undrained lifecycle facts", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        std::array<float, 64> output{};
        fixture.driver->Emit(output);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        fixture.Quiesce();
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
        auto replacement = fixture.open;
        ++replacement.plannedEpoch.callbackEpoch;
        ++replacement.plannedEpoch.formatRevision;
        REQUIRE(fixture.backend->Begin(replacement, Deadline).HasError());
        std::array<Event, 8> events;
        const auto count = fixture.backend->DrainEvents(events);
        REQUIRE(count == 2);
        for (std::size_t index = 0; index < count; ++index)
            REQUIRE(std::get<AudioCallbackEvent>(events[index].fact).epoch == fixture.open.plannedEpoch);
        REQUIRE(std::holds_alternative<Opened>(fixture.Complete(replacement).outcome));
        fixture.Complete(Close{});
    }

    TEST_CASE("Retained native acquisition failure cannot attach an unprepared render candidate", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        fixture.driver->badFormat = true;
        fixture.driver->failClose = true;
        const auto rejected = fixture.Complete(fixture.open);
        REQUIRE(std::get<Failed>(rejected.outcome).resources == ResourceDisposition::Retained);
        const auto &error = std::get<Failed>(rejected.outcome).error;
        REQUIRE(error.code.Value() == AudioErrors::BackendFailed.code.Value());
        REQUIRE(error.cause.Get() != nullptr);
        REQUIRE(error.cause.Get()->code.Value() == AudioErrors::DeviceFormatUnsupported.code.Value());
        REQUIRE(fixture.driver->opened);
        REQUIRE(fixture.backend->Begin(Backend::Start{{}, {&fixture.core, Core::Process}}, Deadline).HasError());
        REQUIRE(fixture.backend->Begin(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}}, Deadline).HasError());
        REQUIRE_FALSE(fixture.driver->running);
        REQUIRE(fixture.core.calls == 0);
        fixture.driver->failClose = false;
        REQUIRE(std::holds_alternative<Closed>(fixture.Complete(Close{}).outcome));
    }

    TEST_CASE("Native acquisition allocation failure requires cleanup before another open", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        fixture.driver->exhaustOpen = true;
        const auto rejected = fixture.Complete(fixture.open);
        REQUIRE(std::get<Failed>(rejected.outcome).error.code.Value() == AudioErrors::MemoryAllocationFailed.code.Value());
        REQUIRE(std::get<Failed>(rejected.outcome).resources == ResourceDisposition::Retained);
        REQUIRE(fixture.driver->opened);
        REQUIRE(fixture.backend->Begin(fixture.open, Deadline).HasError());
        REQUIRE(fixture.backend->Begin(Backend::Start{{}, {&fixture.core, Core::Process}}, Deadline).HasError());
        REQUIRE(std::holds_alternative<Closed>(fixture.Complete(Close{}).outcome));
        REQUIRE_FALSE(fixture.driver->opened);
        fixture.driver->exhaustOpen = false;
        REQUIRE(std::holds_alternative<Opened>(fixture.Complete(fixture.open).outcome));
        fixture.Complete(Close{});
    }

    TEST_CASE("Native loss detaches before final core quiescence and stop failure retains native readers", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        std::array<float, 64> output{};
        fixture.driver->Emit(output);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        fixture.driver->lost = true;
        std::array<Event, 8> events;
        REQUIRE(fixture.backend->DrainEvents(events) > 0);
        fixture.driver->failStop = true;
        const auto failed = fixture.Complete(Quiesce{fixture.open.plannedEpoch});
        REQUIRE(std::get<Failed>(failed.outcome).resources == ResourceDisposition::Retained);
        REQUIRE(fixture.driver->running);
        REQUIRE(fixture.core.quiesced == 0);
        fixture.driver->failStop = false;
        fixture.Complete(Quiesce{fixture.open.plannedEpoch});
        REQUIRE_FALSE(fixture.driver->running);
        REQUIRE(fixture.core.quiesced == 1);
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Native callback nonfinite faults yield complete silence and cannot activate", "[audio][native-contract]") {
        Fixture fixture;
        fixture.core.nonfinite = true;
        fixture.Start();
        std::array<float, 64> output{};
        fixture.driver->Emit(output);
        REQUIRE(std::all_of(output.begin(), output.end(), [](const float sample) {
            return sample == 0.0F;
        }));
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasError());
        fixture.core.nonfinite = false;
        fixture.Quiesce();
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Native loss retirement remains pending until detach proof before final core quiescence", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        std::array<float, 64> samples{};
        fixture.driver->Emit(samples);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        fixture.driver->lost = true;
        fixture.driver->pendingStops = 1;
        std::array<Event, 8> events;
        (void)fixture.backend->DrainEvents(events);
        const auto operation = fixture.backend->Begin(Quiesce{fixture.open.plannedEpoch}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE_FALSE(fixture.backend->Poll(operation).Value().has_value());
        REQUIRE(fixture.driver->running);
        REQUIRE(fixture.core.quiesced == 0);
        REQUIRE(fixture.backend->Begin(Enumerate{}, Deadline).HasError());
        REQUIRE(fixture.backend->Cancel(operation).HasError());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(std::holds_alternative<Quiesced>(fixture.backend->Poll(operation).Value()->outcome));
        REQUIRE(fixture.core.quiesced == 1);
        REQUIRE_FALSE(fixture.driver->running);
        REQUIRE(fixture.backend->AcknowledgeCompletion(operation).HasValue());
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Native stop and close publish no terminal or resource release while retirement is pending", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        fixture.Quiesce();
        fixture.driver->pendingStops = 1;
        const auto stop = fixture.backend->Begin(Stop{fixture.open.plannedEpoch}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE_FALSE(fixture.backend->Poll(stop).Value().has_value());
        REQUIRE(fixture.driver->running);
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(std::holds_alternative<Stopped>(fixture.backend->Poll(stop).Value()->outcome));
        REQUIRE_FALSE(fixture.driver->running);
        REQUIRE(fixture.backend->AcknowledgeCompletion(stop).HasValue());
        fixture.driver->pendingCloses = 1;
        const auto close = fixture.backend->Begin(Close{}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE_FALSE(fixture.backend->Poll(close).Value().has_value());
        REQUIRE(fixture.driver->opened);
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(std::holds_alternative<Closed>(fixture.backend->Poll(close).Value()->outcome));
        REQUIRE_FALSE(fixture.driver->opened);
        REQUIRE(fixture.backend->AcknowledgeCompletion(close).HasValue());
    }

    TEST_CASE("Rejected native candidate retains an unfinished cleanup worker and forbids reopening", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        fixture.driver->badFormat = true;
        fixture.driver->pendingCloses = 1;
        const auto rejected = fixture.Complete(fixture.open);
        REQUIRE(std::get<Failed>(rejected.outcome).resources == ResourceDisposition::Retained);
        REQUIRE(fixture.driver->opened);
        REQUIRE(fixture.backend->Begin(fixture.open, Deadline).HasError());
        REQUIRE(fixture.backend->Begin(Backend::Start{{}, {&fixture.core, Core::Process}}, Deadline).HasError());
        fixture.Complete(Close{});
        REQUIRE_FALSE(fixture.driver->opened);
    }

    TEST_CASE("Failed native close cannot reactivate its partially retired candidate", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        REQUIRE(std::holds_alternative<Opened>(fixture.Complete(fixture.open).outcome));
        fixture.driver->failClose = true;
        const auto failed = fixture.Complete(Close{});
        REQUIRE(std::get<Failed>(failed.outcome).resources == ResourceDisposition::Retained);
        REQUIRE(fixture.driver->opened);
        REQUIRE(fixture.backend->Begin(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}}, Deadline).HasError());
        REQUIRE(fixture.backend->Begin(fixture.open, Deadline).HasError());
        fixture.driver->failClose = false;
        fixture.Complete(Close{});
    }

    TEST_CASE("A later core fault clears the complete native packet including its earlier rendered prefix", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        std::array<float, 64> output{};
        fixture.driver->Emit(output);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        fixture.core.failOnCall = fixture.core.calls + 2;
        fixture.driver->Emit(output);
        REQUIRE(std::all_of(output.begin(), output.end(), [](const float sample) {
            return sample == 0.0F;
        }));
        fixture.core.failOnCall = 0;
        fixture.Quiesce();
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Native negotiation preserves speaker semantics across native order permutations", "[audio][native-contract]") {
        Fixture fixture;
        auto native = fixture.open.format.preferred;
        std::swap(native.layout.orderedChannels[0], native.layout.orderedChannels[1]);
        const auto format = NegotiateNativeFloatOutput(fixture.open, native, 1, 16);
        REQUIRE(format.HasValue());
        REQUIRE(format.Value().effective == fixture.open.format.preferred);
        REQUIRE(format.Value().nativeChannelForHoro == std::vector<std::uint8_t>{1, 0});
        REQUIRE(format.Value().deviations.sampleRate == AudioFormatDeviationReason::None);
        REQUIRE(format.Value().deviations.layout == AudioFormatDeviationReason::None);
        native.sampleRate = 44'100;
        REQUIRE(NegotiateNativeFloatOutput(fixture.open, native, 1, 16).HasError());
        fixture.open.format.allowedAlternatives.push_back({44'100, fixture.open.format.preferred.layout});
        const auto admitted = NegotiateNativeFloatOutput(fixture.open, native, 1, 32);
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().effective.sampleRate == 44'100);
        REQUIRE(admitted.Value().deviations.sampleRate == AudioFormatDeviationReason::DeviceConstraint);
        REQUIRE(admitted.Value().deviations.layout == AudioFormatDeviationReason::None);
        REQUIRE(admitted.Value().deviations.callbackPeriod == AudioFormatDeviationReason::DeviceConstraint);
        REQUIRE(fixture.open.format.preferred.sampleRate == 48'000);
    }

    TEST_CASE("Active discovery emits exact default changes and failure preserves native output ownership", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        fixture.driver->multimediaAvailable = false;
        ++fixture.driver->catalogRevision;
        fixture.Complete(Enumerate{});
        std::array<Event, 8> events;
        const auto count = fixture.backend->DrainEvents(events);
        bool observed{};
        for (std::size_t index = 0; index < count; ++index)
            if (const auto *changed = std::get_if<DefaultChanged>(&events[index].fact))
                observed = changed->role == AudioDefaultDeviceRole::Multimedia && !changed->device && changed->discoveryRevision == 2;
        REQUIRE(observed);
        fixture.driver->failEnumeration = true;
        const auto failure = fixture.Complete(Enumerate{});
        REQUIRE(std::get<Failed>(failure.outcome).resources == ResourceDisposition::Unchanged);
        REQUIRE(fixture.driver->running);
        fixture.driver->failEnumeration = false;
        --fixture.driver->catalogRevision;
        const auto stale = fixture.Complete(Enumerate{});
        REQUIRE(std::get<Failed>(stale.outcome).resources == ResourceDisposition::Unchanged);
        REQUIRE(fixture.driver->running);
        fixture.Quiesce();
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Native migration preserves an actual allowed alternative without rewriting the preferred tuple",
              "[audio][native-contract][switch]") {
        const bool changesActualFormat = GENERATE(false, true);
        for (const auto peer : {AudioBackendKind::WASAPI, AudioBackendKind::CoreAudio, AudioBackendKind::PipeWire}) {
            Fixture fixture(peer);
            auto alternative = fixture.open.format.preferred;
            alternative.sampleRate = 44'100;
            fixture.open.format.allowedAlternatives.push_back(alternative);
            fixture.driver->nativeFormat = alternative;
            fixture.Complete(Enumerate{});
            const auto opened = fixture.Complete(fixture.open);
            REQUIRE(std::holds_alternative<Opened>(opened.outcome));
            const auto previousFormat = std::get<Opened>(opened.outcome).format.effective;
            REQUIRE(previousFormat == alternative);
            fixture.Complete(Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}});
            std::array<float, 64> samples{};
            fixture.driver->Emit(samples);
            REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
            std::array<Event, 16> events;
            (void)fixture.backend->DrainEvents(events);
            AudioDeviceSwitch transaction(*fixture.backend);
            const DeviceSwitchRequest request{.previous = fixture.open.plannedEpoch,
                                              .previousFormat = previousFormat,
                                              .replacement = {.format = fixture.open.format, .formatRevision = 2, .callbackEpoch = 2},
                                              .deadline = Deadline};
            const auto now = [] {
                return AudioMonotonicTimestamp{7, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                                                 std::chrono::steady_clock::now().time_since_epoch())
                                                                                 .count())};
            };
            const auto operation = transaction.Begin(request, now());
            REQUIRE(operation.HasValue());
            for (std::size_t step = 0; step < 64 && transaction.Completion() == nullptr; ++step) {
                const auto pumped = transaction.Pump(now());
                if (pumped.HasError()) {
                    REQUIRE(changesActualFormat);
                    REQUIRE(transaction.Completion() != nullptr);
                    break;
                }
                if (transaction.Completion())
                    break;
                REQUIRE(fixture.backend->AdvanceControl().HasValue());
                fixture.driver->Emit(samples);
                const auto count = fixture.backend->DrainEvents(events);
                for (std::size_t index = 0; index < count; ++index)
                    if (const auto *callback = std::get_if<AudioCallbackEvent>(&events[index].fact))
                        (void)transaction.Observe(*callback);
                if (transaction.Stage() == DeviceSwitchStage::AwaitDiscovery) {
                    if (changesActualFormat)
                        fixture.driver->nativeFormat = fixture.open.format.preferred;
                    const auto discovery = fixture.backend->Begin(Enumerate{}, Deadline);
                    REQUIRE(discovery.HasValue());
                    REQUIRE(fixture.backend->AdvanceControl().HasValue());
                    REQUIRE(transaction.SupplyDiscovery(operation.Value(), discovery.Value(), now()).HasValue());
                }
                if (transaction.Stage() == DeviceSwitchStage::AwaitPlayback) {
                    REQUIRE(transaction.Candidate() != nullptr);
                    REQUIRE(transaction.Candidate()->format.effective == previousFormat);
                    REQUIRE(transaction.Candidate()->format.deviations.sampleRate == AudioFormatDeviationReason::DeviceConstraint);
                    REQUIRE(transaction.SupplyPlayback(operation.Value(), SwitchPlaybackPolicy::Preserve, {&fixture.core, Core::Process})
                                .HasValue());
                }
            }
            REQUIRE(transaction.Completion() != nullptr);
            if (changesActualFormat) {
                REQUIRE_FALSE(transaction.Completion()->activated);
                REQUIRE_FALSE(transaction.Completion()->activationAnchor.has_value());
                REQUIRE(transaction.Completion()->previousDetached);
                REQUIRE(transaction.Completion()->resources == ResourceDisposition::Retained);
                REQUIRE(transaction.Acknowledge(operation.Value()).HasError());
                REQUIRE(transaction.Completion()->error.has_value());
                REQUIRE(transaction.Completion()->error->code.Value() == AudioErrors::DeviceFormatUnsupported.code.Value());
                REQUIRE_FALSE(fixture.driver->running);
                REQUIRE(fixture.driver->opened);
                REQUIRE(fixture.driver->starts == 1);
                fixture.Complete(Close{});  // Real host cleanup of the opened, never-started candidate.
                continue;
            }
            REQUIRE(transaction.Completion()->activated);
            REQUIRE(transaction.Completion()->activationAnchor.has_value());
            REQUIRE(fixture.driver->starts == 2);
            REQUIRE(request.replacement.format.preferred.sampleRate == 48'000);
            REQUIRE(request.replacement.format.allowedAlternatives.front() == alternative);
            const auto replacement = *transaction.Completion()->replacement;
            REQUIRE(transaction.Acknowledge(operation.Value()).HasValue());
            const auto quiesce = fixture.backend->Begin(Quiesce{replacement}, Deadline);
            REQUIRE(quiesce.HasValue());
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            fixture.driver->Emit(samples);
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            const auto quiesced = fixture.backend->Poll(quiesce.Value());
            REQUIRE(quiesced.HasValue());
            REQUIRE(quiesced.Value().has_value());
            REQUIRE(std::holds_alternative<Quiesced>(quiesced.Value()->outcome));
            REQUIRE(fixture.backend->AcknowledgeCompletion(quiesce.Value()).HasValue());
            fixture.Complete(Stop{replacement});
            fixture.Complete(Close{});
        }
    }

    TEST_CASE("Native recovery resolves a changed default only after old connection closure", "[audio][native-contract][switch]") {
        Fixture fixture(AudioBackendKind::PipeWire);
        fixture.Start();
        std::array<float, 64> samples{};
        fixture.driver->Emit(samples);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        fixture.driver->lost = true;
        std::array<Event, 8> events;
        (void)fixture.backend->DrainEvents(events);
        AudioDeviceSwitch transaction(*fixture.backend);
        const DeviceSwitchRequest request{.previous = fixture.open.plannedEpoch,
                                          .previousFormat = fixture.open.format.preferred,
                                          .replacement = {.format = fixture.open.format, .formatRevision = 2, .callbackEpoch = 2},
                                          .deadline = Deadline};
        constexpr AudioMonotonicTimestamp now{7, 0};
        const auto operation = transaction.Begin(request, now).Value();
        const auto drive = [&](const DeviceSwitchStage target) {
            for (int step = 0; step < 24 && transaction.Stage() != target; ++step) {
                REQUIRE(transaction.Pump(now).HasValue());
                if (transaction.Stage() == target)
                    break;
                REQUIRE(fixture.backend->AdvanceControl().HasValue());
                fixture.driver->Emit(samples);
            }
            REQUIRE(transaction.Stage() == target);
        };
        drive(DeviceSwitchStage::AwaitDiscovery);
        REQUIRE_FALSE(fixture.driver->opened);
        REQUIRE_FALSE(fixture.driver->running);
        REQUIRE(transaction.Candidate() == nullptr);
        (void)fixture.backend->DrainEvents(events);
        // The new service's device identity was unavailable while the old connection was retained.
        fixture.driver->catalogDevice = {Owner, 2, 1};
        fixture.driver->catalogRevision = 2;
        const auto discovery = fixture.backend->Begin(Enumerate{}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(transaction.SupplyDiscovery(operation, discovery, now).HasValue());
        drive(DeviceSwitchStage::AwaitPlayback);
        const AudioDeviceEpoch replacement{{Owner, 2, 1}, 2, 2};
        REQUIRE(transaction.Candidate()->timing.epoch == replacement);
        REQUIRE(transaction.Candidate()->format.discoveryRevision == 2);
        REQUIRE(transaction.SupplyPlayback(operation, SwitchPlaybackPolicy::Preserve, {&fixture.core, Core::Process}).HasValue());
        REQUIRE(transaction.Pump(now).HasValue());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        fixture.driver->Emit(samples);
        const auto count = fixture.backend->DrainEvents(events);
        for (std::size_t index = 0; index < count; ++index)
            if (const auto *callback = std::get_if<AudioCallbackEvent>(&events[index].fact))
                (void)transaction.Observe(*callback);
        REQUIRE(transaction.Pump(now).HasValue());
        REQUIRE(transaction.Completion() != nullptr);
        REQUIRE(transaction.Completion()->activated);
        REQUIRE(transaction.Completion()->previousDetached);
        REQUIRE(transaction.Completion()->replacement == replacement);
        REQUIRE(transaction.Acknowledge(operation).HasValue());
        fixture.open.plannedEpoch = replacement;
        fixture.Quiesce();
        fixture.Complete(Stop{replacement});
        fixture.Complete(Close{});
        (void)fixture.backend->DrainEvents(events);
    }

    TEST_CASE("Native loss latch publishes one cause without allocation and retained facts outlive its reset", "[audio][native-contract]") {
        NativeAudioLossLatch source;
        REQUIRE_FALSE(source.Read().has_value());
        std::optional<DeviceLossCause> observed;
        Tests::AllocationProbe::Measurement measured;
        {
            Tests::AllocationProbe::ScopedMeasurement probe;
            source.Capture(DeviceLossCause::FormatChanged);
            source.Capture(DeviceLossCause::Disconnected);
            observed = source.Read();
            measured = probe.Snapshot();
        }
        REQUIRE(measured.requests == 0);
        REQUIRE(observed == DeviceLossCause::FormatChanged);
        NativeAudioFacts facts(Owner);
        REQUIRE(facts.CaptureLoss({{Owner, 1, 1}, *observed}));
        source.Reset();  // This fixture has no concurrent native producer; control has already copied its required fact.
        REQUIRE_FALSE(source.Read().has_value());
        REQUIRE(facts.HasLoss());
        std::array<Event, 1> output;
        REQUIRE(facts.Drain(output) == 1);
        REQUIRE(std::get<DeviceLost>(output[0].fact).cause == DeviceLossCause::FormatChanged);
        source.Capture(static_cast<DeviceLossCause>(255));
        REQUIRE(source.Read() == DeviceLossCause::BackendFailure);
    }

    TEST_CASE("Every native loss cause including the zero enum survives atomic publication", "[audio][native-contract]") {
        constexpr std::array causes{DeviceLossCause::Disconnected, DeviceLossCause::ServiceRestart, DeviceLossCause::FormatChanged,
                                    DeviceLossCause::PermissionRevoked, DeviceLossCause::BackendFailure};
        NativeAudioLossLatch source;
        for (const auto cause : causes) {
            source.Capture(cause);
            source.Capture(DeviceLossCause::BackendFailure);
            REQUIRE(source.Read() == cause);
            source.Reset();
            REQUIRE_FALSE(source.Read().has_value());
        }
    }

    TEST_CASE("Native fact mailbox retains loss before coalesced catalog changes through tiny destinations", "[audio][native-contract]") {
        NativeAudioFacts facts(Owner);
        const auto foreign = AudioRuntimeId::Create(18).Value();
        const AudioDeviceId first{Owner, 1, 1};
        const AudioDeviceId second{Owner, 2, 1};
        REQUIRE_FALSE(facts.CaptureLoss({{foreign, 1, 1}, DeviceLossCause::Disconnected}));
        REQUIRE(facts.CaptureLoss({{Owner, 1, 1}, DeviceLossCause::Disconnected}));
        REQUIRE(facts.CaptureLoss({{Owner, 1, 1}, DeviceLossCause::FormatChanged}));
        REQUIRE_FALSE(facts.CaptureLoss({{Owner, 2, 1}, DeviceLossCause::ServiceRestart}));
        facts.CaptureChange(3);
        facts.CaptureChange(7);
        facts.CaptureChange(5);
        REQUIRE(facts.Drain({}) == 0);
        REQUIRE(facts.HasLoss());
        std::array<Event, 1> output;
        REQUIRE(facts.Drain(output) == 1);
        REQUIRE(output[0].owner == Owner);
        REQUIRE(std::get<DeviceLost>(output[0].fact).device == first);
        REQUIRE(std::get<DeviceLost>(output[0].fact).cause == DeviceLossCause::Disconnected);
        REQUIRE_FALSE(facts.HasLoss());
        REQUIRE(facts.Drain(output) == 1);
        REQUIRE(std::get<DevicesChanged>(output[0].fact).discoveryRevision == 7);
        REQUIRE(facts.Drain(output) == 0);
        REQUIRE(facts.CaptureLoss({{Owner, 2, 1}, DeviceLossCause::ServiceRestart}));
        REQUIRE(facts.Drain(output) == 1);
        REQUIRE(std::get<DeviceLost>(output[0].fact).device == second);
    }

    TEST_CASE("Undrained native loss survives closure and blocks epoch reuse while earlier core facts drain", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Start();
        std::array<float, 64> samples{};
        fixture.driver->Emit(samples);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        fixture.driver->lost = true;
        // Shutdown must obtain native proof even before the host has observed the loss fact.
        REQUIRE(std::holds_alternative<Quiesced>(fixture.Complete(Quiesce{fixture.open.plannedEpoch}).outcome));
        REQUIRE(fixture.core.quiesced == 1);
        REQUIRE_FALSE(fixture.driver->running);
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
        auto replacement = fixture.open;
        ++replacement.plannedEpoch.callbackEpoch;
        ++replacement.plannedEpoch.formatRevision;
        REQUIRE(fixture.backend->Begin(replacement, Deadline).HasError());
        std::array<Event, 1> output;
        for (int index = 0; index < 2; ++index) {
            REQUIRE(fixture.backend->DrainEvents(output) == 1);
            REQUIRE(std::holds_alternative<AudioCallbackEvent>(output[0].fact));
            REQUIRE(fixture.backend->Begin(replacement, Deadline).HasError());
        }
        REQUIRE(fixture.backend->DrainEvents(output) == 1);
        REQUIRE(std::get<DeviceLost>(output[0].fact).device == fixture.open.plannedEpoch.device);
        REQUIRE(std::holds_alternative<Opened>(fixture.Complete(replacement).outcome));
        fixture.Complete(Close{});
    }

    TEST_CASE("Native loss between start admission and execution cannot attach the prepared render port", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        fixture.Complete(fixture.open);
        const auto start =
            fixture.backend->Begin(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}}, Deadline).Value();
        fixture.driver->lost = true;
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        const auto result = fixture.backend->Poll(start);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().has_value());
        REQUIRE(std::get<Failed>(result.Value()->outcome).error.code.Value() == AudioErrors::DeviceLost.code.Value());
        REQUIRE_FALSE(fixture.driver->running);
        REQUIRE(fixture.core.calls == 0);
        REQUIRE(fixture.backend->AcknowledgeCompletion(start).HasValue());
        fixture.Complete(Close{});
        std::array<Event, 8> events;
        (void)fixture.backend->DrainEvents(events);
    }

    TEST_CASE("Unobserved native loss forbids start and readiness activation", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        fixture.Complete(fixture.open);
        fixture.driver->lost = true;
        REQUIRE(fixture.backend->Begin(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}}, Deadline).HasError());
        std::array<Event, 8> events;
        REQUIRE(fixture.backend->DrainEvents(events) == 1);
        fixture.Complete(Close{});
        ++fixture.open.plannedEpoch.callbackEpoch;
        ++fixture.open.plannedEpoch.formatRevision;
        fixture.Complete(fixture.open);
        fixture.Complete(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}});
        std::array<float, 64> samples{};
        fixture.driver->Emit(samples);
        fixture.driver->lost = true;
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasError());
        fixture.Complete(Quiesce{fixture.open.plannedEpoch});
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
        (void)fixture.backend->DrainEvents(events);
    }

    TEST_CASE("Migration cancellation preserves a failed native stop and its still-running reader", "[audio][native-contract][cancel]") {
        Fixture fixture;
        fixture.Start();
        std::array<float, 64> samples{};
        fixture.driver->Emit(samples);
        REQUIRE(fixture.backend->CommitRendering(fixture.open.plannedEpoch).HasValue());
        AudioDeviceSwitch transaction(*fixture.backend);
        const DeviceSwitchRequest request{.previous = fixture.open.plannedEpoch,
                                          .previousFormat = fixture.open.format.preferred,
                                          .replacement = {.format = fixture.open.format, .formatRevision = 2, .callbackEpoch = 2},
                                          .deadline = Deadline};
        constexpr AudioMonotonicTimestamp now{7, 0};
        const auto operation = transaction.Begin(request, now).Value();
        REQUIRE(transaction.Cancel(operation).HasValue());
        fixture.driver->failStop = true;
        for (std::size_t step = 0; step < 16 && transaction.Stage() != DeviceSwitchStage::Terminal; ++step) {
            const auto pumped = transaction.Pump(now);
            if (transaction.Stage() == DeviceSwitchStage::Terminal)
                break;
            REQUIRE(pumped.HasValue());
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            fixture.driver->Emit(samples);
        }
        REQUIRE(transaction.Completion());
        REQUIRE(transaction.Completion()->error);
        CHECK(transaction.Completion()->resources == ResourceDisposition::Retained);
        CHECK_FALSE(transaction.Completion()->previousDetached);
        CHECK_FALSE(transaction.Completion()->activated);
        CHECK(transaction.Completion()->error->code.Value() == AudioErrors::BackendFailed.code.Value());
        CHECK(fixture.driver->running);
        CHECK(fixture.driver->closes == 0);
        CHECK(transaction.Acknowledge(operation).HasError());
        fixture.driver->failStop = false;
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Native control expiration preserves actual callback ownership", "[audio][native-contract]") {
        Fixture fixture;
        fixture.Complete(Enumerate{});
        fixture.Complete(fixture.open);
        bool attached{};
        Request request = Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}};
        SECTION("Open alone does not acquire a reader when Start expires") {}
        SECTION("An expired Close preserves an already completed native detach") {
            fixture.Complete(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}});
            fixture.Quiesce();
            fixture.Complete(Stop{fixture.open.plannedEpoch});
            request = Close{};
        }
        SECTION("Expiration cannot detach a running reader") {
            fixture.Complete(Backend::Start{fixture.open.plannedEpoch, {&fixture.core, Core::Process}});
            attached = true;
            request = Quiesce{fixture.open.plannedEpoch};
        }
        // Exercise the production steady-clock boundary; no fixture timestamp authorizes retirement.
        const auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        const AudioMonotonicTimestamp
            deadline{7,
                     static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(expiry.time_since_epoch()).count())};
        const auto admitted = fixture.backend->Begin(request, deadline);
        REQUIRE(admitted.HasValue());
        const auto stops = fixture.driver->stops;
        const auto closes = fixture.driver->closes;
        std::this_thread::sleep_until(expiry);
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        const auto terminal = fixture.backend->Poll(admitted.Value());
        REQUIRE(terminal.HasValue());
        REQUIRE(terminal.Value().has_value());
        const auto *failed = std::get_if<Failed>(&terminal.Value()->outcome);
        REQUIRE(failed != nullptr);
        REQUIRE(failed->resources == (attached ? ResourceDisposition::Retained : ResourceDisposition::CallbackDetached));
        REQUIRE(fixture.driver->running == attached);
        REQUIRE(fixture.driver->stops == stops);
        REQUIRE(fixture.driver->closes == closes);
        REQUIRE(fixture.backend->AcknowledgeCompletion(admitted.Value()).HasValue());
        if (attached) {
            fixture.Quiesce();
            fixture.Complete(Stop{fixture.open.plannedEpoch});
        }
        REQUIRE(std::holds_alternative<Closed>(fixture.Complete(Close{}).outcome));
    }

    TEST_CASE("Audio processing restores the calling thread floating point environment", "[audio][native-contract]") {
        const auto original = std::fegetround();
        std::fesetround(FE_DOWNWARD);
        int processing{};
        {
            const AudioRenderEnvironment environment;
            processing = std::fegetround();
        }
        const auto restored = std::fegetround();
        std::fesetround(original);
        REQUIRE(processing == FE_TONEAREST);
        REQUIRE(restored == FE_DOWNWARD);
    }
}  // namespace
