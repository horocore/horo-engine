#include "NetworkAdmissionWire.h"

namespace Horo::Network::AdmissionWire {
    namespace {
        template <class Identity> bool IdentityValue(Reader &reader, Identity &identity, const std::size_t width) {
            std::uint64_t raw{};
            if (!reader.Number(raw, width))
                return false;
            const auto value = Identity::Create(static_cast<typename Identity::ValueType>(raw));
            if (value.HasError())
                return false;
            identity = value.Value();
            return true;
        }

        // Authentication identities expose their fixed 64-bit representation without a ValueType alias.
        bool PolicyValue(Reader &reader, NetworkTrustPolicyId &identity) {
            std::uint64_t raw{};
            if (!reader.Number(raw, 8))
                return false;
            const auto value = NetworkTrustPolicyId::Create(raw);
            if (value.HasError())
                return false;
            identity = value.Value();
            return true;
        }

        bool Boolean(Reader &reader, bool &value) {
            std::uint8_t raw{};
            if (!reader.Number(raw, 1) || raw > 1)
                return false;
            value = raw != 0;
            return true;
        }

        bool Preamble(Reader &reader) {
            std::uint8_t version{};
            return reader.Number(version, 1) && version == Version;
        }

        template <std::size_t Size> bool Flags(Reader &reader, std::array<bool, Size> &values) {
            for (auto &value : values)
                if (!Boolean(reader, value))
                    return false;
            return true;
        }

        template <class T, std::size_t Size> bool Flags(Writer &writer, const std::array<T, Size> &values) {
            for (const auto value : values)
                if (!writer.Number(static_cast<std::uint8_t>(value), 1))
                    return false;
            return true;
        }

        bool Requirements(Reader &reader, TransportRequirements &value) {
            return Flags(reader, value.requiredDelivery) && reader.Number(value.requiredChannels, 4) &&
                   reader.Number(value.requiredMaximumMessageBytes, 8) && reader.Number(value.deadline, 1) &&
                   reader.Number(value.requiredMaximumDeadlineMilliseconds, 4);
        }

        bool Requirements(Writer &writer, const TransportRequirements &value) {
            return Flags(writer, value.requiredDelivery) && writer.Number(value.requiredChannels, 4) &&
                   writer.Number(value.requiredMaximumMessageBytes, 8) && writer.Number(static_cast<std::uint8_t>(value.deadline), 1) &&
                   writer.Number(value.requiredMaximumDeadlineMilliseconds, 4);
        }

        bool Capabilities(Reader &reader, TransportCapabilities &value) {
            if (!reader.Number(value.contractVersion, 4) || !reader.Number(value.revision, 8))
                return false;
            for (auto &delivery : value.delivery)
                if (!reader.Number(delivery, 1))
                    return false;
            return reader.Number(value.maximumChannels, 4) && reader.Number(value.maximumMessageBytes, 8) &&
                   reader.Number(value.deadlines, 1) && reader.Number(value.maximumDeadlineMilliseconds, 4) &&
                   ValidateTransportCapabilities(value);
        }

        bool Capabilities(Writer &writer, const TransportCapabilities &value) {
            return writer.Number(value.contractVersion, 4) && writer.Number(value.revision, 8) && Flags(writer, value.delivery) &&
                   writer.Number(value.maximumChannels, 4) && writer.Number(value.maximumMessageBytes, 8) &&
                   writer.Number(static_cast<std::uint8_t>(value.deadlines), 1) && writer.Number(value.maximumDeadlineMilliseconds, 4);
        }

        bool FeatureList(Reader &reader, std::span<ProtocolFeatureId> output, std::size_t &count) {
            std::uint8_t size{};
            if (!reader.Number(size, 1) || size > output.size())
                return false;
            count = size;
            for (std::size_t i = 0; i < count; ++i)
                if (!IdentityValue(reader, output[i], 2) || (i != 0 && output[i] <= output[i - 1]))
                    return false;
            return true;
        }

        bool FeatureList(Writer &writer, const std::span<const ProtocolFeatureId> values) {
            if (values.size() > MaximumHandshakeFeatures || !writer.Number(values.size(), 1))
                return false;
            for (const auto value : values)
                if (!writer.Number(value.Value(), 2))
                    return false;
            return true;
        }

        bool Selection(Reader &reader, HandshakeSelection &value) {
            if (!IdentityValue(reader, value.protocol, 2) || !reader.Number(value.version.major, 2) ||
                !reader.Number(value.version.minor, 2) || !reader.Number(value.schemaFingerprint, 8) ||
                !FeatureList(reader, value.features.values, value.features.count) || !reader.Number(value.compression, 1))
                return false;
            auto &transport = value.transport;
            return reader.Number(transport.capabilityRevision, 8) && Flags(reader, transport.admittedDelivery) &&
                   reader.Number(transport.channelCount, 4) && reader.Number(transport.maximumMessageBytes, 8) &&
                   Boolean(reader, transport.deadlinesEnabled) && reader.Number(transport.maximumDeadlineMilliseconds, 4);
        }

        bool Selection(Writer &writer, const HandshakeSelection &value) {
            if (!writer.Number(value.protocol.Value(), 2) || !writer.Number(value.version.major, 2) ||
                !writer.Number(value.version.minor, 2) || !writer.Number(value.schemaFingerprint, 8) ||
                !FeatureList(writer, value.features.Values()) || !writer.Number(static_cast<std::uint8_t>(value.compression), 1))
                return false;
            const auto &transport = value.transport;
            return writer.Number(transport.capabilityRevision, 8) && Flags(writer, transport.admittedDelivery) &&
                   writer.Number(transport.channelCount, 4) && writer.Number(transport.maximumMessageBytes, 8) &&
                   writer.Number(transport.deadlinesEnabled ? 1 : 0, 1) && writer.Number(transport.maximumDeadlineMilliseconds, 4);
        }
    }  // namespace

    /** @copydoc ReadOffer */
    bool ReadOffer(const std::span<const std::byte> bytes, Offer &offer) {
        Reader reader{bytes};
        auto &value = offer.compatibility;
        if (!Preamble(reader) || !reader.Number(value.contractVersion, 4) || !IdentityValue(reader, value.protocol, 2) ||
            !reader.Number(value.versions.minimum.major, 2) || !reader.Number(value.versions.minimum.minor, 2) ||
            !reader.Number(value.versions.maximum.major, 2) || !reader.Number(value.versions.maximum.minor, 2) ||
            !reader.Number(value.schemaFingerprint, 8))
            return false;
        if (!FeatureList(reader, offer.supported, offer.supportedCount) || !FeatureList(reader, offer.required, offer.requiredCount) ||
            !Flags(reader, value.compression) || !reader.Number(value.requiredCompression, 1) || !reader.Bytes(offer.nonce))
            return false;
        return Requirements(reader, offer.requirements) && Capabilities(reader, offer.capabilities) && reader.Done();
    }

    /** @copydoc WriteOffer */
    bool WriteOffer(Writer &writer, const NetworkAdmissionPolicy &policy, const AuthenticationNonce &nonce) {
        const auto &value = policy.local.compatibility;
        if (!writer.Number(Version, 1) || !writer.Number(value.contractVersion, 4) || !writer.Number(value.protocol.Value(), 2) ||
            !writer.Number(value.versions.minimum.major, 2) || !writer.Number(value.versions.minimum.minor, 2) ||
            !writer.Number(value.versions.maximum.major, 2) || !writer.Number(value.versions.maximum.minor, 2) ||
            !writer.Number(value.schemaFingerprint, 8))
            return false;
        return FeatureList(writer, value.features.supported) && FeatureList(writer, value.features.required) &&
               Flags(writer, value.compression) && writer.Number(static_cast<std::uint8_t>(value.requiredCompression), 1) &&
               writer.Bytes(nonce) && Requirements(writer, policy.requirements) && Capabilities(writer, policy.local.transport);
    }

    /** @copydoc ReadChallenge */
    bool ReadChallenge(const std::span<const std::byte> bytes, Challenge &value) {
        Reader reader{bytes};
        auto &challenge = value.challenge;
        return Preamble(reader) && Selection(reader, value.selection) && PolicyValue(reader, challenge.policy) &&
               reader.Number(challenge.policyRevision, 8) && reader.Bytes(challenge.transcriptDigest) &&
               reader.Bytes(challenge.clientNonce) && reader.Bytes(challenge.serverNonce) && reader.Done();
    }

    /** @copydoc WriteChallenge */
    bool WriteChallenge(Writer &writer, const Challenge &value) {
        const auto &challenge = value.challenge;
        return writer.Number(Version, 1) && Selection(writer, value.selection) && writer.Number(challenge.policy.Value(), 8) &&
               writer.Number(challenge.policyRevision, 8) && writer.Bytes(challenge.transcriptDigest) &&
               writer.Bytes(challenge.clientNonce) && writer.Bytes(challenge.serverNonce);
    }

    /** @copydoc ReadProof */
    bool ReadProof(const std::span<const std::byte> bytes, Proof &value, const std::size_t maximumProofBytes) {
        Reader reader{bytes};
        std::uint16_t proofBytes{};
        auto &response = value.response;
        return Preamble(reader) && PolicyValue(reader, response.policy) && reader.Number(response.policyRevision, 8) &&
               reader.Bytes(response.transcriptDigest) && reader.Bytes(value.clientNonce) && reader.Bytes(value.serverNonce) &&
               reader.Number(proofBytes, 2) && proofBytes != 0 && proofBytes <= maximumProofBytes &&
               reader.Borrow(proofBytes, response.proof) && reader.Done();
    }

    /** @copydoc WriteProof */
    bool WriteProof(Writer &writer, const Proof &value) {
        const auto &response = value.response;
        if (response.proof.empty() || response.proof.size() > MaximumAuthenticationProofBytes)
            return false;
        return writer.Number(Version, 1) && writer.Number(response.policy.Value(), 8) && writer.Number(response.policyRevision, 8) &&
               writer.Bytes(response.transcriptDigest) && writer.Bytes(value.clientNonce) && writer.Bytes(value.serverNonce) &&
               writer.Number(response.proof.size(), 2) && writer.Bytes(response.proof);
    }

    /** @copydoc ReadAcknowledgement */
    bool ReadAcknowledgement(const std::span<const std::byte> bytes, Acknowledgement &value) {
        Reader reader{bytes};
        return Preamble(reader) && Boolean(reader, value.response) && reader.Bytes(value.transcript) && reader.Bytes(value.binding) &&
               reader.Bytes(value.clientNonce) && reader.Bytes(value.serverNonce) && reader.Done();
    }

    /** @copydoc WriteAcknowledgement */
    bool WriteAcknowledgement(Writer &writer, const Acknowledgement &value) {
        return writer.Number(Version, 1) && writer.Number(value.response ? 1 : 0, 1) && writer.Bytes(value.transcript) &&
               writer.Bytes(value.binding) && writer.Bytes(value.clientNonce) && writer.Bytes(value.serverNonce);
    }
}  // namespace Horo::Network::AdmissionWire
