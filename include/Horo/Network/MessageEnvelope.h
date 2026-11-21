#pragma once

/**
 * @file MessageEnvelope.h
 * @brief Bounded canonical message framing values and explicit admission state.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ProtocolIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Horo::Network {
    inline constexpr std::size_t MessageEnvelopeFixedHeaderBytes = 32;

    /** @brief Stable non-zero 16-bit identity for an envelope extension field. */
    class MessageEnvelopeFieldId final {
    public:
        using ValueType = std::uint16_t;

        constexpr MessageEnvelopeFieldId() = default;

        /** @brief Validates a raw extension identity. @param value Non-zero wire value. @return Typed ID or IdentityInvalid. */
        [[nodiscard]] static Result<MessageEnvelopeFieldId> Create(const ValueType value) {
            if (value == 0)
                return Result<MessageEnvelopeFieldId>::Failure(MakeError(NetworkErrors::IdentityInvalid));
            return Result<MessageEnvelopeFieldId>::Success(MessageEnvelopeFieldId{value});
        }

        /** @brief Returns the exact 16-bit wire value. @return Zero only for the invalid identity. */
        [[nodiscard]] constexpr ValueType Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation only. @return Whether the field identity is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

        constexpr auto operator<=>(const MessageEnvelopeFieldId &) const noexcept = default;

    private:
        explicit constexpr MessageEnvelopeFieldId(const ValueType value) noexcept : value_(value) {}

        ValueType value_{};
    };

    /** @brief Wire requirement for a length-delimited extension field. */
    enum class MessageEnvelopeFieldRequirement : std::uint8_t {
        Optional,
        Required,
        Count
    };

    /** @brief One owned envelope extension; fields encode in ascending ID order. */
    struct MessageEnvelopeField final {
        MessageEnvelopeFieldId id;                                                              /**< Stable field identity. */
        MessageEnvelopeFieldRequirement requirement{MessageEnvelopeFieldRequirement::Optional}; /**< Unknown-field behavior. */
        std::vector<std::byte> value; /**< Bounded opaque canonical field bytes. */

        auto operator<=>(const MessageEnvelopeField &) const = default;
    };

    /** @brief Explicit 32-bit wire counter with checked non-wrapping advancement. */
    template <typename Tag> class MessageWireCounter final {
    public:
        using ValueType = std::uint32_t;

        constexpr MessageWireCounter() = default;

        explicit constexpr MessageWireCounter(const ValueType value) noexcept : value_(value) {}

        /** @brief Returns the exact 32-bit wire value. @return Counter representation. */
        [[nodiscard]] constexpr ValueType Value() const noexcept {
            return value_;
        }

        /** @brief Advances without wrapping. @return Next value or MessageCounterExhausted at UINT32_MAX. */
        [[nodiscard]] Result<MessageWireCounter> Next() const {
            if (value_ == std::numeric_limits<ValueType>::max())
                return Result<MessageWireCounter>::Failure(MakeError(NetworkErrors::MessageCounterExhausted));
            return Result<MessageWireCounter>::Success(MessageWireCounter{value_ + 1U});
        }

        constexpr auto operator<=>(const MessageWireCounter &) const noexcept = default;

    private:
        ValueType value_{};
    };

    struct MessageSequenceTag;
    struct MessageAcknowledgementTag;
    using MessageSequenceNumber = MessageWireCounter<MessageSequenceTag>;
    using MessageAcknowledgementNumber = MessageWireCounter<MessageAcknowledgementTag>;

    /** @brief Owned decoded envelope; no span refers to the input frame or registry. */
    struct MessageEnvelope final {
        ProtocolId protocol;                          /**< Stable protocol identity. */
        MessageTypeId message;                        /**< Stable message identity within the protocol. */
        MessageSchemaId schema;                       /**< Stable payload schema identity. */
        MessageSchemaVersion schemaVersion;           /**< Exact payload schema version. */
        MessageSequenceNumber sequence;               /**< Non-wrapping sender sequence counter. */
        MessageAcknowledgementNumber acknowledgement; /**< Non-wrapping peer acknowledgement counter. */
        std::vector<MessageEnvelopeField> fields;     /**< Known canonical extension fields. */
        std::vector<std::byte> payload;               /**< Bounded owned payload bytes. */

        auto operator<=>(const MessageEnvelope &) const = default;
    };

    /** @brief Independent hard bounds validated before frame allocation or payload copy. */
    struct MessageEnvelopeLimits final {
        std::size_t maximumFrameBytes{1024 * 1024};
        std::size_t maximumPayloadBytes{1024 * 1024 - 32};
        std::size_t maximumFields{32};
        std::size_t maximumExtensionBytes{4096};
    };

    /** @brief Caller-owned pure admission state; no runtime/session object is consulted. */
    enum class MessageEnvelopeAdmissionState : std::uint8_t {
        Accepting,
        Cancelled,
        TimedOut,
        ShuttingDown,
        Count
    };
}  // namespace Horo::Network
