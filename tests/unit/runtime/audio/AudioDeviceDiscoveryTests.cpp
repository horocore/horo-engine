#include "Horo/Audio/AudioDeviceDiscovery.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <span>

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
    }  // namespace
}  // namespace Horo::Audio
