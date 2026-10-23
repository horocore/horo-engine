#pragma once
/** @file NetworkAdmissionWire.h @brief Private closed v1 admission payload encoding above the canonical envelope. */
#include "Horo/Network/NetworkAdmissionController.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace Horo::Network::AdmissionWire {
    inline constexpr std::size_t MaximumFrameBytes = 8192;
    inline constexpr std::uint8_t Version = 1;

    /** @brief Fixed-storage hostile offer; views are rebound only for synchronous negotiation. */
    struct Offer final {
        HandshakeCompatibilityDescriptor compatibility;
        TransportRequirements requirements;
        TransportCapabilities capabilities;
        AuthenticationNonce nonce{};
        std::array<ProtocolFeatureId, MaximumHandshakeFeatures> supported{};
        std::array<ProtocolFeatureId, MaximumHandshakeFeatures> required{};
        std::size_t supportedCount{};
        std::size_t requiredCount{};

        [[nodiscard]] HandshakeOffer View() const noexcept {
            auto descriptor = compatibility;
            descriptor.features = {std::span{supported}.first(supportedCount), std::span{required}.first(requiredCount)};
            return {descriptor, requirements};
        }
    };

    /** @brief Only public selection/challenge fields are encoded; process-local fences remain absent. */
    struct Challenge final {
        HandshakeSelection selection;
        AuthenticationChallenge challenge;
    };

    /** @brief Proof borrows the raw transport frame only until synchronous credential verification returns. */
    struct Proof final {
        AuthenticationResponseView response;
        AuthenticationNonce clientNonce{};
        AuthenticationNonce serverNonce{};
    };

    /** @brief Protected channel proposal/ack echo; no principal or active-state grant appears on wire. */
    struct Acknowledgement final {
        bool response{};
        AuthenticationDigest transcript{};
        AuthenticationDigest binding{};
        AuthenticationNonce clientNonce{};
        AuthenticationNonce serverNonce{};
    };

    /** @brief Bounded big-endian payload reader; a failed read never advances beyond its input. */
    class Reader final {
    public:
        explicit Reader(std::span<const std::byte> input) : input_(input) {}

        [[nodiscard]] bool Number(std::uint64_t &value, const std::size_t width) noexcept {
            if (width > 8 || width > input_.size() - offset_)
                return false;
            value = 0;
            for (std::size_t i = 0; i < width; ++i)
                value = (value << 8U) | std::to_integer<std::uint8_t>(input_[offset_++]);
            return true;
        }

        template <class T> [[nodiscard]] bool Number(T &value, const std::size_t width) noexcept {
            std::uint64_t raw{};
            if (!Number(raw, width))
                return false;
            value = static_cast<T>(raw);
            return true;
        }

        [[nodiscard]] bool Bytes(const std::span<std::byte> output) noexcept {
            if (output.size() > input_.size() - offset_)
                return false;
            std::ranges::copy(input_.subspan(offset_, output.size()), output.begin());
            offset_ += output.size();
            return true;
        }

        [[nodiscard]] bool Borrow(const std::size_t size, std::span<const std::byte> &output) noexcept {
            if (size > input_.size() - offset_)
                return false;
            output = input_.subspan(offset_, size);
            offset_ += size;
            return true;
        }

        [[nodiscard]] bool Done() const noexcept {
            return offset_ == input_.size();
        }

    private:
        std::span<const std::byte> input_;
        std::size_t offset_{};
    };

    /** @brief Checked caller-owned payload writer; caller securely clears its backing storage after send. */
    class Writer final {
    public:
        explicit Writer(std::span<std::byte> output) : output_(output) {}

        [[nodiscard]] bool Number(const std::uint64_t value, const std::size_t width) noexcept {
            if (width > 8 || width > output_.size() - offset_)
                return false;
            for (std::size_t i = 0; i < width; ++i)
                output_[offset_++] = static_cast<std::byte>(value >> ((width - i - 1) * 8));
            return true;
        }

        [[nodiscard]] bool Bytes(const std::span<const std::byte> value) noexcept {
            if (value.size() > output_.size() - offset_)
                return false;
            std::ranges::copy(value, output_.begin() + static_cast<std::ptrdiff_t>(offset_));
            offset_ += value.size();
            return true;
        }

        [[nodiscard]] std::span<const std::byte> Written() const noexcept {
            return output_.first(offset_);
        }

    private:
        std::span<std::byte> output_;
        std::size_t offset_{};
    };

    /** @brief Decode and validate one closed offer without allocation or borrowed retained feature storage. */
    [[nodiscard]] bool ReadOffer(std::span<const std::byte> bytes, Offer &offer);
    /** @brief Encode the host policy and fresh local nonce into bounded storage. */
    [[nodiscard]] bool WriteOffer(Writer &writer, const NetworkAdmissionPolicy &policy, const AuthenticationNonce &nonce);
    /** @brief Decode public challenge/selection fields with no local handle interpretation. */
    [[nodiscard]] bool ReadChallenge(std::span<const std::byte> bytes, Challenge &challenge);
    /** @brief Encode public challenge/selection fields, omitting local handles and session generations. */
    [[nodiscard]] bool WriteChallenge(Writer &writer, const Challenge &challenge);
    /** @brief Validate response framing and borrow proof bytes synchronously. */
    [[nodiscard]] bool ReadProof(std::span<const std::byte> bytes, Proof &proof, std::size_t maximumProofBytes);
    /** @brief Encode a securely owned proof borrow without copying it into an ordinary decoded vector. */
    [[nodiscard]] bool WriteProof(Writer &writer, const Proof &proof);
    /** @brief Decode a canonical proposal/ack echo. */
    [[nodiscard]] bool ReadAcknowledgement(std::span<const std::byte> bytes, Acknowledgement &ack);
    /** @brief Encode a canonical proposal/ack echo. */
    [[nodiscard]] bool WriteAcknowledgement(Writer &writer, const Acknowledgement &ack);
}  // namespace Horo::Network::AdmissionWire
