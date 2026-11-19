#include "Horo/Network/NetworkWorldBinding.h"

#include "Horo/Network/NetworkPrefabLifecycle.h"
#include "NetworkAdmissionWire.h"
#include "NetworkWorldBindingState.h"

#include <algorithm>

namespace Horo::Network {
    namespace {
        constexpr std::uint16_t WireVersion = 1;

        /** @brief Check closed representation and distinguish initial publication from replacement. */
        bool ValidRecord(const NetworkWorldBindingRecord &record) {
            const bool emptyPrior = std::ranges::all_of(record.expectedPriorBinding.bytes, [](const std::uint8_t value) {
                return value == 0;
            });
            return record.kind < NetworkWorldBindingKind::Count && record.worldSession.IsValid() && record.remoteEpoch.IsValid() &&
                   record.schemaFingerprint != 0 && (record.kind == NetworkWorldBindingKind::Initial) == emptyPrior;
        }

        /** @brief Checks an extension Control identity and its bounded codec contract. */
        bool ValidExtensionRoute(const NetworkWorldBindingPolicy &policy, const MessageIdentityDescriptor &identity,
                                 ProtocolFeatureId feature, const MessageCodecRegistry &codecs, std::size_t minimum, std::size_t maximum) {
            if (identity.protocol != policy.control.protocol || identity.id == policy.control.id || identity.optional || !feature.IsValid())
                return false;
            const auto codec = codecs.FindCodec(identity.protocol, identity.id);
            return codec && codec->schema == identity.schema && codec->supportedVersions.Contains(identity.version) &&
                   codec->maximumPayloadBytes >= minimum && codec->maximumPayloadBytes <= maximum;
        }

        /** @brief Initial fields require their own negotiated feature and message identity. */
        bool DistinctInitialRoute(const NetworkInitialStatePolicy &initial, const MessageIdentityDescriptor &control,
                                  ProtocolFeatureId feature) {
            return initial.control.id != control.id && initial.feature != feature;
        }
    }  // namespace

    /** @copydoc ValidPrefabLifecycleRoute */
    bool ValidPrefabLifecycleRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs) {
        if (!policy.prefabLifecycle)
            return true;
        const auto &route = *policy.prefabLifecycle;
        return ValidExtensionRoute(policy, route.control, route.feature, codecs, MinimumNetworkPrefabSpawnBytes,
                                   MaximumNetworkPrefabLifecycleBytes);
    }

    /** @copydoc ValidAuthoredSceneRoute */
    bool ValidAuthoredSceneRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs) {
        if (!policy.authoredScene)
            return true;
        const auto &route = *policy.authoredScene;
        if (policy.prefabLifecycle && route.control.id == policy.prefabLifecycle->control.id)
            return false;
        return ValidExtensionRoute(policy, route.control, route.feature, codecs,
                                   MinimumNetworkAuthoredScenePageBytes + NetworkAuthoredSceneRowBytes,
                                   MaximumNetworkAuthoredScenePageBytes);
    }

    /** @copydoc ValidInitialStateRoute */
    bool ValidInitialStateRoute(const NetworkWorldBindingPolicy &policy, const MessageCodecRegistry &codecs) {
        if (!policy.initialState)
            return true;
        if (!policy.authoredScene || !policy.prefabLifecycle)
            return false;
        const auto &route = *policy.initialState;
        if (!DistinctInitialRoute(route, policy.control, policy.feature) ||
            !DistinctInitialRoute(route, policy.authoredScene->control, policy.authoredScene->feature) ||
            !DistinctInitialRoute(route, policy.prefabLifecycle->control, policy.prefabLifecycle->feature))
            return false;
        if (route.maximumWireBytes == 0 || route.maximumWireBytes > MaximumNetworkInitialStateWireBytes)
            return false;
        return ValidExtensionRoute(policy, route.control, route.feature, codecs, NetworkInitialStateSegmentHeaderBytes + 1,
                                   MaximumNetworkInitialStateFrameBytes);
    }

    /** @copydoc WorldBindingFeaturesSupported */
    bool WorldBindingFeaturesSupported(const NetworkWorldBindingPolicy &policy, std::span<const ProtocolFeatureId> features) {
        const auto supports = [features](ProtocolFeatureId feature) {
            return std::ranges::find(features, feature) != features.end();
        };
        if (!supports(policy.feature) || (policy.authoredScene && !supports(policy.authoredScene->feature)))
            return false;
        return !policy.initialState ||
               (policy.prefabLifecycle && supports(policy.prefabLifecycle->feature) && supports(policy.initialState->feature));
    }

    /** @copydoc ValidateNetworkWorldBindingPolicy */
    Result<void> ValidateNetworkWorldBindingPolicy(const NetworkWorldBindingPolicy &policy, const HandshakeLocalDescriptor &local,
                                                   const MessageCodecRegistry &codecs) {
        const auto &identity = policy.control;
        if (const auto codec = codecs.FindCodec(identity.protocol, identity.id);
            !identity.protocol.IsValid() || !identity.id.IsValid() || !identity.schema.IsValid() || !identity.version.IsValid() ||
            identity.optional || !policy.feature.IsValid() || !policy.expectedServer.IsValid() || !policy.expectedProvenance.IsValid() ||
            !policy.authorityPublisher.IsValid() || identity.protocol != local.compatibility.protocol || !codec ||
            codec->schema != identity.schema || !codec->supportedVersions.Contains(identity.version) ||
            codec->maximumPayloadBytes != NetworkWorldBindingWireBytes)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        if (!ValidPrefabLifecycleRoute(policy, codecs) || !ValidAuthoredSceneRoute(policy, codecs) ||
            !ValidInitialStateRoute(policy, codecs))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        if (!WorldBindingFeaturesSupported(policy, local.compatibility.features.required) ||
            (policy.prefabLifecycle && std::ranges::find(local.compatibility.features.required, policy.prefabLifecycle->feature) ==
                                           local.compatibility.features.required.end()))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        return Result<void>::Success();
    }

    /** @copydoc EncodeNetworkWorldBinding */
    Result<NetworkWorldBindingBytes> EncodeNetworkWorldBinding(const NetworkWorldBindingRecord &record) {
        if (!ValidRecord(record))
            return Result<NetworkWorldBindingBytes>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        NetworkWorldBindingBytes bytes{};
        if (AdmissionWire::Writer writer{bytes};
            !writer.Number(WireVersion, 2) || !writer.Number(static_cast<std::uint16_t>(record.kind), 2) ||
            !writer.Number(record.worldSession.Value(), 8) || !writer.Number(record.remoteEpoch.Value(), 8) ||
            !writer.Number(record.schemaFingerprint, 8) || !writer.Bytes(std::as_bytes(std::span{record.expectedPriorBinding.bytes})))
            return Result<NetworkWorldBindingBytes>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        return Result<NetworkWorldBindingBytes>::Success(bytes);
    }

    /** @copydoc DecodeNetworkWorldBinding */
    Result<NetworkWorldBindingRecord> DecodeNetworkWorldBinding(const std::span<const std::byte> bytes) {
        AdmissionWire::Reader reader{bytes};
        NetworkWorldBindingRecord record;
        std::uint16_t version{};
        std::uint16_t kind{};
        std::uint64_t session{};
        std::uint64_t epoch{};
        if (bytes.size() != NetworkWorldBindingWireBytes || !reader.Number(version, 2) || version != WireVersion ||
            !reader.Number(kind, 2) || kind >= static_cast<std::uint16_t>(NetworkWorldBindingKind::Count) || !reader.Number(session, 8) ||
            !reader.Number(epoch, 8) || !reader.Number(record.schemaFingerprint, 8) ||
            !reader.Bytes(std::as_writable_bytes(std::span{record.expectedPriorBinding.bytes})) || !reader.Done())
            return Result<NetworkWorldBindingRecord>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        const auto worldSession = NetworkSessionGeneration::Create(session);
        const auto authority = ReplicationAuthorityEpoch::Create(epoch);
        if (worldSession.HasError() || authority.HasError())
            return Result<NetworkWorldBindingRecord>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        record.kind = static_cast<NetworkWorldBindingKind>(kind);
        record.worldSession = worldSession.Value();
        record.remoteEpoch = authority.Value();
        if (!ValidRecord(record))
            return Result<NetworkWorldBindingRecord>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        return Result<NetworkWorldBindingRecord>::Success(record);
    }

    /** @copydoc NetworkWorldBindingDigest */
    Result<Sha256Digest> NetworkWorldBindingDigest(const NetworkWorldBindingRecord &record) {
        auto encoded = EncodeNetworkWorldBinding(record);
        if (encoded.HasError())
            return Result<Sha256Digest>::Failure(encoded.ErrorValue());
        // Phase/prior metadata describes a transition, not the occurrence. A newly
        // authenticated peer observing an already-traveled world gets the SAME
        // occurrence identity as peers that received its original COMMIT.
        constexpr std::array domain{std::byte{'H'}, std::byte{'R'}, std::byte{'W'}, std::byte{'B'},
                                    std::byte{0},   std::byte{0},   std::byte{0},   std::byte{1}};
        const std::array<std::span<const std::byte>, 2> fragments{domain, std::span{encoded.Value()}.subspan(4, 24)};
        return Result<Sha256Digest>::Success(ComputeSha256Fragments(fragments));
    }
}  // namespace Horo::Network
