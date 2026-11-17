#include "Horo/Audio/Internal/AudioDeviceSwitch.h"
#include "Horo/Audio/Internal/NullAudioBackend.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

using namespace Horo::Audio;
using namespace Horo::Audio::Backend;

namespace {
    const auto Owner = AudioRuntimeId::Create(71).Value();
    constexpr AudioMonotonicTimestamp Now{19, 0};
    constexpr AudioMonotonicTimestamp Deadline{19, 1'000'000'000};

    RenderResult Render(void *, const RenderInvocation &invocation) noexcept {
        const auto disposition = invocation.phase == RenderPhase::Priming     ? RenderDisposition::Ready
                                 : invocation.phase == RenderPhase::Quiescing ? RenderDisposition::Quiesced
                                                                              : RenderDisposition::Rendered;
        return {disposition, AudioCallbackFaultCode::None};
    }

    struct CancelRender final {
        std::size_t rendering{};

        static RenderResult Process(void *context, const RenderInvocation &invocation) noexcept {
            auto &self = *static_cast<CancelRender *>(context);
            if (invocation.phase == RenderPhase::Rendering)
                ++self.rendering;
            for (auto *plane : invocation.output.planes)
                std::fill_n(plane, invocation.output.validFrames, 0.0F);
            return Render(nullptr, invocation);
        }
    };

    struct Fixture final {
        std::unique_ptr<NullAudioBackend> backend = std::move(CreateNullAudioBackend({Owner, 19})).Value();
        Open open{.plannedEpoch = {{Owner, 1, 1}, 7, 11},
                  .format = {.device = AudioDeviceId{Owner, 1, 1},
                             .preferred = {48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)},
                             .period = {64, 128, 256}}};
        OperationId initialDiscovery;

        Backend::Completion Complete(const Request &request) {
            const auto id = backend->Begin(request, Deadline).Value();
            REQUIRE(backend->AdvanceControl().HasValue());
            const auto result = backend->Poll(id);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().has_value());
            auto completion = *result.Value();
            REQUIRE(backend->AcknowledgeCompletion(id).HasValue());
            return completion;
        }

        Fixture() {
            initialDiscovery = Complete(Enumerate{}).operation;
            Complete(open);
            Complete(Start{open.plannedEpoch, {nullptr, Render}});
            REQUIRE(backend->AdvanceCallback().HasValue());
            REQUIRE(backend->CommitRendering(open.plannedEpoch).HasValue());
            std::array<Event, 64> events;
            (void)backend->DrainEvents(events);
        }

        DeviceSwitchRequest Switch() const {
            return {.previous = open.plannedEpoch,
                    .previousFormat = open.format.preferred,
                    .replacement = {.format = open.format,
                                    .formatRevision = open.plannedEpoch.formatRevision + 1,
                                    .callbackEpoch = open.plannedEpoch.callbackEpoch + 1,
                                    .access = open.access,
                                    .latency = open.latency},
                    .deadline = Deadline};
        }

        void DriveToDiscovery(AudioDeviceSwitch &transaction) {
            for (int iteration = 0; iteration < 16 && transaction.Stage() != DeviceSwitchStage::AwaitDiscovery; ++iteration) {
                REQUIRE(transaction.Pump(Now).HasValue());
                if (transaction.Stage() == DeviceSwitchStage::AwaitDiscovery)
                    break;
                REQUIRE(backend->AdvanceControl().HasValue());
                if (backend->State() == NullAudioBackendState::Quiescing) {
                    REQUIRE(backend->AdvanceCallback().HasValue());
                    REQUIRE(backend->AdvanceControl().HasValue());
                }
            }
            REQUIRE(transaction.Stage() == DeviceSwitchStage::AwaitDiscovery);
            REQUIRE(backend->State() == NullAudioBackendState::Closed);
            std::array<Event, 64> retiredEvents;
            (void)backend->DrainEvents(retiredEvents);  // The host accepts old facts before another epoch reuses callback storage.
        }

        void OpenReplacement(AudioDeviceSwitch &transaction) {
            for (int iteration = 0; iteration < 4 && transaction.Stage() != DeviceSwitchStage::AwaitPlayback; ++iteration) {
                REQUIRE(transaction.Pump(Now).HasValue());
                if (transaction.Stage() != DeviceSwitchStage::AwaitPlayback)
                    REQUIRE(backend->AdvanceControl().HasValue());
            }
            REQUIRE(transaction.Stage() == DeviceSwitchStage::AwaitPlayback);
        }

        void DriveToPlayback(AudioDeviceSwitch &transaction, const OperationId operation) {
            DriveToDiscovery(transaction);
            const auto discovery = backend->Begin(Enumerate{}, Deadline).Value();
            REQUIRE(backend->AdvanceControl().HasValue());
            REQUIRE(transaction.SupplyDiscovery(operation, discovery, Now).HasValue());
            REQUIRE(backend->Poll(discovery).HasError());
            OpenReplacement(transaction);
        }

        void RetireCancelled(AudioDeviceSwitch &transaction, const OperationId operation) {
            for (std::size_t step = 0; step < 32 && transaction.Stage() != DeviceSwitchStage::Terminal; ++step) {
                const auto pumped = transaction.Pump(Now);
                if (transaction.Stage() == DeviceSwitchStage::Terminal)
                    break;
                REQUIRE(pumped.HasValue());
                REQUIRE(backend->AdvanceControl().HasValue());
                const auto state = backend->State();
                if (state == NullAudioBackendState::Priming || state == NullAudioBackendState::Quiescing)
                    REQUIRE(backend->AdvanceCallback().HasValue());
                if (state == NullAudioBackendState::Quiescing)
                    REQUIRE(backend->AdvanceControl().HasValue());
            }
            REQUIRE(transaction.Stage() == DeviceSwitchStage::Terminal);
            const auto *completed = transaction.Completion();
            REQUIRE(completed != nullptr);
            CHECK(completed->operation == operation);
            CHECK(completed->previousDetached);
            CHECK_FALSE(completed->activated);
            CHECK_FALSE(completed->activationAnchor.has_value());
            REQUIRE(completed->error);
            CHECK(completed->error->code.Value() == AudioErrors::OperationCancelled.code.Value());
            CHECK(completed->resources == ResourceDisposition::Closed);
            CHECK(backend->State() == NullAudioBackendState::Closed);
            CHECK(transaction.Cancel(operation).Value() == CancelDisposition::AlreadyTerminal);
            CHECK(transaction.Pump(Now).HasError());
            CHECK(transaction.Completion() == completed);
            REQUIRE(transaction.Acknowledge(operation).HasValue());
        }
    };

    TEST_CASE("Migration cancellation retires the old output before discovery instead of reactivating it", "[audio][switch][cancel]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto operation = transaction.Begin(fixture.Switch(), Now).Value();
        auto foreign = operation;
        foreign.owner = AudioRuntimeId::Create(999).Value();
        CHECK(transaction.Cancel(foreign).HasError());
        CHECK(transaction.Stage() == DeviceSwitchStage::Quiescing);
        REQUIRE(transaction.Cancel(operation).Value() == CancelDisposition::Requested);
        REQUIRE(transaction.Cancel(operation).Value() == CancelDisposition::Requested);
        fixture.RetireCancelled(transaction, operation);
        CHECK(transaction.Candidate() == nullptr);
    }

    TEST_CASE("Migration cancellation closes an acquired candidate or retires a priming replacement without activation",
              "[audio][switch][cancel]") {
        Fixture fixture;
        CancelRender candidate;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto operation = transaction.Begin(fixture.Switch(), Now).Value();
        fixture.DriveToPlayback(transaction, operation);
        bool supply{};
        bool submitted{};
        bool started{};
        bool ready{};
        SECTION("Candidate awaiting playback") {}
        SECTION("Playback prepared but Start not submitted") {
            supply = true;
        }
        SECTION("Start admitted but still pending") {
            supply = true;
            submitted = true;
        }
        SECTION("Started without a Ready callback") {
            supply = true;
            submitted = true;
            started = true;
        }
        SECTION("Ready observed but not committed") {
            supply = true;
            submitted = true;
            started = true;
            ready = true;
        }
        if (supply)
            REQUIRE(transaction.SupplyPlayback(operation, SwitchPlaybackPolicy::Preserve, {&candidate, CancelRender::Process}).HasValue());
        if (submitted)
            REQUIRE(transaction.Pump(Now).HasValue());
        if (started) {
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
            REQUIRE(transaction.Pump(Now).HasValue());
            REQUIRE(transaction.Stage() == DeviceSwitchStage::AwaitReady);
        }
        if (ready) {
            REQUIRE(fixture.backend->AdvanceCallback().HasValue());
            std::array<Event, 8> events;
            const auto count = fixture.backend->DrainEvents(events);
            for (std::size_t index = 0; index < count; ++index)
                if (const auto *callback = std::get_if<AudioCallbackEvent>(&events[index].fact))
                    (void)transaction.Observe(*callback);
        }
        REQUIRE(transaction.Cancel(operation).Value() == CancelDisposition::Requested);
        CHECK(transaction.SupplyPlayback(operation, SwitchPlaybackPolicy::Preserve, {&candidate, CancelRender::Process}).HasError());
        fixture.RetireCancelled(transaction, operation);
        CHECK(candidate.rendering == 0);
    }

    TEST_CASE("Cancelled migration still reconciles the exact discovery receipt before native Close", "[audio][switch][cancel]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto operation = transaction.Begin(fixture.Switch(), Now).Value();
        fixture.DriveToDiscovery(transaction);
        const auto discovery = fixture.backend->Begin(Enumerate{}, Deadline).Value();
        REQUIRE(transaction.Cancel(operation).Value() == CancelDisposition::Requested);
        CHECK(transaction.SupplyDiscovery(operation, discovery, Now).HasError());
        SECTION("Discovery completes") {
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
        }
        SECTION("Discovery itself is cancelled") {
            REQUIRE(fixture.backend->Cancel(discovery).HasValue());
        }
        REQUIRE(transaction.SupplyDiscovery(operation, discovery, Now).HasValue());
        CHECK(fixture.backend->Poll(discovery).HasError());
        CHECK(transaction.Candidate() == nullptr);
        CHECK(transaction.Stage() == DeviceSwitchStage::ClosingReplacement);
        fixture.RetireCancelled(transaction, operation);
    }

    TEST_CASE("Cancelled migration reconciles pending Open before closing its acquired prefix", "[audio][switch][cancel]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto operation = transaction.Begin(fixture.Switch(), Now).Value();
        fixture.DriveToDiscovery(transaction);
        const auto discovery = fixture.backend->Begin(Enumerate{}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(transaction.SupplyDiscovery(operation, discovery, Now).HasValue());
        SECTION("Open has not been admitted") {}
        SECTION("Open still pending") {
            REQUIRE(transaction.Pump(Now).HasValue());
        }
        SECTION("Open terminal retained by the backend") {
            REQUIRE(transaction.Pump(Now).HasValue());
            REQUIRE(fixture.backend->AdvanceControl().HasValue());
        }
        REQUIRE(transaction.Cancel(operation).Value() == CancelDisposition::Requested);
        fixture.RetireCancelled(transaction, operation);
    }

    TEST_CASE("Cancellation expiration cannot claim Closed before actual native retirement", "[audio][switch][cancel]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto operation = transaction.Begin(fixture.Switch(), Now).Value();
        REQUIRE(transaction.Cancel(operation).HasValue());
        REQUIRE(transaction.Pump(Deadline).HasError());
        REQUIRE(transaction.Completion());
        CHECK(transaction.Completion()->resources == ResourceDisposition::Retained);
        CHECK_FALSE(transaction.Completion()->previousDetached);
        CHECK_FALSE(transaction.Completion()->activated);
        CHECK(fixture.backend->State() == NullAudioBackendState::Rendering);
        CHECK(transaction.Acknowledge(operation).HasError());
        const auto quiesce = fixture.backend->Begin(Quiesce{fixture.open.plannedEpoch}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(fixture.backend->AdvanceCallback().HasValue());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(fixture.backend->AcknowledgeCompletion(quiesce).HasValue());
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Device migration detaches before playback preparation and activates once after ready", "[audio][switch]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto request = fixture.Switch();
        const auto id = transaction.Begin(request, Now).Value();
        REQUIRE(transaction.Begin(request, Now).HasError());
        fixture.DriveToPlayback(transaction, id);
        REQUIRE(transaction.Candidate() != nullptr);
        const auto replacement = transaction.Candidate()->timing.epoch;
        REQUIRE(replacement.device == request.previous.device);
        REQUIRE(replacement.callbackEpoch == request.replacement.callbackEpoch);
        REQUIRE(replacement.formatRevision == request.replacement.formatRevision);
        REQUIRE(fixture.backend->State() == NullAudioBackendState::Opened);
        REQUIRE(transaction.SupplyPlayback(id, SwitchPlaybackPolicy::Restart, {nullptr, Render}).HasError());
        REQUIRE(transaction.SupplyPlayback(id, SwitchPlaybackPolicy::Preserve, {nullptr, Render}).HasValue());
        REQUIRE(transaction.Pump(Now).HasValue());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(transaction.Stage() == DeviceSwitchStage::Starting);
        REQUIRE(transaction.Completion() == nullptr);
        REQUIRE(fixture.backend->AdvanceCallback().HasValue());
        std::array<Event, 64> events;
        const auto count = fixture.backend->DrainEvents(events);
        std::optional<AudioCallbackEvent> firstReady;
        for (std::size_t index = 0; index < count; ++index)
            if (const auto *callback = std::get_if<AudioCallbackEvent>(&events[index].fact);
                callback != nullptr && std::holds_alternative<AudioCallbackReady>(callback->fact)) {
                auto stale = *callback;
                stale.epoch = request.previous;
                CHECK_FALSE(transaction.Observe(stale));
                auto foreignClock = *callback;
                ++foreignClock.timestamp.clockDomain;
                CHECK_FALSE(transaction.Observe(foreignClock));
                REQUIRE(transaction.Observe(*callback));
                firstReady = *callback;
                auto duplicate = *callback;
                ++duplicate.sampleFrame;
                ++duplicate.timestamp.nanoseconds;
                CHECK_FALSE(transaction.Observe(duplicate));
            }
        REQUIRE(firstReady.has_value());
        REQUIRE(transaction.Pump(Now).HasValue());
        REQUIRE(fixture.backend->State() == NullAudioBackendState::Rendering);
        const auto *completion = transaction.Completion();
        REQUIRE(completion != nullptr);
        REQUIRE(completion->activated);
        REQUIRE(completion->previousDetached);
        REQUIRE(completion->playback == SwitchPlaybackPolicy::Preserve);
        REQUIRE(completion->replacement == replacement);
        REQUIRE(completion->activationAnchor.has_value());
        REQUIRE(completion->activationAnchor->epoch == firstReady->epoch);
        REQUIRE(completion->activationAnchor->sampleFrame == firstReady->sampleFrame);
        REQUIRE(completion->activationAnchor->timestamp.clockDomain == firstReady->timestamp.clockDomain);
        REQUIRE(completion->activationAnchor->timestamp.nanoseconds == firstReady->timestamp.nanoseconds);
        REQUIRE(completion->activationAnchor->timestamp.nanoseconds != Deadline.nanoseconds);
        REQUIRE(std::holds_alternative<AudioCallbackReady>(completion->activationAnchor->fact));
        REQUIRE(transaction.Pump(Deadline).HasValue());
        REQUIRE(transaction.Completion() == completion);
        REQUIRE(completion->activationAnchor->sampleFrame == firstReady->sampleFrame);
        REQUIRE(transaction.Acknowledge(id).HasValue());
        // Explicitly retire the replacement; transaction acknowledgement itself never stops a backend.
        const auto quiesce = fixture.backend->Begin(Quiesce{replacement}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(fixture.backend->AdvanceCallback().HasValue());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(fixture.backend->AcknowledgeCompletion(quiesce).HasValue());
        fixture.Complete(Stop{replacement});
        fixture.Complete(Close{});
    }

    TEST_CASE("Device migration rejects stale epochs and unsupported preserve-format changes before native work", "[audio][switch]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        auto request = fixture.Switch();
        request.replacement.callbackEpoch = request.previous.callbackEpoch;
        REQUIRE(transaction.Begin(request, Now).HasError());
        request = fixture.Switch();
        request.playback = static_cast<SwitchPlaybackPolicy>(255);
        REQUIRE(transaction.Begin(request, Now).HasError());
        REQUIRE(transaction.Stage() == DeviceSwitchStage::Idle);
        request = fixture.Switch();
        request.replacement.format.preferred.sampleRate = 44'100;
        REQUIRE(transaction.Begin(request, Now).HasError());
        REQUIRE(fixture.backend->State() == NullAudioBackendState::Rendering);
        auto mixedTuples = request;
        mixedTuples.replacement.format.preferred.layout = MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono);
        mixedTuples.replacement.format.allowedAlternatives = {{48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)},
                                                              {32'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Stereo)}};
        REQUIRE(ValidateAudioDeviceFormatRequest(mixedTuples.replacement.format));
        REQUIRE(transaction.Begin(mixedTuples, Now).HasError());
        REQUIRE(fixture.backend->State() == NullAudioBackendState::Rendering);
        request.playback = SwitchPlaybackPolicy::Restart;
        REQUIRE(transaction.Begin(request, Now).HasValue());
        // No operation was submitted; expire explicitly and retain rather than fabricate detachment.
        REQUIRE(transaction.Pump(Deadline).HasError());
        REQUIRE(transaction.Completion() != nullptr);
        REQUIRE_FALSE(transaction.Completion()->previousDetached);
        REQUIRE(transaction.Completion()->resources == ResourceDisposition::Retained);
        REQUIRE(transaction.Acknowledge(transaction.Completion()->operation).HasError());
        // Explicit host recovery obtains fresh proof; the timeout itself did not authorize destruction.
        const auto quiesce = fixture.backend->Begin(Quiesce{fixture.open.plannedEpoch}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(fixture.backend->AdvanceCallback().HasValue());
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(fixture.backend->AcknowledgeCompletion(quiesce).HasValue());
        fixture.Complete(Stop{fixture.open.plannedEpoch});
        fixture.Complete(Close{});
    }

    TEST_CASE("Device migration consumes only a completed post-close catalog receipt", "[audio][switch]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto id = transaction.Begin(fixture.Switch(), Now).Value();
        fixture.DriveToDiscovery(transaction);
        REQUIRE(transaction.Candidate() == nullptr);
        REQUIRE(transaction.SupplyDiscovery(id, fixture.initialDiscovery, Now).HasError());
        const auto unrelated = fixture.backend->Begin(Probe{}, Deadline).Value();
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(transaction.SupplyDiscovery(id, unrelated, Now).HasError());
        REQUIRE(fixture.backend->Poll(unrelated).Value().has_value());
        REQUIRE(fixture.backend->AcknowledgeCompletion(unrelated).HasValue());
        const auto discovery = fixture.backend->Begin(Enumerate{}, Deadline).Value();
        REQUIRE(transaction.SupplyDiscovery(id, discovery, Now).HasError());
        REQUIRE(transaction.Stage() == DeviceSwitchStage::AwaitDiscovery);
        REQUIRE(fixture.backend->AdvanceControl().HasValue());
        REQUIRE(transaction.SupplyDiscovery({Owner, id.sequence + 1}, discovery, Now).HasError());
        REQUIRE(fixture.backend->Poll(discovery).Value().has_value());
        REQUIRE(transaction.SupplyDiscovery(id, discovery, Now).HasValue());
        REQUIRE(fixture.backend->Poll(discovery).HasError());
        REQUIRE(transaction.Stage() == DeviceSwitchStage::Opening);
        REQUIRE(fixture.backend->State() == NullAudioBackendState::Closed);
        fixture.OpenReplacement(transaction);
        REQUIRE(transaction.Pump(Deadline).HasError());
        fixture.Complete(Close{});  // Host obtains explicit retirement proof after the failed transaction.
    }

    TEST_CASE("Device migration waits closed without inventing a replacement when fresh discovery never arrives", "[audio][switch]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto id = transaction.Begin(fixture.Switch(), Now).Value();
        fixture.DriveToDiscovery(transaction);
        REQUIRE(transaction.Pump(Now).HasValue());
        REQUIRE(fixture.backend->State() == NullAudioBackendState::Closed);
        REQUIRE(fixture.backend->AdvanceControl().HasError());
        REQUIRE(transaction.Candidate() == nullptr);
        REQUIRE(transaction.Completion() == nullptr);
        REQUIRE(transaction.Pump(Deadline).HasError());
        REQUIRE(transaction.Completion()->previousDetached);
        REQUIRE_FALSE(transaction.Completion()->replacement.has_value());
        REQUIRE(transaction.Completion()->resources == ResourceDisposition::Retained);
        REQUIRE(transaction.Acknowledge(id).HasError());
    }

    TEST_CASE("Cancelled post-close discovery produces one retained migration failure and acknowledges its receipt", "[audio][switch]") {
        Fixture fixture;
        AudioDeviceSwitch transaction(*fixture.backend);
        const auto id = transaction.Begin(fixture.Switch(), Now).Value();
        fixture.DriveToDiscovery(transaction);
        const auto discovery = fixture.backend->Begin(Enumerate{}, Deadline).Value();
        REQUIRE(fixture.backend->Cancel(discovery).HasValue());
        REQUIRE(transaction.SupplyDiscovery(id, discovery, Now).HasError());
        REQUIRE(fixture.backend->Poll(discovery).HasError());
        const auto *terminal = transaction.Completion();
        REQUIRE(terminal != nullptr);
        REQUIRE(terminal->previousDetached);
        REQUIRE_FALSE(terminal->replacement.has_value());
        REQUIRE(terminal->resources == ResourceDisposition::Retained);
        REQUIRE(transaction.Pump(Now).HasError());
        REQUIRE(transaction.Completion() == terminal);
    }
}  // namespace
