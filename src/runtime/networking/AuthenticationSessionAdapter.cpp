#include "Horo/Network/AuthenticationSessionAdapter.h"

#include "Horo/Network/AdmissionProtection.h"
#include "NetworkProviderBoundary.h"
#include "NetworkValidationInternal.h"

namespace Horo::Network {
    namespace {
        [[nodiscard]] bool ValidPolicy(const NetworkTrustPolicySnapshot &policy) noexcept {
            if (policy.contractVersion != AuthenticationContractVersion || !policy.id.IsValid() || policy.revision == 0 ||
                policy.exposure >= NetworkExposure::Count || !policy.credential.IsValid() || !policy.certificate.IsValid() ||
                !policy.privateKey.IsValid() || policy.maximumProofBytes == 0 || policy.maximumProofBytes > MaximumAuthenticationProofBytes)
                return false;
            return policy.exposure == NetworkExposure::LoopbackDevelopment || !policy.allowUnprotectedInMemoryLoopback;
        }

        [[nodiscard]] bool ValidChallenge(const AuthenticationChallenge &challenge, const NetworkTrustPolicySnapshot &policy) noexcept {
            return challenge.contractVersion == AuthenticationContractVersion && challenge.connection.IsValid() &&
                   challenge.sessionGeneration.IsValid() && challenge.policy == policy.id && challenge.policyRevision == policy.revision &&
                   Detail::HasNonZeroByte(challenge.transcriptDigest) && Detail::HasNonZeroByte(challenge.clientNonce) &&
                   Detail::HasNonZeroByte(challenge.serverNonce) && challenge.clientNonce != challenge.serverNonce;
        }

        [[nodiscard]] bool ValidStamp(const AuthenticationAuthorityStamp &stamp, const AuthenticationChallenge &challenge) noexcept {
            return stamp.connection == challenge.connection && stamp.sessionGeneration == challenge.sessionGeneration &&
                   stamp.authorityGeneration != 0;
        }

        [[nodiscard]] bool ExposureAllowsTrust(const NetworkExposure exposure, const NetworkTrustLevel trustLevel) noexcept {
            switch (exposure) {
                case NetworkExposure::LoopbackDevelopment:
                    return trustLevel < NetworkTrustLevel::Count;
                case NetworkExposure::LocalNetwork:
                    return trustLevel == NetworkTrustLevel::Paired || trustLevel == NetworkTrustLevel::ProductAnchor;
                case NetworkExposure::Remote:
                    return trustLevel == NetworkTrustLevel::ProductAnchor;
                case NetworkExposure::Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] bool ValidTransportEvidence(const PeerAuthenticationEvidence &evidence, const AuthenticationChallenge &challenge,
                                                  const NetworkTrustPolicySnapshot &policy) noexcept {
            const auto &transport = evidence.transport;
            if (transport.connection != challenge.connection || transport.sessionGeneration != challenge.sessionGeneration ||
                !transport.channel.IsValid() || transport.channelGeneration == 0)
                return false;
            const bool protectedChannel = transport.confidentiality && transport.integrity;
            if (!protectedChannel && !(policy.allowUnprotectedInMemoryLoopback && transport.inMemoryLoopback))
                return false;
            if (policy.exposure != NetworkExposure::LoopbackDevelopment && (!protectedChannel || transport.inMemoryLoopback))
                return false;
            if (policy.exposure == NetworkExposure::Remote && !transport.authenticatedPeer)
                return false;
            return evidence.certificate.binding == policy.certificate && evidence.certificate.evidenceGeneration != 0 &&
                   Detail::HasNonZeroByte(evidence.certificate.certificateDigest);
        }

        [[nodiscard]] bool ValidPrincipal(const SessionPrincipal &principal, const NetworkTrustLevel trustLevel,
                                          const std::uint64_t nowTick) noexcept {
            return principal.principal.IsValid() && principal.session.IsValid() && principal.trustLevel == trustLevel &&
                   principal.provenance.IsValid() && principal.expiresAtTick > nowTick &&
                   Detail::ValidCanonicalIdentities(principal.roles.values, principal.roles.count) &&
                   Detail::ValidCanonicalIdentities(principal.capabilities.values, principal.capabilities.count);
        }

        /** @brief Issuance and credential acceptance must observe one exact public channel/certificate generation. */
        bool SameEvidence(const PeerAuthenticationEvidence &left, const PeerAuthenticationEvidence &right) noexcept {
            const auto &a = left.transport;
            const auto &b = right.transport;
            return a.connection == b.connection && a.sessionGeneration == b.sessionGeneration && a.channel == b.channel &&
                   a.channelGeneration == b.channelGeneration && a.confidentiality == b.confidentiality && a.integrity == b.integrity &&
                   a.authenticatedPeer == b.authenticatedPeer && a.inMemoryLoopback == b.inMemoryLoopback &&
                   left.certificate.binding == right.certificate.binding &&
                   left.certificate.certificateDigest == right.certificate.certificateDigest &&
                   left.certificate.evidenceGeneration == right.certificate.evidenceGeneration;
        }

        /** @brief Requires every host authority before entering the ordered verification chain. */
        [[nodiscard]] bool AvailableAuthorities(const AuthenticationAuthorities &authorities) noexcept {
            return authorities.certificates && authorities.peers && authorities.credentials && authorities.privateKeys &&
                   authorities.certificates->Available() && authorities.peers->Available() && authorities.credentials->Available() &&
                   authorities.privateKeys->Available();
        }
    }  // namespace

    /** @copydoc ProofIssuanceAuthorization::MatchesEvidence */
    bool ProofIssuanceAuthorization::MatchesEvidence(const PeerAuthenticationEvidence &evidence,
                                                     const std::uint64_t nowTick) const noexcept {
        return nowTick != 0 && nowTick < deadlineTick_ && SameEvidence(evidence, evidence_);
    }

    /** @copydoc NetworkSessionId::IsValid */
    bool NetworkSessionId::IsValid() const noexcept {
        return Detail::HasNonZeroByte(bytes);
    }

    AuthenticationSessionAdapter::AuthenticationSessionAdapter(const NetworkTrustPolicySnapshot &policy,
                                                               const AuthenticationChallenge &challenge,
                                                               const AuthenticationAuthorities &authorities,
                                                               const std::uint64_t deadlineTick, AdmissionProtection *protection) noexcept
        : policy_(policy), challenge_(challenge), authorities_(authorities), protection_(protection), deadlineTick_(deadlineTick) {}

    /** @copydoc ValidateNetworkTrustPolicy */
    bool ValidateNetworkTrustPolicy(const NetworkTrustPolicySnapshot &policy) noexcept {
        return ValidPolicy(policy);
    }

    /** @copydoc AuthenticationSessionAdapter::Create */
    Result<AuthenticationSessionAdapter> AuthenticationSessionAdapter::Create(const NetworkTrustPolicySnapshot &policy,
                                                                              const AuthenticationChallenge &challenge,
                                                                              const AuthenticationAuthorities &authorities,
                                                                              const std::uint64_t deadlineTick,
                                                                              AdmissionProtection *protection) {
        if (!ValidateNetworkTrustPolicy(policy) || !ValidChallenge(challenge, policy) || deadlineTick == 0)
            return Result<AuthenticationSessionAdapter>::Failure(MakeError(NetworkErrors::AuthenticationInvalid));
        return Result<AuthenticationSessionAdapter>::Success(
            AuthenticationSessionAdapter{policy, challenge, authorities, deadlineTick, protection});
    }

    bool AuthenticationSessionAdapter::Owns(const ConnectionHandle connection,
                                            const NetworkOperationGeneration sessionGeneration) const noexcept {
        return connection == challenge_.connection && sessionGeneration == challenge_.sessionGeneration;
    }

    Result<AuthenticationResult> AuthenticationSessionAdapter::Reject(const ErrorCodeDescriptor &error,
                                                                      const AuthenticationFailureClass failure) {
        state_ = AuthenticationState::Rejected;
        failure_ = failure;
        return Result<AuthenticationResult>::Failure(MakeError(error));
    }

    /** @copydoc AuthenticationSessionAdapter::VerifyPeerAuthorities */
    Result<AuthenticationSessionAdapter::VerifiedPeerAuthorities> AuthenticationSessionAdapter::VerifyPeerAuthorities(
        const PeerAuthenticationEvidence &evidence, const std::uint64_t nowTick) {
        const auto reject = [this](const ErrorCodeDescriptor &error, const AuthenticationFailureClass failure) {
            state_ = AuthenticationState::Rejected;
            failure_ = failure;
            return Result<VerifiedPeerAuthorities>::Failure(MakeError(error));
        };
        const auto charge = [this, nowTick]() {
            return protection_ == nullptr ||
                   protection_->Charge(challenge_.connection, challenge_.sessionGeneration, AdmissionWork::VerifierCall, 1, nowTick)
                       .HasValue();
        };
        if (!AvailableAuthorities(authorities_))
            return reject(NetworkErrors::AuthenticationTrustUnavailable, AuthenticationFailureClass::TrustUnavailable);
        auto attempted = Detail::InvokeAdmissionProvider([this, &evidence, &reject, &charge]() {
            if (!charge())
                return reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
            const auto certificate = authorities_.certificates->Verify({challenge_, evidence.certificate});
            if (certificate.HasError())
                return reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
            if (!ValidStamp(certificate.Value().stamp, challenge_) || certificate.Value().binding != policy_.certificate)
                return reject(NetworkErrors::AuthenticationInvalid, AuthenticationFailureClass::Malformed);
            if (!charge())
                return reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
            const auto peer = authorities_.peers->Verify({challenge_, evidence, certificate.Value()});
            if (peer.HasError())
                return reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
            if (!ValidStamp(peer.Value().stamp, challenge_) || !ExposureAllowsTrust(policy_.exposure, peer.Value().trustLevel))
                return reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
            return Result<VerifiedPeerAuthorities>::Success({certificate.Value(), peer.Value()});
        });
        if (!attempted)
            return reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
        return std::move(*attempted);
    }

    /** @copydoc AuthenticationSessionAdapter::AuthorizeProofIssuance */
    Result<ProofIssuanceAuthorization> AuthenticationSessionAdapter::AuthorizeProofIssuance(
        const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration, const AuthenticationChallenge &peerChallenge,
        const PeerAuthenticationEvidence &evidence, const std::uint64_t nowTick) {
        if (!Owns(connection, sessionGeneration))
            return Result<ProofIssuanceAuthorization>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (state_ != AuthenticationState::AwaitingProof || proofAuthorization_)
            return Result<ProofIssuanceAuthorization>::Failure(MakeError(NetworkErrors::AuthenticationStateInvalid));
        if (nowTick >= deadlineTick_) {
            (void)Expire(nowTick);
            return Result<ProofIssuanceAuthorization>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        }
        if (nowTick == 0 || !ValidChallenge(peerChallenge, policy_) || peerChallenge.connection != connection ||
            peerChallenge.sessionGeneration != sessionGeneration || peerChallenge.clientNonce != challenge_.clientNonce ||
            peerChallenge.serverNonce != challenge_.serverNonce || !ValidTransportEvidence(evidence, challenge_, policy_)) {
            (void)Reject(NetworkErrors::AuthenticationIncompatible, AuthenticationFailureClass::Incompatible);
            return Result<ProofIssuanceAuthorization>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        }
        const auto peer = VerifyPeerAuthorities(evidence, nowTick);
        if (peer.HasError())
            return Result<ProofIssuanceAuthorization>::Failure(peer.ErrorValue());
        proofAuthorization_ =
            ProofIssuanceAuthorization{peerChallenge, evidence, peer.Value().peer, peer.Value().certificate.stamp, deadlineTick_};
        return Result<ProofIssuanceAuthorization>::Success(*proofAuthorization_);
    }

    /** @copydoc AuthenticationSessionAdapter::VerifyWithAuthorities */
    Result<AuthenticationResult> AuthenticationSessionAdapter::VerifyWithAuthorities(const AuthenticationResponseView &response,
                                                                                     const PeerAuthenticationEvidence &evidence,
                                                                                     const std::uint64_t nowTick) {
        auto attempted = Detail::InvokeAdmissionProvider([this, &response, &evidence, nowTick]() {
            const auto peer = VerifyPeerAuthorities(evidence, nowTick);
            if (peer.HasError())
                return Result<AuthenticationResult>::Failure(peer.ErrorValue());
            if (proofAuthorization_ && (!SameEvidence(evidence, proofAuthorization_->Evidence()) ||
                                        peer.Value().certificate.stamp != proofAuthorization_->certificateStamp_ ||
                                        peer.Value().peer.stamp != proofAuthorization_->Peer().stamp ||
                                        peer.Value().peer.trustLevel != proofAuthorization_->Peer().trustLevel))
                return Reject(NetworkErrors::AuthenticationIncompatible, AuthenticationFailureClass::Incompatible);
            const auto chargeVerifier = [this, nowTick]() {
                return protection_ == nullptr ||
                       protection_->Charge(challenge_.connection, challenge_.sessionGeneration, AdmissionWork::VerifierCall, 1, nowTick)
                           .HasValue();
            };
            if (!chargeVerifier())
                return Reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
            const auto credential =
                authorities_.credentials->Verify({challenge_, policy_.credential, peer.Value().peer.trustLevel, response.proof, nowTick});
            if (credential.HasError())
                return Reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
            if (!ValidStamp(credential.Value().stamp, challenge_) || credential.Value().binding != policy_.credential ||
                !ValidPrincipal(credential.Value().principal, peer.Value().peer.trustLevel, nowTick))
                return Reject(NetworkErrors::AuthenticationInvalid, AuthenticationFailureClass::Malformed);

            if (!chargeVerifier())
                return Reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
            const auto channel = authorities_.privateKeys->Bind(
                {challenge_, policy_.privateKey, evidence.transport, credential.Value().stamp, credential.Value().principal.session});
            if (channel.HasError())
                return Reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
            if (!ValidStamp(channel.Value().stamp, challenge_) || channel.Value().binding != policy_.privateKey ||
                channel.Value().channel != evidence.transport.channel ||
                channel.Value().channelGeneration != evidence.transport.channelGeneration ||
                !Detail::HasNonZeroByte(channel.Value().bindingDigest))
                return Reject(NetworkErrors::AuthenticationInvalid, AuthenticationFailureClass::Malformed);

            accepted_ = AuthenticationResult{challenge_.connection, challenge_.sessionGeneration, policy_.id,
                                             policy_.revision,      credential.Value().principal, channel.Value()};
            state_ = AuthenticationState::Accepted;
            failure_ = AuthenticationFailureClass::None;
            return Result<AuthenticationResult>::Success(accepted_);
        });
        if (!attempted)
            return Reject(NetworkErrors::AuthenticationRejected, AuthenticationFailureClass::Rejected);
        return std::move(*attempted);
    }

    /** @copydoc AuthenticationSessionAdapter::Authenticate */
    Result<AuthenticationResult> AuthenticationSessionAdapter::Authenticate(
        const ConnectionHandle connection, const NetworkOperationGeneration sessionGeneration, const AuthenticationResponseView &response,
        const PeerAuthenticationEvidence &evidence, const std::uint64_t nowTick, const TransportAdmissionState operation) {
        if (!Owns(connection, sessionGeneration))
            return Result<AuthenticationResult>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (state_ == AuthenticationState::ShuttingDown)
            return Result<AuthenticationResult>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        if (state_ != AuthenticationState::AwaitingProof)
            return Result<AuthenticationResult>::Failure(MakeError(NetworkErrors::AuthenticationStateInvalid));
        if (operation == TransportAdmissionState::Cancelled)
            return Reject(NetworkErrors::SessionCancelled, AuthenticationFailureClass::Cancelled);
        if (operation == TransportAdmissionState::ShuttingDown) {
            state_ = AuthenticationState::ShuttingDown;
            failure_ = AuthenticationFailureClass::Shutdown;
            return Result<AuthenticationResult>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        }
        if (operation != TransportAdmissionState::Accepting || nowTick == 0)
            return Reject(NetworkErrors::AuthenticationInvalid, AuthenticationFailureClass::Malformed);
        if (nowTick >= deadlineTick_) {
            state_ = AuthenticationState::TimedOut;
            failure_ = AuthenticationFailureClass::TimedOut;
            return Result<AuthenticationResult>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        }
        if (protection_ != nullptr &&
            (protection_->Charge(connection, sessionGeneration, AdmissionWork::AuthenticationAttempt, 1, nowTick).HasError() ||
             (!response.proof.empty() &&
              protection_->Charge(connection, sessionGeneration, AdmissionWork::ParsedBytes, response.proof.size(), nowTick).HasError())))
            return Reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
        if (response.contractVersion != AuthenticationContractVersion || response.proof.empty() ||
            response.proof.size() > policy_.maximumProofBytes) {
            if (protection_ != nullptr &&
                protection_->Charge(connection, sessionGeneration, AdmissionWork::ParseFailure, 1, nowTick).HasError())
                return Reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
            return Reject(NetworkErrors::AuthenticationInvalid, AuthenticationFailureClass::Malformed);
        }
        if (response.policy != policy_.id || response.policyRevision != policy_.revision ||
            response.transcriptDigest != challenge_.transcriptDigest) {
            if (protection_ != nullptr &&
                protection_->Charge(connection, sessionGeneration, AdmissionWork::ParseFailure, 1, nowTick).HasError())
                return Reject(NetworkErrors::AdmissionLimitExceeded, AuthenticationFailureClass::ResourceLimited);
            return Reject(NetworkErrors::AuthenticationIncompatible, AuthenticationFailureClass::Incompatible);
        }
        if (!ValidTransportEvidence(evidence, challenge_, policy_))
            return Reject(NetworkErrors::AuthenticationIncompatible, AuthenticationFailureClass::Incompatible);
        state_ = AuthenticationState::Authenticating;
        return VerifyWithAuthorities(response, evidence, nowTick);
    }

    /** @copydoc AuthenticationSessionAdapter::ValidateActivationEvidence */
    Result<void> AuthenticationSessionAdapter::ValidateActivationEvidence(const ConnectionHandle connection,
                                                                          const NetworkOperationGeneration sessionGeneration,
                                                                          const PeerAuthenticationEvidence &evidence,
                                                                          const std::uint64_t nowTick) {
        if (!Owns(connection, sessionGeneration))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkLifecycleOperationStale));
        if (state_ != AuthenticationState::Accepted)
            return Result<void>::Failure(MakeError(NetworkErrors::AuthenticationStateInvalid));
        if (nowTick == 0 || nowTick >= deadlineTick_ || nowTick >= accepted_.principal.expiresAtTick) {
            state_ = AuthenticationState::TimedOut;
            failure_ = AuthenticationFailureClass::TimedOut;
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        }
        if (!ValidTransportEvidence(evidence, challenge_, policy_) || evidence.transport.channel != accepted_.secureChannel.channel ||
            evidence.transport.channelGeneration != accepted_.secureChannel.channelGeneration ||
            (proofAuthorization_ && !proofAuthorization_->MatchesEvidence(evidence, nowTick))) {
            (void)Reject(NetworkErrors::AuthenticationIncompatible, AuthenticationFailureClass::Incompatible);
            return Result<void>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        }
        const auto peer = VerifyPeerAuthorities(evidence, nowTick);
        if (peer.HasError())
            return Result<void>::Failure(peer.ErrorValue());
        if (proofAuthorization_ && (peer.Value().certificate.stamp != proofAuthorization_->certificateStamp_ ||
                                    peer.Value().peer.stamp != proofAuthorization_->Peer().stamp ||
                                    peer.Value().peer.trustLevel != proofAuthorization_->Peer().trustLevel)) {
            (void)Reject(NetworkErrors::AuthenticationIncompatible, AuthenticationFailureClass::Incompatible);
            return Result<void>::Failure(MakeError(NetworkErrors::AuthenticationIncompatible));
        }
        return Result<void>::Success();
    }

    /** @copydoc AuthenticationSessionAdapter::Expire */
    bool AuthenticationSessionAdapter::Expire(const std::uint64_t nowTick) noexcept {
        if (state_ != AuthenticationState::AwaitingProof || nowTick < deadlineTick_)
            return false;
        state_ = AuthenticationState::TimedOut;
        failure_ = AuthenticationFailureClass::TimedOut;
        return true;
    }

    /** @copydoc AuthenticationSessionAdapter::Shutdown */
    bool AuthenticationSessionAdapter::Shutdown() noexcept {
        if (state_ != AuthenticationState::AwaitingProof)
            return false;
        state_ = AuthenticationState::ShuttingDown;
        failure_ = AuthenticationFailureClass::Shutdown;
        return true;
    }

    /** @copydoc AuthenticationSessionAdapter::Accepted */
    const AuthenticationResult *AuthenticationSessionAdapter::Accepted() const noexcept {
        return state_ == AuthenticationState::Accepted ? &accepted_ : nullptr;
    }

    /** @copydoc AuthenticationSessionAdapter::Diagnostics */
    AuthenticationDiagnostics AuthenticationSessionAdapter::Diagnostics() const noexcept {
        return {challenge_.connection, challenge_.sessionGeneration, policy_.id, policy_.revision, policy_.exposure, state_, failure_};
    }
}  // namespace Horo::Network
