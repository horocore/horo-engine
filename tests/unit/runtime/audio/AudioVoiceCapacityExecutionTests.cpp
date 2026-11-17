#include "AllocationProbe.h"
#include "AudioVoicePlaybackTestFixture.h"
#include "Horo/Audio/AudioVoiceAdmission.h"

#include <optional>
#include <vector>

namespace Horo::Audio {
    namespace {
        using namespace PlaybackTest;

        /** @brief Own actual PCM voices and project their canonical registry state at the physical ceiling. */
        struct CapacityRig final {
            AudioVoiceStateMachine registry;
            Samples samples;
            std::vector<std::optional<AudioVoicePlayback>> voices;
            std::vector<AudioVoiceAdmissionCandidate> candidates;
            std::uint32_t capacity;

            CapacityRig(const std::uint32_t limit, const bool virtualSlot)
                : registry(Registry(limit + (virtualSlot ? 1U : 0U))), capacity(limit) {
                voices.reserve(limit);
                candidates.reserve(limit);
                for (std::uint32_t index = 0; index < limit; ++index) {
                    voices.emplace_back(Playback(registry, samples, 0.5, {true, 3, 7}));
                    auto &voice = *voices.back();
                    Control(voice, AudioVoiceControl::Start);
                    REQUIRE(voice.Render(samples.Destination(16)).error == nullptr);
                    candidates.push_back({.snapshot = registry.Snapshot(voice.Voice()).Value(),
                                          .admissionOrder = index + 1U,
                                          .audibleGain = 1.0F,
                                          .priority = 100,
                                          .listenerDistance = 1.0,
                                          .physical = true});
                }
            }

            /** @brief Reevaluate one immutable projection without changing ownership or allocating on the decision path. */
            AudioVoiceAdmissionDecision Decide(const AudioConcurrencyMode mode) const {
                const AudioVoiceAdmissionRequest request{.runtime = Owner(),
                                                         .maximumPhysicalVoices = capacity,
                                                         .mode = mode,
                                                         .time = {1, 100},
                                                         .voices = candidates};
                const auto before = Tests::AllocationProbe::Count();
                const auto result = EvaluateAudioVoiceAdmission(request);
                const auto after = Tests::AllocationProbe::Count();
                REQUIRE(result.HasValue());
                CHECK(after == before);
                return result.Value();
            }
        };

        /** @brief Retire the chosen physical voice before reusing its slot, then fence the previous generation. */
        void ReplaceVictim(CapacityRig &rig, const AudioVoiceHandle victim) {
            REQUIRE(victim == rig.voices.front()->Voice());
            auto &selected = rig.voices.front();
            const auto allocations = Tests::AllocationProbe::Count();
            const auto frees = Tests::AllocationProbe::FreeCount();
            const auto *error = selected->Apply({victim, AudioVoiceControl::Stop});
            const auto terminal = selected->Render(rig.samples.Destination(16));
            const auto repeated = selected->Render(rig.samples.Destination(1));
            const auto allocationEnd = Tests::AllocationProbe::Count();
            const auto freeEnd = Tests::AllocationProbe::FreeCount();
            REQUIRE(error == nullptr);
            REQUIRE(terminal.error == nullptr);
            CHECK(terminal.terminal);
            CHECK(repeated.error == nullptr);
            CHECK_FALSE(repeated.terminal);
            CHECK(allocationEnd == allocations);
            CHECK(freeEnd == frees);
            CHECK(rig.registry.State(victim).Value() == AudioVoiceState::Stopped);
            selected.reset();  // This exclusive test host has no queued callback/mixer borrows.
            selected.emplace(Playback(rig.registry, rig.samples));
            CHECK(selected->Voice().slot == victim.slot);
            CHECK(selected->Voice().generation != victim.generation);
            CHECK(rig.registry.State(victim).HasError());
            CHECK(selected->Apply({victim, AudioVoiceControl::Cancel}) != nullptr);
            Control(*selected, AudioVoiceControl::Start);
            CHECK(selected->Render(rig.samples.Destination(16)).produced == 16);
        }

        /** @brief Virtual admission consumes its own logical slot while leaving every physical owner running. */
        void AdmitVirtual(CapacityRig &rig) {
            auto extra = Playback(rig.registry, rig.samples, 0.5, {true, 3, 7});
            Control(extra, AudioVoiceControl::StartVirtual);
            const auto rendered = extra.Render(rig.samples.Destination(40));
            REQUIRE(rendered.error == nullptr);
            CHECK(rendered.produced == 0);
            CHECK(extra.Cursor().frame == 4);
            CHECK(extra.Cursor().fraction == 0.0);
            CHECK(rig.registry.State(extra.Voice()).Value() == AudioVoiceState::Virtual);
            CHECK(rig.registry.CreateVoice().HasError());
        }

        /** @brief Every logical slot can be admitted again after the exclusive processing owners retire. */
        void CheckAllSlotsReusable(CapacityRig &rig, const std::uint32_t slots) {
            rig.voices.clear();
            std::vector<AudioVoiceHandle> admitted;
            admitted.reserve(slots);
            for (std::uint32_t index = 0; index < slots; ++index) {
                const auto voice = rig.registry.CreateVoice();
                REQUIRE(voice.HasValue());
                admitted.push_back(voice.Value());
            }
            CHECK(rig.registry.CreateVoice().HasError());
            for (const auto voice : admitted) {
                REQUIRE(rig.registry.Cancel(voice).HasValue());
                REQUIRE(rig.registry.Release(voice).HasValue());
            }
        }

        TEST_CASE("Every capacity policy commits against real PCM owners and retires reusable generations",
                  "[audio][admission][playback][qualification]") {
            for (const std::uint32_t capacity : {1U, 64U, 256U, 1024U}) {
                for (const auto mode :
                     {AudioConcurrencyMode::Allow, AudioConcurrencyMode::RejectNew, AudioConcurrencyMode::StealOldest,
                      AudioConcurrencyMode::StealQuietest, AudioConcurrencyMode::StealLowestPriority, AudioConcurrencyMode::StealFurthest,
                      AudioConcurrencyMode::Replace, AudioConcurrencyMode::Virtualize}) {
                    CAPTURE(capacity, mode);
                    const bool virtualSlot = mode == AudioConcurrencyMode::Virtualize;
                    CapacityRig rig{capacity, virtualSlot};
                    const auto decision = rig.Decide(mode);
                    CHECK(rig.Decide(mode) == decision);
                    if (mode == AudioConcurrencyMode::Allow || mode == AudioConcurrencyMode::RejectNew) {
                        CHECK(decision.action == AudioVoiceAdmissionAction::Reject);
                        CHECK_FALSE(decision.victim.has_value());
                        CHECK(rig.registry.CreateVoice().HasError());
                    } else if (virtualSlot) {
                        REQUIRE(decision.action == AudioVoiceAdmissionAction::AdmitVirtual);
                        AdmitVirtual(rig);
                    } else {
                        REQUIRE(decision.action == AudioVoiceAdmissionAction::Replace);
                        REQUIRE(decision.victim.has_value());
                        ReplaceVictim(rig, *decision.victim);
                    }
                    for (const auto &voice : rig.voices)
                        CHECK(rig.registry.State(voice->Voice()).Value() == AudioVoiceState::Playing);
                    CheckAllSlotsReusable(rig, capacity + (virtualSlot ? 1U : 0U));
                }
            }
        }
    }  // namespace
}  // namespace Horo::Audio
