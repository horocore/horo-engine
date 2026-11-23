#include "Horo/Audio/AudioDeviceDiscovery.h"

#include "Horo/Audio/AudioErrors.h"

#include <algorithm>
#include <new>

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
        bool Contains(const auto &snapshot, const auto &id) noexcept {
            return std::ranges::any_of(snapshot.devices, [&id](const auto &device) {
                return device.id == id;
            });
        }

        /** @brief Missing default roles are valid; present roles must name an enumerated endpoint. */
        bool ValidDefault(const auto &snapshot, const auto &id) noexcept {
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

        /** @brief SDL capture inventory does not disclose physical-versus-virtual provenance. */
        bool ValidClass(const AudioCaptureDeviceSnapshot &snapshot, const AudioDeviceClass deviceClass) noexcept {
            return ValidClass(snapshot.backend, deviceClass) ||
                   (snapshot.backend == AudioBackendKind::SDL3Audio && deviceClass == AudioDeviceClass::Unknown);
        }

        /** @brief Retains the playback classification contract. */
        bool ValidClass(const AudioDeviceSnapshot &snapshot, const AudioDeviceClass deviceClass) noexcept {
            return ValidClass(snapshot.backend, deviceClass);
        }

        /** @brief Resolves a role without inventing cross-role or enumeration-order fallback. */
        template <class Resolution> Resolution ResolveDefault(const auto &snapshot, const AudioDefaultDeviceRole role) noexcept {
            using enum AudioDefaultDeviceRole;
            using enum AudioDeviceResolutionStatus;
            const decltype(snapshot.defaults.console) *binding{};
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
        bool ValidDevices(const auto &snapshot) noexcept {
            for (std::size_t index = 0; index < snapshot.devices.size(); ++index) {
                const auto &device = snapshot.devices[index];
                if (!device.id.IsValid() || device.id.owner != snapshot.owner || device.displayName.size() > 256 ||
                    !ValidClass(snapshot, device.deviceClass))
                    return false;
                for (std::size_t previous = 0; previous < index; ++previous) {
                    if (snapshot.devices[previous].id.slot == device.id.slot)
                        return false;
                }
            }
            return true;
        }

        /** @brief Checks the shared shape of a playback or capture catalog before using any endpoint. */
        bool ValidCatalog(const auto &snapshot) noexcept {
            if (snapshot.contractVersion != 1 || !snapshot.owner.IsValid() || snapshot.revision == 0 || !KnownBackend(snapshot.backend) ||
                snapshot.devices.size() > MaximumAudioDiscoveredDevices)
                return false;
            return ValidDevices(snapshot) && ValidDefault(snapshot, snapshot.defaults.console) &&
                   ValidDefault(snapshot, snapshot.defaults.multimedia) && ValidDefault(snapshot, snapshot.defaults.communications);
        }

        /** @brief A refresh compares successive complete observations of the same owner and backend. */
        bool ValidCaptureTransition(const AudioCaptureDeviceSnapshot &before, const AudioCaptureDeviceSnapshot &after) noexcept {
            return before.owner == after.owner && before.backend == after.backend && before.revision < after.revision &&
                   ValidateAudioCaptureDeviceSnapshot(before) && ValidateAudioCaptureDeviceSnapshot(after);
        }

        /** @brief Keeps explicit selection fenced to this catalog's exact owner and generation. */
        template <class Resolution, class DeviceId>
        Resolution ResolveSelection(const auto &snapshot, const std::variant<DeviceId, AudioDefaultDeviceRole> &selection) noexcept {
            using enum AudioDeviceResolutionStatus;
            if (const auto *explicitDevice = std::get_if<DeviceId>(&selection)) {
                if (!explicitDevice->IsValid() || explicitDevice->owner != snapshot.owner)
                    return {.status = InvalidSelection};
                if (!Contains(snapshot, *explicitDevice))
                    return {.status = Unavailable};
                return {.status = Resolved, .device = *explicitDevice, .revision = snapshot.revision};
            }
            return ResolveDefault<Resolution>(snapshot, std::get<AudioDefaultDeviceRole>(selection));
        }
    }  // namespace

    /** @copydoc ValidateAudioDeviceSnapshot */
    bool ValidateAudioDeviceSnapshot(const AudioDeviceSnapshot &snapshot) noexcept {
        return ValidCatalog(snapshot);
    }

    /** @copydoc ResolveAudioDevice */
    AudioDeviceResolution ResolveAudioDevice(const AudioDeviceSnapshot &snapshot, const AudioDeviceSelection &selection) noexcept {
        if (!ValidateAudioDeviceSnapshot(snapshot))
            return {};
        return ResolveSelection<AudioDeviceResolution>(snapshot, selection);
    }

    /** @copydoc ValidateAudioCaptureDeviceSnapshot */
    bool ValidateAudioCaptureDeviceSnapshot(const AudioCaptureDeviceSnapshot &snapshot) noexcept {
        return ValidCatalog(snapshot) && snapshot.permission <= AudioCapturePermission::Denied &&
               std::ranges::all_of(snapshot.devices, [](const AudioDiscoveredCaptureDevice &device) {
            return !device.preferredFormat || ValidateAudioProcessingFormat(*device.preferredFormat);
        });
    }

    /** @copydoc ResolveAudioCaptureDevice */
    AudioCaptureDeviceResolution ResolveAudioCaptureDevice(const AudioCaptureDeviceSnapshot &snapshot,
                                                           const AudioCaptureDeviceSelection &selection) noexcept {
        if (!ValidateAudioCaptureDeviceSnapshot(snapshot))
            return {};
        return ResolveSelection<AudioCaptureDeviceResolution>(snapshot, selection);
    }

    /** @copydoc DescribeAudioCaptureDeviceChanges */
    Result<std::vector<AudioCaptureDeviceChange>> DescribeAudioCaptureDeviceChanges(const AudioCaptureDeviceSnapshot &before,
                                                                                    const AudioCaptureDeviceSnapshot &after) {
        using Output = Result<std::vector<AudioCaptureDeviceChange>>;
        if (!ValidCaptureTransition(before, after))
            return Output::Failure(MakeError(AudioErrors::IdentityInvalid));
        try {
            std::vector<AudioCaptureDeviceChange> changes;
            for (const auto &device : before.devices) {
                const auto current = std::ranges::find_if(after.devices, [&device](const auto &candidate) {
                    return candidate.id.slot == device.id.slot;
                });
                if (current != after.devices.end() && current->id.generation < device.id.generation)
                    return Output::Failure(MakeError(AudioErrors::IdentityInvalid));
                if (current == after.devices.end() || current->id != device.id)
                    changes.emplace_back(AudioCaptureDeviceChangeKind::Removed, device.id, after.revision);
            }
            for (const auto &device : after.devices)
                if (!Contains(before, device.id))
                    changes.emplace_back(AudioCaptureDeviceChangeKind::Added, device.id, after.revision);
            return Output::Success(std::move(changes));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }
}  // namespace Horo::Audio
