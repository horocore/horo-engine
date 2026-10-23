#pragma once

/**
 * @file MessageCodecRegistry.h
 * @brief Inert payload codec metadata and bounded canonical message envelope codec.
 */

#include "Horo/Network/MessageEnvelope.h"
#include "Horo/Network/ProtocolIdentityRegistry.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::Network {
    /** @brief Metadata-only payload codec capability; contains no callback or runtime registration. */
    struct MessageCodecDescriptor final {
        ProtocolId protocol;                         /**< Owning protocol. */
        MessageTypeId message;                       /**< Message resolved by this codec capability. */
        MessageSchemaId schema;                      /**< Required payload schema identity. */
        MessageSchemaVersionRange supportedVersions; /**< Explicit decodable version interval. */
        std::size_t maximumPayloadBytes{};           /**< Codec-specific payload bound. */

        constexpr auto operator<=>(const MessageCodecDescriptor &) const noexcept = default;
    };

    /** @brief Metadata for one understood singleton envelope extension scoped to a message. */
    struct MessageEnvelopeFieldDescriptor final {
        ProtocolId protocol;               /**< Owning protocol. */
        MessageTypeId message;             /**< Owning message. */
        MessageEnvelopeFieldId id;         /**< Stable extension identity. */
        std::size_t maximumEncodedBytes{}; /**< Independent field-value bound. */

        constexpr auto operator<=>(const MessageEnvelopeFieldDescriptor &) const noexcept = default;
    };

    /** @brief Hard construction bounds for an immutable codec metadata snapshot. */
    struct MessageCodecRegistryLimits final {
        std::size_t maximumCodecs{1024};
        std::size_t maximumFields{4096};
        std::size_t maximumPayloadBytes{1024 * 1024};
        std::size_t maximumFieldBytes{4096};
    };

    /** @brief Borrowed contributions copied synchronously; backing storage need not outlive Create. */
    struct MessageCodecContributions final {
        std::span<const MessageCodecDescriptor> codecs;
        std::span<const MessageEnvelopeFieldDescriptor> fields;
    };

    /** @brief Owned canonical inert codec metadata snapshot with value-returning lookups. */
    class MessageCodecRegistry final {
    public:
        /**
         * @brief Copies and validates codec metadata against stable protocol identities.
         * @param contributions Borrowed metadata valid for this call only.
         * @param identities Stable identity registry used synchronously for cross-reference validation.
         * @param limits Non-zero construction and value bounds.
         * @return Owned immutable metadata or a typed validation/conflict/capacity error.
         */
        [[nodiscard]] static Result<MessageCodecRegistry> Create(const MessageCodecContributions &contributions,
                                                                 const ProtocolIdentityRegistry &identities,
                                                                 const MessageCodecRegistryLimits &limits = {});
        /** @brief Resolves payload codec metadata. @param protocol Protocol ID. @param message Message ID. @return Owned descriptor when
         * known. */
        [[nodiscard]] std::optional<MessageCodecDescriptor> FindCodec(ProtocolId protocol, MessageTypeId message) const noexcept;
        /** @brief Resolves extension metadata. @param protocol Protocol ID. @param message Message ID. @param field Field ID. @return Owned
         * descriptor when known. */
        [[nodiscard]] std::optional<MessageEnvelopeFieldDescriptor> FindField(ProtocolId protocol, MessageTypeId message,
                                                                              MessageEnvelopeFieldId field) const noexcept;

    private:
        std::vector<MessageCodecDescriptor> codecs_;
        std::vector<MessageEnvelopeFieldDescriptor> fields_;
    };

    /**
     * @brief Canonically encodes one owned envelope after complete bounded validation.
     * @param envelope Value to encode; field input order does not affect bytes.
     * @param registry Inert codec and field metadata.
     * @param limits Independent frame, payload, count, and extension bounds.
     * @param state Caller-owned cancellation/shutdown admission state.
     * @return Canonical big-endian frame or a typed safe error.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeMessageEnvelope(
        const MessageEnvelope &envelope, const MessageCodecRegistry &registry, const MessageEnvelopeLimits &limits = {},
        MessageEnvelopeAdmissionState state = MessageEnvelopeAdmissionState::Accepting);

    /**
     * @brief Validated envelope metadata and a synchronous borrow of its payload.
     * @details The input frame must remain alive and unchanged until this view is discarded.
     * All extension fields are validated, including unknown required fields, without copying
     * their values. This view retains no credentials or ownership of the input storage.
     */
    struct MessageEnvelopePayloadView final {
        ProtocolId protocol;
        MessageTypeId message;
        MessageSchemaId schema;
        MessageSchemaVersion schemaVersion;
        MessageSequenceNumber sequence;
        MessageAcknowledgementNumber acknowledgement;
        std::size_t fieldCount{};
        std::span<const std::byte> payload;
    };

    /**
     * @brief Encode an extension-free payload borrow into caller-owned bounded storage.
     * @param envelope Typed metadata and synchronous payload borrow; fieldCount must be zero.
     * @param registry Canonical immutable product codec inventory.
     * @param output Caller-owned writable storage; no payload copy is retained by this function.
     * @param limits Independent complete-frame and payload bounds.
     * @return Encoded prefix length, or typed framing/schema/capacity error before writing.
     * @details Intended for securely cleared control-frame storage. Input/output must not overlap.
     */
    [[nodiscard]] Result<std::size_t> EncodeMessageEnvelopePayloadInto(const MessageEnvelopePayloadView &envelope,
                                                                       const MessageCodecRegistry &registry, std::span<std::byte> output,
                                                                       const MessageEnvelopeLimits &limits = {});

    /**
     * @brief Validate the complete canonical envelope without allocating or copying payload bytes.
     * @param frame Borrowed complete frame; backing bytes must outlive the returned view.
     * @param registry Inert codec and field metadata.
     * @param limits Independent frame, payload, count and extension bounds.
     * @param state Explicit caller-owned lifecycle admission state.
     * @return Synchronous payload borrow or the same typed framing/schema/bounds denial as DecodeMessageEnvelope.
     */
    [[nodiscard]] Result<MessageEnvelopePayloadView> InspectMessageEnvelope(
        std::span<const std::byte> frame, const MessageCodecRegistry &registry, const MessageEnvelopeLimits &limits = {},
        MessageEnvelopeAdmissionState state = MessageEnvelopeAdmissionState::Accepting);

    /**
     * @brief Decodes a complete canonical frame after validating every length before allocation.
     * @param frame Borrowed complete frame valid for this call only.
     * @param registry Inert codec and field metadata.
     * @param limits Independent frame, payload, count, and extension bounds.
     * @param state Caller-owned cancellation/shutdown admission state.
     * @return Owned envelope; unknown optional fields are skipped, while unknown required fields fail.
     */
    [[nodiscard]] Result<MessageEnvelope> DecodeMessageEnvelope(
        std::span<const std::byte> frame, const MessageCodecRegistry &registry, const MessageEnvelopeLimits &limits = {},
        MessageEnvelopeAdmissionState state = MessageEnvelopeAdmissionState::Accepting);
}  // namespace Horo::Network
