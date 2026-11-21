#include "Horo/Audio/AudioDeviceDiscovery.h"

#include <algorithm>

namespace Horo::Audio {
    namespace {  // NOSONAR - internal-linkage validation helpers must not enter the public audio namespace.

        /** @brief Rejects unknown first-party backend identity values. */
        bool KnownBackend(const AudioBackendKind backend) noexcept {
            using enum AudioBackendKind;
            switch (backend) {
                case WASAPI:
                case CoreAudio:
                case PipeWire:
                case SDL3Audio:
                case NullAudio:
                    return true;
            }
            return false;
        }

        /** @brief Matches the complete generation, not a reused slot or presentation name. */
        bool Contains(const AudioDeviceSnapshot &snapshot, const AudioDeviceId &id) noexcept {
            return std::ranges::any_of(snapshot.devices, [&id](const auto &device) {
                return device.id == id;
            });
        }

        /** @brief Missing default roles are valid; present roles must name an enumerated endpoint. */
        bool ValidDefault(const AudioDeviceSnapshot &snapshot, const std::optional<AudioDeviceId> &id) noexcept {
            return !id || Contains(snapshot, *id);
        }

        /** @brief Null endpoints cannot make physical claims; physical peers cannot masquerade as Null. */
        bool ValidClass(const AudioBackendKind backend, const AudioDeviceClass deviceClass) noexcept {
            using enum AudioBackendKind;
            using enum AudioDeviceClass;
            if (backend == NullAudio)
                return deviceClass == Headless;
            return deviceClass == Physical || deviceClass == Virtual || (backend == PipeWire && deviceClass == Unknown);
        }

        /** @brief Resolves a role without inventing cross-role or enumeration-order fallback. */
        AudioDeviceResolution ResolveDefault(const AudioDeviceSnapshot &snapshot, const AudioDefaultDeviceRole role) noexcept {
            using enum AudioDefaultDeviceRole;
            using enum AudioDeviceResolutionStatus;
            const std::optional<AudioDeviceId> *binding{};
            switch (role) {
                case Console:
                    binding = &snapshot.defaults.console;
                    break;
                case Multimedia:
                    binding = &snapshot.defaults.multimedia;
                    break;
                case Communications:
                    binding = &snapshot.defaults.communications;
                    break;
                default:
                    return {.status = InvalidSelection};
            }
            if (!*binding)
                return {.status = Unavailable};
            return {.status = Resolved, .device = **binding, .revision = snapshot.revision};
        }

        /** @brief Checks endpoint shape and prevents simultaneous generations of one registry slot. */
        bool ValidDevices(const AudioDeviceSnapshot &snapshot) noexcept {
            for (std::size_t index = 0; index < snapshot.devices.size(); ++index) {
                const auto &device = snapshot.devices[index];
                if (!device.id.IsValid() || device.id.owner != snapshot.owner || device.displayName.size() > 256 ||
                    !ValidClass(snapshot.backend, device.deviceClass))
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (snapshot.devices[previous].id.slot == device.id.slot)
                        return false;
                }
            }
            return true;
        }
    }  // namespace

    /** @copydoc ValidateAudioDeviceSnapshot */
    bool ValidateAudioDeviceSnapshot(const AudioDeviceSnapshot &snapshot) noexcept {
        if (snapshot.contractVersion != 1 || !snapshot.owner.IsValid() || snapshot.revision == 0 || !KnownBackend(snapshot.backend) ||
            snapshot.devices.size() > MaximumAudioDiscoveredDevices)
            return false;
        return ValidDevices(snapshot) && ValidDefault(snapshot, snapshot.defaults.console) &&
               ValidDefault(snapshot, snapshot.defaults.multimedia) && ValidDefault(snapshot, snapshot.defaults.communications);
    }

    /** @copydoc ResolveAudioDevice */
    AudioDeviceResolution ResolveAudioDevice(const AudioDeviceSnapshot &snapshot, const AudioDeviceSelection &selection) noexcept {
        using enum AudioDeviceResolutionStatus;
        if (!ValidateAudioDeviceSnapshot(snapshot))
            return {};
        if (const auto *explicitDevice = std::get_if<AudioDeviceId>(&selection)) {
            if (!explicitDevice->IsValid() || explicitDevice->owner != snapshot.owner)
                return {.status = InvalidSelection};
            if (!Contains(snapshot, *explicitDevice))
                return {.status = Unavailable};
            return {.status = Resolved, .device = *explicitDevice, .revision = snapshot.revision};
        }
        return ResolveDefault(snapshot, std::get<AudioDefaultDeviceRole>(selection));
    }
}  // namespace Horo::Audio
