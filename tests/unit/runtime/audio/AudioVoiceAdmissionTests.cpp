#include "AllocationProbe.h"
#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioVoiceAdmission.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] AudioRuntimeId Runtime() {
            return AudioRuntimeId::Create(71).Value();
        }

        [[nodiscard]] AudioVoiceAdmissionCandidate Candidate(const std::uint32_t slot) {
            return {.snapshot = {{Runtime(), slot, 1}, AudioVoiceState::Playing, {}},
                    .admissionOrder = slot,
                    .audibleGain = 1.0F,
                    .priority = 100,
                    .listenerDistance = 1.0,
                    .physical = true};
        }

        [[nodiscard]] AudioVoiceAdmissionRequest Request(const std::span<const AudioVoiceAdmissionCandidate> voices,
                                                         const AudioConcurrencyMode mode = AudioConcurrencyMode::StealOldest) {
            return {.runtime = Runtime(),
                    .maximumPhysicalVoices = static_cast<std::uint32_t>(voices.size()),
                    .mode = mode,
                    .time = {1, 100},
                    .voices = voices};
        }

        [[nodiscard]] AudioVoiceAdmissionConstraint Constraint(const std::span<const AudioVoiceSnapshot> voices,
                                                               const std::uint64_t id = 9) {
            const AudioConcurrencyGroup group{.group = AudioConcurrencyGroupId::Create(id).Value(), .maximumInstances = 1};
            const AudioConcurrencyRequest request{.runtime = Runtime()};
            return {group, request, {MakeAudioConcurrencyKey(group, request).Value(), 1, {}, voices}};
        }

        void CheckDecision(const AudioVoiceAdmissionRequest &request, const AudioVoiceAdmissionAction action,
                           const AudioVoiceAdmissionReason reason, const std::optional<AudioVoiceHandle> victim = {}) {
            const auto before = Tests::AllocationProbe::Count();
            const auto result = EvaluateAudioVoiceAdmission(request);
            const auto allocations = Tests::AllocationProbe::Count() - before;
            REQUIRE(result.HasValue());
            CHECK(allocations == 0);
            CHECK(result.Value().action == action);
            CHECK(result.Value().reason == reason);
            CHECK(result.Value().victim == victim);
        }

        TEST_CASE("Voice admission selects every policy with distinct rank metrics", "[unit][audio][admission]") {
            std::array voices{Candidate(1), Candidate(2), Candidate(3), Candidate(4)};
            voices[0].admissionOrder = 30;
            voices[1].admissionOrder = 10;
            voices[2].audibleGain = 0.01F;
            voices[3].priority = 1;
            voices[0].listenerDistance = 900;
            using enum AudioConcurrencyMode;
            using enum AudioVoiceAdmissionReason;

            struct Expected {
                AudioConcurrencyMode mode;
                AudioVoiceAdmissionReason reason;
                std::size_t index;
            };

            for (const auto expected :
                 {Expected{StealOldest, StopOldest, 2}, Expected{Replace, ReplaceOldest, 2}, Expected{StealQuietest, StopQuietest, 2},
                  Expected{StealLowestPriority, StopLowestPriority, 3}, Expected{StealFurthest, StopFurthest, 0}}) {
                CheckDecision(Request(voices, expected.mode), AudioVoiceAdmissionAction::Replace, expected.reason,
                              voices[expected.index].snapshot.voice);
            }
            CheckDecision(Request(voices, RejectNew), AudioVoiceAdmissionAction::Reject, RejectNewest);
            CheckDecision(Request(voices, Allow), AudioVoiceAdmissionAction::Reject, PhysicalCapacity);
            CheckDecision(Request(voices, Virtualize), AudioVoiceAdmissionAction::AdmitVirtual, Virtualized);
        }

        TEST_CASE("Voice policy ties minimize admission order then complete handle with no allocation", "[unit][audio][admission]") {
            std::array voices{Candidate(1), Candidate(2), Candidate(3)};
            voices[0].admissionOrder = 5;
            voices[1].admissionOrder = 4;
            voices[1].snapshot.voice.generation = 7;
            voices[2].admissionOrder = 4;
            for (const auto mode :
                 {AudioConcurrencyMode::StealOldest, AudioConcurrencyMode::StealQuietest, AudioConcurrencyMode::StealLowestPriority,
                  AudioConcurrencyMode::StealFurthest, AudioConcurrencyMode::Replace}) {
                const auto request = Request(voices, mode);
                const auto before = Tests::AllocationProbe::Count();
                const auto result = EvaluateAudioVoiceAdmission(request);
                const auto allocations = Tests::AllocationProbe::Count() - before;
                REQUIRE(result.HasValue());
                CHECK(allocations == 0);
                CHECK(result.Value().victim == voices[1].snapshot.voice);
                CHECK(EvaluateAudioVoiceAdmission(request).Value() == result.Value());
                CHECK(voices[1].snapshot.state == AudioVoiceState::Playing);
            }
        }

        TEST_CASE("Voice admission respects spare capacity zero budget and existing overcommit", "[unit][audio][admission]") {
            std::array voices{Candidate(1)};
            auto request = Request(voices);
            request.maximumPhysicalVoices = 2;
            CheckDecision(request, AudioVoiceAdmissionAction::AdmitPhysical, AudioVoiceAdmissionReason::CapacityAvailable);
            request.maximumPhysicalVoices = 0;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::OverCapacity);
            request.voices = {};
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::PhysicalCapacity);
            request.mode = AudioConcurrencyMode::Virtualize;
            CheckDecision(request, AudioVoiceAdmissionAction::AdmitVirtual, AudioVoiceAdmissionReason::Virtualized);
        }

        TEST_CASE("Voice replacement intersects saturated physical and authored buckets", "[unit][audio][admission]") {
            std::array voices{Candidate(1), Candidate(2), Candidate(3)};
            std::array bucket{voices[1].snapshot, voices[2].snapshot};
            std::array ownerBucket{voices[2].snapshot};
            std::array constraints{Constraint(bucket), Constraint(ownerBucket, 10)};
            constraints[0].group.maximumInstances = 2;
            auto request = Request(voices);
            request.constraints = constraints;
            CheckDecision(request, AudioVoiceAdmissionAction::Replace, AudioVoiceAdmissionReason::StopOldest, voices[2].snapshot.voice);
            ownerBucket[0] = voices[0].snapshot;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::NoEligibleVictim);
            constraints[0].group.maximumInstances = 1;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::OverCapacity);
            constraints[0].snapshot.lastAdmissionFrame = 99;
            constraints[0].group.retriggerFrames = 5;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::RetriggerWindow);
        }

        TEST_CASE("Voice admission never bypasses retrigger or instance ceilings with virtualization", "[unit][audio][admission]") {
            std::array voices{Candidate(1)};
            const std::array bucket{voices[0].snapshot};
            std::array constraints{Constraint(bucket)};
            auto request = Request(voices, AudioConcurrencyMode::Virtualize);
            request.constraints = constraints;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::InstanceCapacity);
            constraints[0].group.retriggerFrames = 2;
            constraints[0].snapshot.lastAdmissionFrame = 99;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::RetriggerWindow);
            constraints[0].snapshot.lastAdmissionFrame = 98;
            request.mode = AudioConcurrencyMode::StealOldest;
            CheckDecision(request, AudioVoiceAdmissionAction::Replace, AudioVoiceAdmissionReason::StopOldest, voices[0].snapshot.voice);
        }

        TEST_CASE("Voice admission counts reservations but protects stopping terminal and event proxy owners", "[unit][audio][admission]") {
            std::array voices{Candidate(1), Candidate(2)};
            voices[0].stealable = false;
            auto request = Request(voices);
            CheckDecision(request, AudioVoiceAdmissionAction::Replace, AudioVoiceAdmissionReason::StopOldest, voices[1].snapshot.voice);
            voices[1].snapshot.state = AudioVoiceState::Stopping;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::NoEligibleVictim);
            voices[1].snapshot.state = AudioVoiceState::Finished;
            voices[1].snapshot.terminalReason = AudioVoiceTerminalReason::Finished;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::NoEligibleVictim);
            voices[1].physical = false;
            CheckDecision(request, AudioVoiceAdmissionAction::AdmitPhysical, AudioVoiceAdmissionReason::CapacityAvailable);
            voices[1] = Candidate(2);
            voices[1].snapshot.state = AudioVoiceState::Virtual;
            voices[1].physical = false;
            const std::array bucket{voices[1].snapshot};
            std::array constraints{Constraint(bucket)};
            request.constraints = constraints;
            CheckDecision(request, AudioVoiceAdmissionAction::Replace, AudioVoiceAdmissionReason::StopOldest, voices[1].snapshot.voice);
            request.maximumPhysicalVoices = 1;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::NoEligibleVictim);
            constraints[0].group.countVirtual = false;
            CheckDecision(request, AudioVoiceAdmissionAction::Reject, AudioVoiceAdmissionReason::NoEligibleVictim);
        }

        TEST_CASE("Voice admission validates full inputs even when rejection is already certain", "[unit][audio][admission]") {
            std::array voices{Candidate(1), Candidate(2)};
            auto request = Request(voices, AudioConcurrencyMode::Reject);
            const auto checkInvalid = [&request] {
                const auto result = EvaluateAudioVoiceAdmission(request);
                REQUIRE(result.HasError());
                CHECK(result.ErrorValue().code.Value() == AudioErrors::ConcurrencyInvalid.code.Value());
            };
            voices[1] = voices[0];
            checkInvalid();
            voices[1] = Candidate(2);
            voices[1].audibleGain = std::numeric_limits<float>::quiet_NaN();
            checkInvalid();
            voices[1] = Candidate(2);
            voices[1].listenerDistance = -1;
            checkInvalid();
            voices[1] = Candidate(2);
            voices[1].priority = MaximumAudioPriority + 1;
            checkInvalid();
            voices[1] = Candidate(2);
            voices[1].snapshot.state = AudioVoiceState::Virtual;
            checkInvalid();
            voices[1] = Candidate(2);
            voices[1].snapshot.state = static_cast<AudioVoiceState>(255);
            checkInvalid();
            voices[1] = Candidate(2);
            request.mode = static_cast<AudioConcurrencyMode>(255);
            checkInvalid();
            request.mode = AudioConcurrencyMode::Reject;
            request.time.timelineGeneration = 0;
            checkInvalid();
        }

        TEST_CASE("Voice admission pause counting and scope generation remain authoritative", "[unit][audio][admission]") {
            std::array voices{Candidate(1)};
            voices[0].snapshot.state = AudioVoiceState::Paused;
            voices[0].physical = false;
            const std::array bucket{voices[0].snapshot};
            std::array constraints{Constraint(bucket)};
            auto request = Request(voices);
            request.constraints = constraints;
            CheckDecision(request, AudioVoiceAdmissionAction::Replace, AudioVoiceAdmissionReason::StopOldest, voices[0].snapshot.voice);
            constraints[0].group.countPaused = false;
            CheckDecision(request, AudioVoiceAdmissionAction::AdmitPhysical, AudioVoiceAdmissionReason::CapacityAvailable);
            constraints[0].group.scope = AudioConcurrencyScope::Emitter;
            constraints[0].request.emitter = {Runtime(), 1, 1};
            constraints[0].snapshot.key = MakeAudioConcurrencyKey(constraints[0].group, constraints[0].request).Value();
            REQUIRE(EvaluateAudioVoiceAdmission(request).HasValue());
            ++constraints[0].request.emitter.generation;
            CHECK(EvaluateAudioVoiceAdmission(request).HasError());
        }

        TEST_CASE("Voice admission rejects malformed dimensions before visiting projections", "[unit][audio][admission]") {
            const std::array voices{Candidate(1)};
            auto request = Request(voices);
            request.maximumPhysicalVoices = MaximumAudioVoiceSlots + 1;
            CHECK(EvaluateAudioVoiceAdmission(request).HasError());
            request.maximumPhysicalVoices = 1;
            request.runtime = {};
            CHECK(EvaluateAudioVoiceAdmission(request).HasError());
            request.runtime = Runtime();
            const std::array<AudioVoiceAdmissionConstraint, MaximumAudioVoiceAdmissionConstraints + 1> constraints{};
            request.constraints = constraints;
            CHECK(EvaluateAudioVoiceAdmission(request).HasError());
            request.constraints = {};
            auto foreignVoices = voices;
            foreignVoices[0].snapshot.voice.owner = AudioRuntimeId::Create(72).Value();
            request.voices = foreignVoices;
            const auto foreign = EvaluateAudioVoiceAdmission(request);
            REQUIRE(foreign.HasError());
            CHECK(foreign.ErrorValue().code.Value() == AudioErrors::HandleOwnerMismatch.code.Value());
        }

        TEST_CASE("Voice admission rejects stale foreign and contradictory bucket projections", "[unit][audio][admission]") {
            const std::array voices{Candidate(1)};
            std::array bucket{voices[0].snapshot};
            std::array constraints{Constraint(bucket)};
            auto request = Request(voices);
            request.constraints = constraints;
            bucket[0].state = AudioVoiceState::Ready;
            CHECK(EvaluateAudioVoiceAdmission(request).HasError());
            bucket[0] = voices[0].snapshot;
            constraints[0].snapshot.timelineGeneration = 2;
            const auto stale = EvaluateAudioVoiceAdmission(request);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == AudioErrors::ConcurrencyTimelineStale.code.Value());
            constraints[0].request.runtime = AudioRuntimeId::Create(72).Value();
            const auto foreign = EvaluateAudioVoiceAdmission(request);
            REQUIRE(foreign.HasError());
            CHECK(foreign.ErrorValue().code.Value() == AudioErrors::HandleOwnerMismatch.code.Value());
        }
    }  // namespace
}  // namespace Horo::Audio
