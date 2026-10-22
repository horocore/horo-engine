#include "Horo/Network/HandshakeNegotiation.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Network {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr std::size_t CompressionIndex(const HandshakeCompression compression) noexcept {
            return static_cast<std::size_t>(compression);
        }

        [[nodiscard]] bool HasCompression(const std::array<bool, static_cast<std::size_t>(HandshakeCompression::Count)> &support) noexcept {
            return std::ranges::find(support, true) != support.end();
        }

        [[nodiscard]] bool ValidRequiredCompression(const std::array<bool, static_cast<std::size_t>(HandshakeCompression::Count)> &support,
                                                    const HandshakeCompression required) noexcept {
            return required == HandshakeCompression::Count ||
                   (required < HandshakeCompression::Count && support[CompressionIndex(required)]);
        }

        [[nodiscard]] bool Contains(const std::span<const ProtocolFeatureId> values, const ProtocolFeatureId candidate) noexcept {
            return std::ranges::find(values, candidate) != values.end();
        }

        [[nodiscard]] bool ValidFeatureSet(const ProtocolId protocol, const HandshakeFeatureSetView &features) noexcept {
            if (features.supported.size() > MaximumHandshakeFeatures || features.required.size() > MaximumHandshakeFeatures)
                return false;
            for (std::size_t index = 0; index < features.supported.size(); ++index) {
                const auto feature = features.supported[index];
                if (!feature.IsValid() || feature.Namespace() != protocol.Namespace())
                    return false;
                if (std::ranges::find(features.supported.first(index), feature) != features.supported.first(index).end())
                    return false;
            }
            for (std::size_t index = 0; index < features.required.size(); ++index) {
                const auto feature = features.required[index];
                if (!Contains(features.supported, feature) ||
                    std::ranges::find(features.required.first(index), feature) != features.required.first(index).end())
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool IncludesAll(const std::span<const ProtocolFeatureId> available,
                                       const std::span<const ProtocolFeatureId> required) noexcept {
            return std::ranges::all_of(required, [available](const ProtocolFeatureId feature) {
                return Contains(available, feature);
            });
        }

        [[nodiscard]] Result<HandshakeCompression> SelectCompression(
            const std::array<bool, static_cast<std::size_t>(HandshakeCompression::Count)> &local, const HandshakeCompression localRequired,
            const std::array<bool, static_cast<std::size_t>(HandshakeCompression::Count)> &peer, const HandshakeCompression peerRequired) {
            using enum HandshakeCompression;

            if (!HasCompression(peer) || !ValidRequiredCompression(peer, peerRequired))
                return Failure<HandshakeCompression>(NetworkErrors::HandshakeInvalid);
            if (localRequired != Count && peerRequired != Count && localRequired != peerRequired)
                return Failure<HandshakeCompression>(NetworkErrors::HandshakeIncompatible);

            if (const auto required = localRequired != Count ? localRequired : peerRequired; required != Count)
                return peer[CompressionIndex(required)] && local[CompressionIndex(required)]
                           ? Result<HandshakeCompression>::Success(required)
                           : Failure<HandshakeCompression>(NetworkErrors::HandshakeIncompatible);

            constexpr std::array preference{
                Zstandard,
                Lz4,
                None,
            };
            for (const HandshakeCompression candidate : preference) {
                if (local[CompressionIndex(candidate)] && peer[CompressionIndex(candidate)])
                    return Result<HandshakeCompression>::Success(candidate);
            }
            return Failure<HandshakeCompression>(NetworkErrors::HandshakeIncompatible);
        }
    }  // namespace

    /** @copydoc ValidateHandshakeLocalDescriptor */
    Result<void> ValidateHandshakeLocalDescriptor(const HandshakeLocalDescriptor &local) {
        const auto &compatibility = local.compatibility;
        if (compatibility.contractVersion != HandshakeContractVersion || !compatibility.protocol.IsValid() ||
            !compatibility.versions.IsValid() || compatibility.schemaFingerprint == 0 ||
            !ValidFeatureSet(compatibility.protocol, compatibility.features) || !HasCompression(compatibility.compression) ||
            !ValidRequiredCompression(compatibility.compression, compatibility.requiredCompression) ||
            !ValidateTransportCapabilities(local.transport))
            return Result<void>::Failure(MakeError(NetworkErrors::HandshakeInvalid));
        return Result<void>::Success();
    }

    HandshakeNegotiator::HandshakeNegotiator(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                             const std::uint64_t deadlineTick, OwnedLocalPolicy local) noexcept
        : connection_(connection), sessionGeneration_(sessionGeneration), deadlineTick_(deadlineTick), local_(std::move(local)) {}

    /** @copydoc HandshakeNegotiator::Create */
    Result<HandshakeNegotiator> HandshakeNegotiator::Create(const ConnectionHandle connection,
                                                            const NetworkOperationGeneration sessionGeneration,
                                                            const std::uint64_t deadlineTick, const HandshakeLocalDescriptor &local) {
        if (!connection.IsValid() || !sessionGeneration.IsValid() || deadlineTick == 0 ||
            ValidateHandshakeLocalDescriptor(local).HasError())
            return Failure<HandshakeNegotiator>(NetworkErrors::HandshakeInvalid);
        const auto &compatibility = local.compatibility;

        OwnedFeatures features;
        features.supportedCount = compatibility.features.supported.size();
        features.requiredCount = compatibility.features.required.size();
        std::ranges::copy(compatibility.features.supported, features.supported.begin());
        std::ranges::copy(compatibility.features.required, features.required.begin());
        std::ranges::sort(std::span{features.supported}.first(features.supportedCount));
        std::ranges::sort(std::span{features.required}.first(features.requiredCount));
        OwnedLocalPolicy owned{compatibility.protocol, compatibility.versions,    compatibility.schemaFingerprint,
                               std::move(features),    compatibility.compression, compatibility.requiredCompression,
                               local.transport};
        return Result<HandshakeNegotiator>::Success(HandshakeNegotiator{connection, sessionGeneration, deadlineTick, std::move(owned)});
    }

    bool HandshakeNegotiator::Owns(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration) const noexcept {
        return connection == connection_ && sessionGeneration == sessionGeneration_;
    }

    Result<HandshakeSelection> HandshakeNegotiator::Negotiate(const HandshakeOffer &offer) const {
        const auto &compatibility = offer.compatibility;
        if (compatibility.contractVersion != HandshakeContractVersion || !compatibility.protocol.IsValid() ||
            !compatibility.versions.IsValid() || compatibility.schemaFingerprint == 0 ||
            !ValidFeatureSet(compatibility.protocol, compatibility.features))
            return Failure<HandshakeSelection>(NetworkErrors::HandshakeInvalid);
        if (compatibility.protocol != local_.protocol || compatibility.schemaFingerprint != local_.schemaFingerprint ||
            compatibility.versions.minimum.major != local_.versions.minimum.major)
            return Failure<HandshakeSelection>(NetworkErrors::HandshakeIncompatible);

        const ProtocolVersion minimum = std::max(local_.versions.minimum, compatibility.versions.minimum);
        const ProtocolVersion maximum = std::min(local_.versions.maximum, compatibility.versions.maximum);
        if (minimum > maximum)
            return Failure<HandshakeSelection>(NetworkErrors::HandshakeIncompatible);

        const auto localSupported = std::span{local_.features.supported}.first(local_.features.supportedCount);
        if (const auto localRequired = std::span{local_.features.required}.first(local_.features.requiredCount);
            !IncludesAll(localSupported, compatibility.features.required) || !IncludesAll(compatibility.features.supported, localRequired))
            return Failure<HandshakeSelection>(NetworkErrors::HandshakeIncompatible);

        NegotiatedFeatureSet negotiated;
        for (const auto feature : localSupported) {
            if (Contains(compatibility.features.supported, feature))
                negotiated.values[negotiated.count++] = feature;
        }

        const auto compression =
            SelectCompression(local_.compression, local_.requiredCompression, compatibility.compression, compatibility.requiredCompression);
        if (compression.HasError())
            return Result<HandshakeSelection>::Failure(compression.ErrorValue());
        const auto transport = ResolveTransportCapabilities(local_.transport, local_.transport.revision, offer.transport);
        if (transport.HasError())
            return Result<HandshakeSelection>::Failure(transport.ErrorValue());

        return Result<HandshakeSelection>::Success(HandshakeSelection{connection_, sessionGeneration_, local_.protocol, maximum,
                                                                      local_.schemaFingerprint, negotiated, compression.Value(),
                                                                      transport.Value()});
    }

    /** @copydoc HandshakeNegotiator::Accept */
    Result<HandshakeSelection> HandshakeNegotiator::Accept(const ConnectionHandle connection,
                                                           const NetworkOperationGeneration sessionGeneration, const HandshakeOffer &offer,
                                                           const std::uint64_t nowTick, const TransportAdmissionState operation) {
        if (!Owns(connection, sessionGeneration))
            return Failure<HandshakeSelection>(NetworkErrors::NetworkLifecycleOperationStale);
        if (state_ == HandshakeState::ShuttingDown)
            return Failure<HandshakeSelection>(NetworkErrors::SessionShuttingDown);
        if (state_ != HandshakeState::AwaitingOffer)
            return Failure<HandshakeSelection>(NetworkErrors::HandshakeStateInvalid);
        if (operation == TransportAdmissionState::Cancelled) {
            state_ = HandshakeState::Rejected;
            return Failure<HandshakeSelection>(NetworkErrors::SessionCancelled);
        }
        if (operation == TransportAdmissionState::ShuttingDown) {
            state_ = HandshakeState::ShuttingDown;
            return Failure<HandshakeSelection>(NetworkErrors::SessionShuttingDown);
        }
        if (nowTick == 0 || operation != TransportAdmissionState::Accepting) {
            state_ = HandshakeState::Rejected;
            return Failure<HandshakeSelection>(NetworkErrors::HandshakeInvalid);
        }
        if (nowTick >= deadlineTick_) {
            state_ = HandshakeState::TimedOut;
            return Failure<HandshakeSelection>(NetworkErrors::SessionTimedOut);
        }

        state_ = HandshakeState::Negotiating;
        auto result = Negotiate(offer);
        if (result.HasError()) {
            state_ = HandshakeState::Rejected;
            return result;
        }

        selection_ = std::move(result).Value();
        state_ = HandshakeState::Accepted;
        return Result<HandshakeSelection>::Success(selection_);
    }

    /** @copydoc HandshakeNegotiator::Reject */
    Result<void> HandshakeNegotiator::Reject(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration) {
        using enum HandshakeState;

        if (!Owns(connection, sessionGeneration))
            return Failure<void>(NetworkErrors::NetworkLifecycleOperationStale);
        if (state_ == ShuttingDown)
            return Failure<void>(NetworkErrors::SessionShuttingDown);
        if (state_ != AwaitingOffer)
            return Failure<void>(NetworkErrors::HandshakeStateInvalid);
        state_ = Rejected;
        return Result<void>::Success();
    }

    /** @copydoc HandshakeNegotiator::Expire */
    bool HandshakeNegotiator::Expire(const std::uint64_t nowTick) noexcept {
        if (state_ != HandshakeState::AwaitingOffer || nowTick < deadlineTick_)
            return false;
        state_ = HandshakeState::TimedOut;
        return true;
    }

    /** @copydoc HandshakeNegotiator::Shutdown */
    bool HandshakeNegotiator::Shutdown() noexcept {
        using enum HandshakeState;

        if (state_ == ShuttingDown)
            return false;
        if (state_ == Accepted || state_ == Rejected || state_ == TimedOut)
            return false;
        state_ = ShuttingDown;
        return true;
    }

    /** @copydoc HandshakeNegotiator::Selection */
    const HandshakeSelection *HandshakeNegotiator::Selection() const noexcept {
        return state_ == HandshakeState::Accepted ? &selection_ : nullptr;
    }
}  // namespace Horo::Network
