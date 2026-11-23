#include "Horo/Audio/AudioDeviceDiscovery.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <span>
#include <type_traits>

namespace Horo::Audio {
    namespace {
        AudioDeviceSnapshot Snapshot(const AudioBackendKind backend = AudioBackendKind::CoreAudio) {
            const auto owner = AudioRuntimeId::Create(7).Value();
            const auto deviceClass = backend == AudioBackendKind::NullAudio ? AudioDeviceClass::Headless : AudioDeviceClass::Physical;
            return {.owner = owner,
                    .backend = backend,
                    .revision = 9,
                    .devices = {{{owner, 1, 2}, "Output", deviceClass}, {{owner, 2, 1}, "Output", deviceClass}},
                    .defaults = {.console = AudioDeviceId{owner, 1, 2},
                                 .multimedia = AudioDeviceId{owner, 2, 1},
                                 .communications = AudioDeviceId{owner, 1, 2}}};
        }

        void ExpectFailure(const AudioDeviceResolution &result, const AudioDeviceResolutionStatus status) {
            REQUIRE(result.status == status);
            REQUIRE(result.device == AudioDeviceId{});
            REQUIRE(result.revision == 0);
        }

        using SnapshotMutation = void (*)(AudioDeviceSnapshot &);

        void ExpectRejectedMutations(const std::span<const SnapshotMutation> mutations) {
            for (const auto mutation : mutations) {
                auto snapshot = Snapshot();
                mutation(snapshot);
                REQUIRE_FALSE(ValidateAudioDeviceSnapshot(snapshot));
                ExpectFailure(ResolveAudioDevice(snapshot, AudioDefaultDeviceRole::Console), AudioDeviceResolutionStatus::InvalidSnapshot);
            }
        }

        TEST_CASE("Audio discovery shares generation scoped selection across all backend peers", "[audio][discovery]") {
            for (const auto backend : {AudioBackendKind::WASAPI, AudioBackendKind::CoreAudio, AudioBackendKind::PipeWire,
                                       AudioBackendKind::SDL3Audio, AudioBackendKind::NullAudio}) {
                auto snapshot = Snapshot(backend);
                REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
                const AudioDeviceSelection selection = snapshot.devices[0].id;
                std::ranges::reverse(snapshot.devices);
                const auto resolved = ResolveAudioDevice(snapshot, selection);
                REQUIRE(resolved.status == AudioDeviceResolutionStatus::Resolved);
                REQUIRE(resolved.device == std::get<AudioDeviceId>(selection));
                REQUIRE(resolved.revision == 9);
                REQUIRE(std::get<AudioDeviceId>(selection) == snapshot.devices[1].id);
            }
        }

        TEST_CASE("PipeWire can publish a device without claiming physical endpoint classification", "[audio][discovery]") {
            auto snapshot = Snapshot(AudioBackendKind::PipeWire);
            snapshot.devices.front().deviceClass = AudioDeviceClass::Unknown;
            REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
            CHECK(ResolveAudioDevice(snapshot, snapshot.devices.front().id).status == AudioDeviceResolutionStatus::Resolved);
            snapshot.backend = AudioBackendKind::NullAudio;
            CHECK_FALSE(ValidateAudioDeviceSnapshot(snapshot));
        }

        TEST_CASE("Audio discovery defaults never imply role or device fallback", "[audio][discovery]") {
            auto snapshot = Snapshot();
            const std::array roles{AudioDefaultDeviceRole::Console, AudioDefaultDeviceRole::Multimedia,
                                   AudioDefaultDeviceRole::Communications};
            const std::array expected{snapshot.devices[0].id, snapshot.devices[1].id, snapshot.devices[0].id};
            for (std::size_t index = 0; index < roles.size(); ++index) {
                const auto resolved = ResolveAudioDevice(snapshot, roles[index]);
                REQUIRE(resolved.status == AudioDeviceResolutionStatus::Resolved);
                REQUIRE(resolved.device == expected[index]);
            }
            snapshot.defaults = {};
            for (const auto role : roles)
                ExpectFailure(ResolveAudioDevice(snapshot, role), AudioDeviceResolutionStatus::Unavailable);
            ExpectFailure(ResolveAudioDevice(snapshot, static_cast<AudioDefaultDeviceRole>(255)),
                          AudioDeviceResolutionStatus::InvalidSelection);
            snapshot.devices.clear();
            REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
            ExpectFailure(ResolveAudioDevice(snapshot, roles[0]), AudioDeviceResolutionStatus::Unavailable);
        }

        TEST_CASE("Audio discovery rejects stale foreign and malformed explicit selections", "[audio][discovery]") {
            const auto snapshot = Snapshot();
            auto selected = snapshot.devices[0].id;
            selected.generation = 1;
            ExpectFailure(ResolveAudioDevice(snapshot, selected), AudioDeviceResolutionStatus::Unavailable);
            selected = snapshot.devices[0].id;
            selected.slot = 3;
            ExpectFailure(ResolveAudioDevice(snapshot, selected), AudioDeviceResolutionStatus::Unavailable);
            selected.owner = AudioRuntimeId::Create(8).Value();
            ExpectFailure(ResolveAudioDevice(snapshot, selected), AudioDeviceResolutionStatus::InvalidSelection);
            ExpectFailure(ResolveAudioDevice(snapshot, AudioDeviceId{}), AudioDeviceResolutionStatus::InvalidSelection);
        }

        TEST_CASE("Audio discovery rejects invalid snapshot identity and version", "[audio][discovery]") {
            const std::array<SnapshotMutation, 4> mutations{
                [](auto &value) {
                value.contractVersion = 2;
            },
                [](auto &value) {
                value.owner = {};
            },
                [](auto &value) {
                value.revision = 0;
            },
                [](auto &value) {
                value.backend = static_cast<AudioBackendKind>(255);
            },
            };
            ExpectRejectedMutations(mutations);
        }

        TEST_CASE("Audio discovery invalid endpoint metadata cannot publish a resolved device", "[audio][discovery]") {
            const std::array<SnapshotMutation, 10> mutations{
                [](auto &value) {
                value.devices[0].id = {};
            },
                [](auto &value) {
                value.devices[0].id.owner = AudioRuntimeId::Create(8).Value();
            },
                [](auto &value) {
                value.devices[0].displayName.assign(257, 'x');
            },
                [](auto &value) {
                value.devices[0].deviceClass = static_cast<AudioDeviceClass>(255);
            },
                [](auto &value) {
                value.devices[0].deviceClass = AudioDeviceClass::Headless;
            },
                [](auto &value) {
                value.devices[1].id = value.devices[0].id;
            },
                [](auto &value) {
                value.devices[1].id.slot = value.devices[0].id.slot;
            },
                [](auto &value) {
                value.defaults.console->generation = 99;
            },
                [](auto &value) {
                value.defaults.multimedia->generation = 99;
            },
                [](auto &value) {
                value.defaults.communications->generation = 99;
            },
            };
            ExpectRejectedMutations(mutations);
        }

        TEST_CASE("Audio discovery enforces complete bounded snapshots and truthful Null endpoints", "[audio][discovery]") {
            auto snapshot = Snapshot();
            snapshot.devices[0].displayName.assign(256, 'x');
            snapshot.devices[1].deviceClass = AudioDeviceClass::Virtual;
            REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
            while (snapshot.devices.size() < MaximumAudioDiscoveredDevices) {
                const auto slot = static_cast<std::uint32_t>(snapshot.devices.size() + 1);
                snapshot.devices.push_back({{snapshot.owner, slot, 1}, "Output", AudioDeviceClass::Virtual});
            }
            REQUIRE(ValidateAudioDeviceSnapshot(snapshot));
            snapshot.devices.push_back({{snapshot.owner, 257, 1}, "Overflow", AudioDeviceClass::Virtual});
            REQUIRE_FALSE(ValidateAudioDeviceSnapshot(snapshot));
            auto nullSnapshot = Snapshot(AudioBackendKind::NullAudio);
            nullSnapshot.devices[0].deviceClass = AudioDeviceClass::Physical;
            REQUIRE_FALSE(ValidateAudioDeviceSnapshot(nullSnapshot));
        }

        TEST_CASE("Audio discovery owns metadata and preserves selection intent across default changes", "[audio][discovery]") {
            const auto original = Snapshot();
            auto changed = original;
            changed.defaults.multimedia = changed.devices[0].id;
            changed.revision = 10;
            changed.devices[0].displayName = "Replacement display label";
            const AudioDeviceSelection intent = AudioDefaultDeviceRole::Multimedia;
            const auto before = ResolveAudioDevice(original, intent);
            const auto after = ResolveAudioDevice(changed, intent);
            REQUIRE(before.device != after.device);
            REQUIRE(before.revision == 9);
            REQUIRE(after.revision == 10);
            REQUIRE(original.devices[0].displayName == "Output");
            REQUIRE(std::get<AudioDefaultDeviceRole>(intent) == AudioDefaultDeviceRole::Multimedia);
        }

        TEST_CASE("Recording device selection stays independent of playback and retains exact default roles",
                  "[audio][discovery][capture]") {
            static_assert(!std::is_constructible_v<AudioDeviceSelection, AudioCaptureDeviceId>);
            static_assert(!std::is_constructible_v<AudioCaptureDeviceSelection, AudioDeviceId>);
            const auto playback = Snapshot();
            const AudioCaptureDeviceId microphone{playback.owner, 1, 2};
            const AudioCaptureDeviceId headset{playback.owner, 2, 1};
            const AudioProcessingFormat mono{48'000, MakeAudioSpeakerLayout(AudioSpeakerPreset::Mono)};
            AudioCaptureDeviceSnapshot input{.owner = playback.owner,
                                             .backend = playback.backend,
                                             .revision = 5,
                                             .devices = {{microphone, "Microphone", AudioDeviceClass::Physical, mono},
                                                         {headset, "Headset", AudioDeviceClass::Physical, std::nullopt}},
                                             .defaults = {.multimedia = microphone, .communications = headset},
                                             .permission = AudioCapturePermission::Required};
            REQUIRE(ValidateAudioCaptureDeviceSnapshot(input));
            const auto defaultMic = ResolveAudioCaptureDevice(input, AudioDefaultDeviceRole::Multimedia);
            CHECK(defaultMic.status == AudioDeviceResolutionStatus::Resolved);
            CHECK(defaultMic.device == microphone);
            CHECK(defaultMic.revision == 5);
            CHECK(ResolveAudioCaptureDevice(input, AudioDefaultDeviceRole::Communications).device == headset);
            CHECK(ResolveAudioCaptureDevice(input, AudioDefaultDeviceRole::Console).status == AudioDeviceResolutionStatus::Unavailable);

            input.devices.erase(input.devices.begin());
            input.defaults.multimedia.reset();
            ++input.revision;
            const auto removed = ResolveAudioCaptureDevice(input, microphone);
            CHECK(removed.status == AudioDeviceResolutionStatus::Unavailable);
            CHECK_FALSE(removed.device.IsValid());
            CHECK(removed.revision == 0);
            CHECK(ResolveAudioDevice(playback, playback.devices.front().id).status == AudioDeviceResolutionStatus::Resolved);
        }

        TEST_CASE("Capture discovery rejects invalid formats, permission states and stale defaults", "[audio][discovery][capture]") {
            const auto owner = AudioRuntimeId::Create(7).Value();
            const AudioCaptureDeviceId device{owner, 1, 2};
            AudioCaptureDeviceSnapshot input{.owner = owner,
                                             .backend = AudioBackendKind::WASAPI,
                                             .revision = 5,
                                             .devices = {{device, "Microphone"}},
                                             .defaults = {.communications = device},
                                             .permission = AudioCapturePermission::Denied};
            REQUIRE(ValidateAudioCaptureDeviceSnapshot(input));
            input.devices.front().preferredFormat = AudioProcessingFormat{};
            CHECK_FALSE(ValidateAudioCaptureDeviceSnapshot(input));
            input.devices.front().preferredFormat.reset();
            input.permission = static_cast<AudioCapturePermission>(255);
            CHECK_FALSE(ValidateAudioCaptureDeviceSnapshot(input));
            input.permission = AudioCapturePermission::Unknown;
            input.defaults.communications->generation = 1;
            CHECK(ResolveAudioCaptureDevice(input, device).status == AudioDeviceResolutionStatus::InvalidSnapshot);
        }

        TEST_CASE("Recording discovery emits removal before reusing a slot at a new generation", "[audio][discovery][capture]") {
            const auto owner = AudioRuntimeId::Create(7).Value();
            const AudioCaptureDeviceId retired{owner, 1, 2};
            const AudioCaptureDeviceId replacement{owner, 1, 3};
            const AudioCaptureDeviceId unchanged{owner, 2, 1};
            const AudioCaptureDeviceSnapshot before{.owner = owner,
                                                    .backend = AudioBackendKind::SDL3Audio,
                                                    .revision = 8,
                                                    .devices = {{retired, "Microphone"}, {unchanged, "Headset"}}};
            auto after = before;
            after.revision = 9;
            after.devices.front().id = replacement;
            std::ranges::reverse(after.devices);
            const auto changes = DescribeAudioCaptureDeviceChanges(before, after);
            REQUIRE(changes.HasValue());
            REQUIRE(changes.Value().size() == 2);
            CHECK(changes.Value()[0].kind == AudioCaptureDeviceChangeKind::Removed);
            CHECK(changes.Value()[0].device == retired);
            CHECK(changes.Value()[0].revision == after.revision);
            CHECK(changes.Value()[1].kind == AudioCaptureDeviceChangeKind::Added);
            CHECK(changes.Value()[1].device == replacement);
            CHECK(DescribeAudioCaptureDeviceChanges(after, before).HasError());
            auto staleGeneration = before;
            staleGeneration.revision = 10;
            REQUIRE(ValidateAudioCaptureDeviceSnapshot(staleGeneration));
            CHECK(DescribeAudioCaptureDeviceChanges(after, staleGeneration).HasError());
            after.backend = AudioBackendKind::CoreAudio;
            CHECK(DescribeAudioCaptureDeviceChanges(before, after).HasError());
        }
    }  // namespace
}  // namespace Horo::Audio
