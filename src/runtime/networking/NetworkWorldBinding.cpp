#include "Horo/Network/NetworkWorldBinding.h"

#include "NetworkAdmissionWire.h"

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
    }  // namespace

    /** @copydoc ValidateNetworkWorldBindingPolicy */
    Result<void> ValidateNetworkWorldBindingPolicy(const NetworkWorldBindingPolicy &policy, const HandshakeLocalDescriptor &local,
                                                   const MessageCodecRegistry &codecs) {
        const auto &identity = policy.control;
        const auto codec = codecs.FindCodec(identity.protocol, identity.id);
        if (!identity.protocol.IsValid() || !identity.id.IsValid() || !identity.schema.IsValid() || !identity.version.IsValid() ||
            identity.optional || !policy.feature.IsValid() || !policy.expectedServer.IsValid() || !policy.expectedProvenance.IsValid() ||
            !policy.authorityPublisher.IsValid() || identity.protocol != local.compatibility.protocol ||
            std::ranges::find(local.compatibility.features.required, policy.feature) == local.compatibility.features.required.end() ||
            !codec || codec->schema != identity.schema || !codec->supportedVersions.Contains(identity.version) ||
            codec->maximumPayloadBytes != NetworkWorldBindingWireBytes)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        return Result<void>::Success();
    }

    /** @copydoc EncodeNetworkWorldBinding */
    Result<NetworkWorldBindingBytes> EncodeNetworkWorldBinding(const NetworkWorldBindingRecord &record) {
        if (!ValidRecord(record))
            return Result<NetworkWorldBindingBytes>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        NetworkWorldBindingBytes bytes{};
        AdmissionWire::Writer writer{bytes};
        if (!writer.Number(WireVersion, 2) || !writer.Number(static_cast<std::uint16_t>(record.kind), 2) ||
            !writer.Number(record.worldSession.Value(), 8) || !writer.Number(record.remoteEpoch.Value(), 8) ||
            !writer.Number(record.schemaFingerprint, 8) || !writer.Bytes(std::as_bytes(std::span{record.expectedPriorBinding.bytes})))
            return Result<NetworkWorldBindingBytes>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        return Result<NetworkWorldBindingBytes>::Success(bytes);
    }

    /** @copydoc DecodeNetworkWorldBinding */
    Result<NetworkWorldBindingRecord> DecodeNetworkWorldBinding(const std::span<const std::byte> bytes) {
        AdmissionWire::Reader reader{bytes};
        NetworkWorldBindingRecord record;
        std::uint16_t version{}, kind{};
        std::uint64_t session{}, epoch{};
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
