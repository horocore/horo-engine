#include "Horo/Network/NetworkProjectSettings.h"

#include "Horo/Foundation/StableHash.h"

#include <algorithm>
#include <utility>

namespace Horo::Network {
    namespace {
        constexpr NetworkProjectRoleSet AllRoles = NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client |
                                                   NetworkProjectRoleSet::ListenServer | NetworkProjectRoleSet::DedicatedServer;

        [[nodiscard]] bool IsKnownRole(const NetworkProjectRole role) noexcept {
            return role < NetworkProjectRole::Count;
        }

        [[nodiscard]] bool IsKnownRoleSet(const NetworkProjectRoleSet roles) noexcept {
            const auto bits = static_cast<std::uint32_t>(roles);
            return bits != 0 && (bits & ~static_cast<std::uint32_t>(AllRoles)) == 0;
        }

        [[nodiscard]] bool HasRequiredDelivery(const TransportRequirements &requirements) noexcept {
            return std::ranges::any_of(requirements.requiredDelivery, [](const bool required) {
                return required;
            });
        }

        [[nodiscard]] bool IsValidTransportRequirements(const NetworkProjectTransportPolicy &policy) noexcept {
            if (policy.requirement >= NetworkProjectTransportRequirement::Count)
                return false;

            const auto &requirements = policy.capabilities;
            if (requirements.deadline >= DeadlineRequirement::Count)
                return false;
            if (requirements.deadline == DeadlineRequirement::None && requirements.requiredMaximumDeadlineMilliseconds != 0)
                return false;
            if (requirements.deadline != DeadlineRequirement::None && requirements.requiredMaximumDeadlineMilliseconds == 0)
                return false;

            if (policy.requirement == NetworkProjectTransportRequirement::Optional) {
                return !HasRequiredDelivery(requirements) && requirements.requiredChannels == 0 &&
                       requirements.requiredMaximumMessageBytes == 0 && requirements.deadline == DeadlineRequirement::None;
            }

            return HasRequiredDelivery(requirements) && requirements.requiredChannels != 0 && requirements.requiredMaximumMessageBytes != 0;
        }

        [[nodiscard]] bool IsValidProfile(const NetworkProjectProfileV1 &profile) noexcept {
            if (!profile.id.IsValid() || !profile.revision.IsValid() || !profile.networkTickRate.IsValid() ||
                !profile.maxActiveConnections.IsValid() || !profile.maxNetworkObjects.IsValid() ||
                !profile.maxCandidatesPerConnectionPerTick.IsValid() || !profile.maxRelevantObjectsPerConnection.IsValid() ||
                !profile.maxInterestTransitionsPerConnectionPerTick.IsValid() || !profile.maxCapturedObjectsPerTick.IsValid() ||
                !profile.maxSerializedFieldsPerConnectionPerTick.IsValid() || !profile.maxGlobalInterestWorkPerTick.IsValid() ||
                !profile.maxGlobalCaptureWorkPerTick.IsValid() || !profile.maxGlobalSchedulingWorkPerTick.IsValid() ||
                !profile.targetBytesPerConnection.IsValid() || !profile.burstBytesPerConnection.IsValid() ||
                !profile.maxMessagesPerConnectionPerTick.IsValid() || !profile.maxReliableQueuedBytesPerConnection.IsValid() ||
                !profile.maxUnreliableQueuedBytesPerConnection.IsValid() || !profile.enterDwellTicks.IsValid() ||
                !profile.exitDwellTicks.IsValid() || !profile.maxEligibleStarvationTicks.IsValid() ||
                !profile.saturationGraceTicks.IsValid())
                return false;

            return true;
        }

        [[nodiscard]] bool IsProfileCapacityCoherent(const NetworkProjectProfileV1 &profile) noexcept {
            return profile.maxRelevantObjectsPerConnection.value <= profile.maxCandidatesPerConnectionPerTick.value &&
                   profile.maxCandidatesPerConnectionPerTick.value <= profile.maxNetworkObjects.value &&
                   profile.maxCapturedObjectsPerTick.value <= profile.maxNetworkObjects.value &&
                   profile.burstBytesPerConnection.value >= profile.targetBytesPerConnection.value;
        }

        [[nodiscard]] bool IsValidProtocol(const NetworkProjectProtocolPolicy &policy) noexcept {
            return policy.protocol.IsValid() && policy.supportedVersions.IsValid() && policy.schemaFingerprint != 0;
        }

        void HashProfile(Foundation::StableHash64 &hash, const NetworkProjectProfileV1 &profile) noexcept {
            hash.AddInteger(profile.id.Value());
            hash.AddInteger(profile.revision.Value());
            hash.AddInteger(profile.networkTickRate.value);
            hash.AddInteger(profile.maxActiveConnections.value);
            hash.AddInteger(profile.maxNetworkObjects.value);
            hash.AddInteger(profile.maxCandidatesPerConnectionPerTick.value);
            hash.AddInteger(profile.maxRelevantObjectsPerConnection.value);
            hash.AddInteger(profile.maxInterestTransitionsPerConnectionPerTick.value);
            hash.AddInteger(profile.maxCapturedObjectsPerTick.value);
            hash.AddInteger(profile.maxSerializedFieldsPerConnectionPerTick.value);
            hash.AddInteger(profile.maxGlobalInterestWorkPerTick.value);
            hash.AddInteger(profile.maxGlobalCaptureWorkPerTick.value);
            hash.AddInteger(profile.maxGlobalSchedulingWorkPerTick.value);
            hash.AddInteger(profile.targetBytesPerConnection.value);
            hash.AddInteger(profile.burstBytesPerConnection.value);
            hash.AddInteger(profile.maxMessagesPerConnectionPerTick.value);
            hash.AddInteger(profile.maxReliableQueuedBytesPerConnection.value);
            hash.AddInteger(profile.maxUnreliableQueuedBytesPerConnection.value);
            hash.AddInteger(profile.enterDwellTicks.value);
            hash.AddInteger(profile.exitDwellTicks.value);
            hash.AddInteger(profile.maxEligibleStarvationTicks.value);
            hash.AddInteger(profile.saturationGraceTicks.value);
        }

        [[nodiscard]] NetworkProjectSettingsFingerprint ComputeFingerprint(const NetworkProjectSettingsInput &input) {
            Foundation::StableHash64 hash;
            hash.AddInteger(input.contractVersion);
            hash.AddInteger(input.settings.Value());
            hash.AddInteger(input.revision.Value());
            hash.AddInteger(static_cast<std::uint32_t>(input.supportedRoles));
            hash.AddInteger(static_cast<std::uint8_t>(input.defaultRole));
            HashProfile(hash, input.profile);
            hash.AddInteger(input.protocol.protocol.Value());
            hash.AddInteger(input.protocol.supportedVersions.minimum.major);
            hash.AddInteger(input.protocol.supportedVersions.minimum.minor);
            hash.AddInteger(input.protocol.supportedVersions.maximum.major);
            hash.AddInteger(input.protocol.supportedVersions.maximum.minor);
            hash.AddInteger(input.protocol.schemaFingerprint);
            hash.AddInteger(static_cast<std::uint8_t>(input.transport.requirement));
            for (const bool required : input.transport.capabilities.requiredDelivery)
                hash.AddInteger(static_cast<std::uint8_t>(required));
            hash.AddInteger(input.transport.capabilities.requiredChannels);
            hash.AddInteger(input.transport.capabilities.requiredMaximumMessageBytes);
            hash.AddInteger(static_cast<std::uint8_t>(input.transport.capabilities.deadline));
            hash.AddInteger(input.transport.capabilities.requiredMaximumDeadlineMilliseconds);
            const auto endpointDiagnostic = input.defaultEndpoint.Diagnostic();
            const auto endpoint = input.defaultEndpoint.IsValid() ? endpointDiagnostic.View() : std::string_view{};
            hash.AddInteger(static_cast<std::uint16_t>(endpoint.size()));
            for (const char character : endpoint)
                hash.AddByte(static_cast<std::uint8_t>(character));
            hash.AddInteger(input.credentialRequirementId);
            const auto value = hash.Value() == 0 ? 1 : hash.Value();
            return NetworkProjectSettingsFingerprint::Create(value).Value();
        }

        [[nodiscard]] Result<void> ValidateInput(const NetworkProjectSettingsInput &input) {
            if (input.contractVersion != NetworkProjectSettingsInput::CurrentContractVersion || !input.settings.IsValid() ||
                !input.revision.IsValid() || !IsKnownRoleSet(input.supportedRoles) || !IsKnownRole(input.defaultRole) ||
                !ContainsNetworkProjectRole(input.supportedRoles, input.defaultRole) || !IsValidProtocol(input.protocol))
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));

            if (!IsValidTransportRequirements(input.transport))
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));

            if (!IsValidProfile(input.profile))
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));

            if (!IsProfileCapacityCoherent(input.profile))
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsCapacityExceeded));

            if (input.defaultEndpoint.Kind() != NetworkAddressKind::Count && !input.defaultEndpoint.IsValid())
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));
            if (input.supportedRoles == NetworkProjectRoleSet::Standalone && input.credentialRequirementId != 0)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsInvalid));

            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc NetworkProjectSettings::Create */
    Result<NetworkProjectSettings> NetworkProjectSettings::Create(const NetworkProjectSettingsInput &input) {
        if (const auto validation = ValidateInput(input); validation.HasError())
            return Result<NetworkProjectSettings>::Failure(validation.ErrorValue());
        return Result<NetworkProjectSettings>::Success(NetworkProjectSettings{input, ComputeFingerprint(input)});
    }

    /** @copydoc NetworkProjectSettings::Replace */
    Result<NetworkProjectSettings> NetworkProjectSettings::Replace(const NetworkProjectSettings &previous,
                                                                   const NetworkProjectSettingsInput &input) {
        if (input.settings != previous.Settings() || !input.revision.IsValid() || input.revision.Value() <= previous.Revision().Value())
            return Result<NetworkProjectSettings>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsStale));
        return Create(input);
    }

    /** @copydoc NetworkProjectSettings::NetworkProjectSettings */
    NetworkProjectSettings::NetworkProjectSettings(const NetworkProjectSettingsInput &input,
                                                   const NetworkProjectSettingsFingerprint fingerprint) noexcept
        : input_(input), fingerprint_(fingerprint) {}

    /** @copydoc NetworkProjectSettings::Settings */
    NetworkProjectSettingsId NetworkProjectSettings::Settings() const noexcept {
        return input_.settings;
    }

    /** @copydoc NetworkProjectSettings::Revision */
    NetworkProjectSettingsRevision NetworkProjectSettings::Revision() const noexcept {
        return input_.revision;
    }

    /** @copydoc NetworkProjectSettings::Fingerprint */
    NetworkProjectSettingsFingerprint NetworkProjectSettings::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc NetworkProjectSettings::ContractVersion */
    std::uint32_t NetworkProjectSettings::ContractVersion() const noexcept {
        return input_.contractVersion;
    }

    /** @copydoc NetworkProjectSettings::SupportedRoles */
    NetworkProjectRoleSet NetworkProjectSettings::SupportedRoles() const noexcept {
        return input_.supportedRoles;
    }

    /** @copydoc NetworkProjectSettings::DefaultRole */
    NetworkProjectRole NetworkProjectSettings::DefaultRole() const noexcept {
        return input_.defaultRole;
    }

    /** @copydoc NetworkProjectSettings::Profile */
    const NetworkProjectProfileV1 &NetworkProjectSettings::Profile() const noexcept {
        return input_.profile;
    }

    /** @copydoc NetworkProjectSettings::Protocol */
    const NetworkProjectProtocolPolicy &NetworkProjectSettings::Protocol() const noexcept {
        return input_.protocol;
    }

    /** @copydoc NetworkProjectSettings::Transport */
    const NetworkProjectTransportPolicy &NetworkProjectSettings::Transport() const noexcept {
        return input_.transport;
    }

    /** @copydoc NetworkProjectSettings::DefaultEndpoint */
    const NetworkAddress &NetworkProjectSettings::DefaultEndpoint() const noexcept {
        return input_.defaultEndpoint;
    }

    /** @copydoc NetworkProjectSettings::CredentialRequirementId */
    std::uint32_t NetworkProjectSettings::CredentialRequirementId() const noexcept {
        return input_.credentialRequirementId;
    }

    /** @copydoc NetworkProjectSettingsAuthority::Create */
    Result<NetworkProjectSettingsAuthority> NetworkProjectSettingsAuthority::Create(const NetworkProjectSettingsInput &input) {
        const auto settings = NetworkProjectSettings::Create(input);
        if (settings.HasError())
            return Result<NetworkProjectSettingsAuthority>::Failure(settings.ErrorValue());
        return Result<NetworkProjectSettingsAuthority>::Success(NetworkProjectSettingsAuthority{settings.Value()});
    }

    /** @copydoc NetworkProjectSettingsAuthority::NetworkProjectSettingsAuthority */
    NetworkProjectSettingsAuthority::NetworkProjectSettingsAuthority(NetworkProjectSettings settings) noexcept
        : settings_(std::move(settings)) {}

    /** @copydoc NetworkProjectSettingsAuthority::Apply */
    Result<NetworkProjectSettingsSnapshot> NetworkProjectSettingsAuthority::Apply(const NetworkProjectSettingsCommand &command) {
        if (lifecycle_ != NetworkProjectSettingsLifecycle::Active)
            return Result<NetworkProjectSettingsSnapshot>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsShuttingDown));
        if (command.expectedRevision != settings_.Revision())
            return Result<NetworkProjectSettingsSnapshot>::Failure(MakeError(NetworkErrors::NetworkProjectSettingsStale));

        const auto replacement = NetworkProjectSettings::Replace(settings_, command.candidate);
        if (replacement.HasError())
            return Result<NetworkProjectSettingsSnapshot>::Failure(replacement.ErrorValue());
        settings_ = replacement.Value();
        return Result<NetworkProjectSettingsSnapshot>::Success(Snapshot());
    }

    /** @copydoc NetworkProjectSettingsAuthority::Shutdown */
    void NetworkProjectSettingsAuthority::Shutdown() noexcept {
        lifecycle_ = NetworkProjectSettingsLifecycle::Closed;
    }

    /** @copydoc NetworkProjectSettingsAuthority::Lifecycle */
    NetworkProjectSettingsLifecycle NetworkProjectSettingsAuthority::Lifecycle() const noexcept {
        return lifecycle_;
    }

    /** @copydoc NetworkProjectSettingsAuthority::Snapshot */
    NetworkProjectSettingsSnapshot NetworkProjectSettingsAuthority::Snapshot() const {
        return NetworkProjectSettingsSnapshot{.settings = settings_};
    }
}  // namespace Horo::Network
