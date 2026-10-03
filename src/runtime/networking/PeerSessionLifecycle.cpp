#include "Horo/Network/PeerSessionLifecycle.h"

#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/NetworkMetrics.h"
#include "NetworkValidationInternal.h"

#include <algorithm>
#include <limits>
#include <ranges>

namespace Horo::Network {
    namespace {
        [[nodiscard]] bool ValidDeadlines(const PeerSessionDeadlines &deadlines) noexcept {
            return deadlines.negotiationTick != 0 && deadlines.authenticationTick > deadlines.negotiationTick &&
                   deadlines.activationTick > deadlines.authenticationTick && deadlines.lifetimeTick > deadlines.activationTick &&
                   deadlines.inactivityTicks != 0;
        }

        [[nodiscard]] bool ValidTransportSelection(const TransportSelectionEvidence &transport) noexcept {
            const bool hasDelivery = std::ranges::any_of(transport.admittedDelivery, [](const bool admitted) {
                return admitted;
            });
            return transport.capabilityRevision != 0 && hasDelivery && transport.channelCount != 0 && transport.maximumMessageBytes != 0 &&
                   (transport.deadlinesEnabled == (transport.maximumDeadlineMilliseconds != 0));
        }

        [[nodiscard]] bool ValidNegotiation(const HandshakeSelection &selection) noexcept {
            if (selection.features.count > MaximumHandshakeFeatures)
                return false;
            const auto features = selection.features.Values();
            return selection.connection.IsValid() && selection.sessionGeneration.IsValid() && selection.protocol.IsValid() &&
                   selection.version.IsValid() && selection.schemaFingerprint != 0 &&
                   std::ranges::all_of(features,
                                       [](const ProtocolFeatureId feature) {
                return feature.IsValid();
            }) &&
                   std::ranges::adjacent_find(features,
                                              [](const ProtocolFeatureId left, const ProtocolFeatureId right) {
                return left.Value() >= right.Value();
            }) == features.end() &&
                   selection.compression < HandshakeCompression::Count && ValidTransportSelection(selection.transport);
        }

        [[nodiscard]] bool ValidAuthentication(const AuthenticationResult &authentication, const std::uint64_t nowTick) noexcept {
            const auto &principal = authentication.principal;
            const auto &channel = authentication.secureChannel;
            return authentication.connection.IsValid() && authentication.sessionGeneration.IsValid() && authentication.policy.IsValid() &&
                   authentication.policyRevision != 0 && principal.principal.IsValid() && principal.session.IsValid() &&
                   principal.trustLevel < NetworkTrustLevel::Count && principal.provenance.IsValid() && principal.expiresAtTick > nowTick &&
                   Detail::ValidCanonicalIdentities(principal.roles.values, principal.roles.count) &&
                   Detail::ValidCanonicalIdentities(principal.capabilities.values, principal.capabilities.count) &&
                   channel.stamp.connection == authentication.connection &&
                   channel.stamp.sessionGeneration == authentication.sessionGeneration && channel.stamp.authorityGeneration != 0 &&
                   channel.binding.IsValid() && channel.channel.IsValid() && channel.channelGeneration != 0 &&
                   Detail::HasNonZeroByte(channel.bindingDigest);
        }

        [[nodiscard]] std::uint64_t SaturatingAdd(const std::uint64_t left, const std::uint64_t right) noexcept {
            return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() : left + right;
        }

        [[nodiscard]] bool IsGracefulClose(const PeerSessionTerminalKind kind) noexcept {
            return kind == PeerSessionTerminalKind::LocalClose || kind == PeerSessionTerminalKind::RemoteClose;
        }
    }  // namespace

    PeerSessionLifecycle::PeerSessionLifecycle(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                               const PeerSessionDeadlines &deadlines, NetworkMetrics *metrics) noexcept
        : connection_(connection), sessionGeneration_(sessionGeneration), deadlines_(deadlines), metrics_(metrics) {}

    /** @copydoc PeerSessionLifecycle::Create */
    Result<PeerSessionLifecycle> PeerSessionLifecycle::Create(const ConnectionHandle connection,
                                                              const NetworkOperationGeneration sessionGeneration,
                                                              const PeerSessionDeadlines &deadlines, NetworkMetrics *metrics) {
        if (!connection.IsValid() || !sessionGeneration.IsValid() || !ValidDeadlines(deadlines))
            return Result<PeerSessionLifecycle>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));
        return Result<PeerSessionLifecycle>::Success(PeerSessionLifecycle{connection, sessionGeneration, deadlines, metrics});
    }

    bool PeerSessionLifecycle::Owns(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration) const noexcept {
        return connection == connection_ && sessionGeneration == sessionGeneration_;
    }

    Result<void> PeerSessionLifecycle::MutableOperation(const ConnectionHandle connection,
                                                        const NetworkOperationGeneration sessionGeneration) const {
        if (!Owns(connection, sessionGeneration))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (terminal_.has_value())
            return Result<void>::Failure(MakeError(NetworkErrors::TerminalAlreadyResolved));
        return Result<void>::Success();
    }

    Result<NetworkTerminalRecord> PeerSessionLifecycle::SessionFailure(const NetworkFailureKind kind) const {
        using enum NetworkFailureKind;
        NetworkFailureLayer layer = NetworkFailureLayer::Session;
        if (kind == ProtocolMalformed || kind == ProtocolIncompatible)
            layer = NetworkFailureLayer::Protocol;
        else if (kind == NameResolutionFailed || kind == TransportUnavailable || kind == TransportSaturated)
            layer = NetworkFailureLayer::Transport;
        return MakeNetworkTerminalRecord(layer, kind);
    }

    Result<void> PeerSessionLifecycle::PublishTerminal(const PeerSessionTerminalKind kind, const std::uint64_t nowTick,
                                                       std::optional<NetworkTerminalRecord> failure, const CloseReasonId closeReason) {
        if (terminal_.has_value())
            return Result<void>::Failure(MakeError(NetworkErrors::TerminalAlreadyResolved));
        if (kind >= PeerSessionTerminalKind::Count || nowTick == 0 || (IsGracefulClose(kind) != closeReason.IsValid()) ||
            (IsGracefulClose(kind) == failure.has_value()))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));

        terminal_.emplace(connection_, sessionGeneration_, kind, closeReason, nowTick, std::move(failure));
        activityDeadlineTick_ = 0;
        state_ = IsGracefulClose(kind) || kind == PeerSessionTerminalKind::LocalCancellation || kind == PeerSessionTerminalKind::Shutdown
                     ? PeerSessionState::Closed
                     : PeerSessionState::Failed;
        if (metrics_ && metrics_->IsCollecting()) {
            switch (kind) {
                case PeerSessionTerminalKind::ProtocolRejected:
                case PeerSessionTerminalKind::NegotiationTimeout:
                    (void)metrics_->RecordFailure(NetworkMetricFailure::Protocol);
                    break;
                case PeerSessionTerminalKind::AuthenticationRejected:
                case PeerSessionTerminalKind::AuthenticationTimeout:
                case PeerSessionTerminalKind::CredentialExpired:
                    (void)metrics_->RecordFailure(NetworkMetricFailure::Authentication);
                    break;
                case PeerSessionTerminalKind::TransportFailed:
                    (void)metrics_->RecordFailure(NetworkMetricFailure::Transport);
                    break;
                case PeerSessionTerminalKind::LocalCancellation:
                    (void)metrics_->RecordDrop(NetworkMetricDrop::Cancelled);
                    break;
                case PeerSessionTerminalKind::ActivationTimeout:
                case PeerSessionTerminalKind::InactivityTimeout:
                case PeerSessionTerminalKind::LifetimeTimeout:
                    (void)metrics_->RecordFailure(NetworkMetricFailure::Session);
                    break;
                case PeerSessionTerminalKind::LocalClose:
                case PeerSessionTerminalKind::RemoteClose:
                case PeerSessionTerminalKind::Shutdown:
                case PeerSessionTerminalKind::Count:
                    break;
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::PublishFailureOrClose */
    Result<void> PeerSessionLifecycle::PublishFailureOrClose(const NetworkFailureKind failure, const PeerSessionTerminalKind kind,
                                                             const std::uint64_t nowTick) {
        if (state_ == PeerSessionState::Closing)
            return CompleteClose(connection_, sessionGeneration_, nowTick);
        auto terminal = SessionFailure(failure);
        if (terminal.HasError())
            return Result<void>::Failure(terminal.ErrorValue());
        return PublishTerminal(kind, nowTick, std::move(terminal).Value());
    }

    /** @copydoc PeerSessionLifecycle::BeginNegotiation */
    Result<void> PeerSessionLifecycle::BeginNegotiation(const ConnectionHandle connection,
                                                        const NetworkOperationGeneration sessionGeneration, const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Created)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        if (nowTick == 0 || nowTick >= deadlines_.negotiationTick)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        state_ = PeerSessionState::Negotiating;
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::AcceptNegotiation */
    Result<void> PeerSessionLifecycle::AcceptNegotiation(const HandshakeSelection &selection, const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(selection.connection, selection.sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Negotiating)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        if (nowTick == 0 || nowTick >= deadlines_.negotiationTick || nowTick >= deadlines_.authenticationTick)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        if (!ValidNegotiation(selection))
            return Result<void>::Failure(MakeError(NetworkErrors::HandshakeInvalid));
        negotiation_ = selection;
        state_ = PeerSessionState::Authenticating;
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::AcceptAuthentication */
    Result<void> PeerSessionLifecycle::AcceptAuthentication(const AuthenticationResult &authentication, const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(authentication.connection, authentication.sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Authenticating)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        if (nowTick == 0 || nowTick >= deadlines_.authenticationTick || nowTick >= deadlines_.activationTick)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        if (!ValidAuthentication(authentication, nowTick))
            return Result<void>::Failure(MakeError(NetworkErrors::AuthenticationInvalid));
        authentication_ = authentication;
        state_ = PeerSessionState::Activating;
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::Activate */
    Result<void> PeerSessionLifecycle::Activate(const PeerSessionActivation &activation, const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(activation.connection, activation.sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Activating || !authentication_.has_value())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        if (nowTick == 0 || nowTick >= deadlines_.activationTick || nowTick >= deadlines_.lifetimeTick ||
            nowTick >= authentication_->principal.expiresAtTick)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        if (const auto &channel = authentication_->secureChannel; activation.channel != channel.channel ||
                                                                  activation.channelGeneration != channel.channelGeneration ||
                                                                  activation.bindingDigest != channel.bindingDigest)
            return Result<void>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        activityDeadlineTick_ = std::min(
            {deadlines_.lifetimeTick, authentication_->principal.expiresAtTick, SaturatingAdd(nowTick, deadlines_.inactivityTicks)});
        state_ = PeerSessionState::Active;
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::AdmitGameplay */
    Result<void> PeerSessionLifecycle::AdmitGameplay(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                                     const std::uint64_t nowTick) const {
        if (!Owns(connection, sessionGeneration) || terminal_.has_value())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (state_ != PeerSessionState::Active)
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (nowTick == 0 || nowTick >= activityDeadlineTick_)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::RecordActivity */
    Result<void> PeerSessionLifecycle::RecordActivity(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                                      const std::uint64_t nowTick) {
        if (auto admitted = AdmitGameplay(connection, sessionGeneration, nowTick); admitted.HasError())
            return admitted;
        activityDeadlineTick_ = std::min(
            {deadlines_.lifetimeTick, authentication_->principal.expiresAtTick, SaturatingAdd(nowTick, deadlines_.inactivityTicks)});
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::RequestClose */
    Result<void> PeerSessionLifecycle::RequestClose(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                                    const PeerSessionTerminalKind kind, const CloseReasonId reason,
                                                    const std::uint64_t nowTick) {
        using enum PeerSessionState;
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        if (!IsGracefulClose(kind) || !reason.IsValid() || nowTick == 0)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));
        if (state_ == Closing)
            return pendingCloseKind_ == kind && pendingCloseReason_ == reason
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(NetworkErrors::TerminalAlreadyResolved));
        if (state_ != Active)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        pendingCloseKind_ = kind;
        pendingCloseReason_ = reason;
        state_ = Closing;
        activityDeadlineTick_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc PeerSessionLifecycle::CompleteClose */
    Result<void> PeerSessionLifecycle::CompleteClose(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                                     const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Closing || !IsGracefulClose(pendingCloseKind_) || !pendingCloseReason_.IsValid())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        return PublishTerminal(pendingCloseKind_, nowTick, {}, pendingCloseReason_);
    }

    /** @copydoc PeerSessionLifecycle::RejectProtocol */
    Result<void> PeerSessionLifecycle::RejectProtocol(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                                      const NetworkFailureKind failure, const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Closing) {
            if (failure != NetworkFailureKind::ProtocolMalformed && failure != NetworkFailureKind::ProtocolIncompatible)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));
            if (state_ != PeerSessionState::Negotiating)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        }
        return PublishFailureOrClose(failure, PeerSessionTerminalKind::ProtocolRejected, nowTick);
    }

    /** @copydoc PeerSessionLifecycle::RejectAuthentication */
    Result<void> PeerSessionLifecycle::RejectAuthentication(const ConnectionHandle connection,
                                                            const NetworkOperationGeneration sessionGeneration,
                                                            const std::uint64_t nowTick) {
        using enum PeerSessionState;
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        if (state_ != Closing && state_ != Authenticating && state_ != Activating)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleTransitionInvalid));
        return PublishFailureOrClose(NetworkFailureKind::SessionAuthenticationRejected, PeerSessionTerminalKind::AuthenticationRejected,
                                     nowTick);
    }

    /** @copydoc PeerSessionLifecycle::FailTransport */
    Result<void> PeerSessionLifecycle::FailTransport(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                                     const NetworkFailureKind failure, const std::uint64_t nowTick) {
        using enum NetworkFailureKind;
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        if (state_ != PeerSessionState::Closing && failure != NameResolutionFailed && failure != TransportUnavailable &&
            failure != TransportSaturated)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleInvalid));
        return PublishFailureOrClose(failure, PeerSessionTerminalKind::TransportFailed, nowTick);
    }

    /** @copydoc PeerSessionLifecycle::Cancel */
    Result<void> PeerSessionLifecycle::Cancel(const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration,
                                              const std::uint64_t nowTick) {
        if (auto valid = MutableOperation(connection, sessionGeneration); valid.HasError())
            return valid;
        return PublishFailureOrClose(NetworkFailureKind::SessionCancelled, PeerSessionTerminalKind::LocalCancellation, nowTick);
    }

    /** @copydoc PeerSessionLifecycle::Expire */
    bool PeerSessionLifecycle::Expire(const std::uint64_t nowTick) {
        if (terminal_.has_value() || nowTick == 0)
            return false;
        if (state_ == PeerSessionState::Closing && nowTick >= deadlines_.lifetimeTick)
            return CompleteClose(connection_, sessionGeneration_, nowTick).HasValue();
        PeerSessionTerminalKind kind = PeerSessionTerminalKind::Count;
        if ((state_ == PeerSessionState::Created || state_ == PeerSessionState::Negotiating) && nowTick >= deadlines_.negotiationTick)
            kind = PeerSessionTerminalKind::NegotiationTimeout;
        else if (state_ == PeerSessionState::Authenticating && nowTick >= deadlines_.authenticationTick)
            kind = PeerSessionTerminalKind::AuthenticationTimeout;
        else if (state_ == PeerSessionState::Activating && nowTick >= deadlines_.activationTick)
            kind = PeerSessionTerminalKind::ActivationTimeout;
        else if (state_ == PeerSessionState::Active) {
            std::uint64_t earliest = activityDeadlineTick_;
            kind = PeerSessionTerminalKind::InactivityTimeout;
            if (deadlines_.lifetimeTick <= earliest) {
                earliest = deadlines_.lifetimeTick;
                kind = PeerSessionTerminalKind::LifetimeTimeout;
            }
            if (authentication_->principal.expiresAtTick <= earliest) {
                earliest = authentication_->principal.expiresAtTick;
                kind = PeerSessionTerminalKind::CredentialExpired;
            }
            if (nowTick < earliest)
                kind = PeerSessionTerminalKind::Count;
        }
        if (kind == PeerSessionTerminalKind::Count)
            return false;
        const auto failure = kind == PeerSessionTerminalKind::CredentialExpired ? NetworkFailureKind::SessionAuthenticationRejected
                                                                                : NetworkFailureKind::SessionTimedOut;
        auto terminal = SessionFailure(failure);
        return terminal.HasValue() && PublishTerminal(kind, nowTick, std::move(terminal).Value()).HasValue();
    }

    /** @copydoc PeerSessionLifecycle::Shutdown */
    bool PeerSessionLifecycle::Shutdown(const std::uint64_t nowTick) {
        if (terminal_.has_value() || nowTick == 0)
            return false;
        if (state_ == PeerSessionState::Closing)
            return CompleteClose(connection_, sessionGeneration_, nowTick).HasValue();
        auto terminal = SessionFailure(NetworkFailureKind::SessionShutdown);
        return terminal.HasValue() && PublishTerminal(PeerSessionTerminalKind::Shutdown, nowTick, std::move(terminal).Value()).HasValue();
    }

    /** @copydoc PeerSessionLifecycle::Negotiation */
    const HandshakeSelection *PeerSessionLifecycle::Negotiation() const noexcept {
        return negotiation_.has_value() ? &*negotiation_ : nullptr;
    }

    /** @copydoc PeerSessionLifecycle::Authentication */
    const AuthenticationResult *PeerSessionLifecycle::Authentication() const noexcept {
        return authentication_.has_value() ? &*authentication_ : nullptr;
    }

    /** @copydoc PeerSessionLifecycle::Terminal */
    const PeerSessionTerminalSnapshot *PeerSessionLifecycle::Terminal() const noexcept {
        return terminal_.has_value() ? &*terminal_ : nullptr;
    }
}  // namespace Horo::Network
