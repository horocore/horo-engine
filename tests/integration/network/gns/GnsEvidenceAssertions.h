#pragma once

/** @file
 * @brief Private native-qualification observations and crossbinding assertions; never an authentication authority.
 */
#include "Horo/Network/NetworkErrors.h"
#include "NetworkProductGnsSelection.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <thread>

namespace Horo::Network::NativeQualification {
    /** @brief Observe only a successful real credential verification; no proof or secret is retained. */
    struct ObservedCredentialGrant final {
        AuthenticationChallenge challenge;
        CredentialVerificationResult result;
    };

    struct CredentialObservation final {
        std::optional<ObservedCredentialGrant> grant;
    };

    /** @brief Forward to the same credential owner unchanged and retain its genuine accepted public grant for assertions. */
    class ObservedCredential final : public ICredentialAuthority {
    public:
        ObservedCredential(std::shared_ptr<ICredentialAuthority> owner, std::shared_ptr<CredentialObservation> observation)
            : owner_(std::move(owner)), observation_(std::move(observation)) {}

        bool Available() const noexcept override {
            return owner_->Available();
        }

        Result<CredentialVerificationResult> Verify(const CredentialVerificationRequest &request) override {
            auto verified = owner_->Verify(request);
            if (verified.HasValue())
                observation_->grant.emplace(ObservedCredentialGrant{request.challenge, verified.Value()});
            return verified;
        }

    private:
        std::shared_ptr<ICredentialAuthority> owner_;
        std::shared_ptr<CredentialObservation> observation_;
    };

    inline void RequireNativeEvidenceError(const Error &error, const ErrorCodeDescriptor &expected) {
        REQUIRE(error.domain.Value() == expected.domain.Value());
        REQUIRE(error.code.Value() == expected.code.Value());
    }

    /** @brief Alter exactly one protection fence from real protected-channel evidence. */
    inline TransportProtectionEvidence AlterProtection(TransportProtectionEvidence value, const unsigned field) {
        switch (field) {
            case 0:
                value.connection = {};
                break;
            case 1:
                value.sessionGeneration = {};
                break;
            case 2:
                value.channel = {};
                break;
            case 3:
                ++value.channelGeneration;
                break;
            case 4:
                value.confidentiality = false;
                break;
            case 5:
                value.integrity = false;
                break;
            case 6:
                value.authenticatedPeer = false;
                break;
            case 7:
                value.inMemoryLoopback = true;
                break;
            default:
                FAIL("unknown protection fence");
        }
        return value;
    }

    inline void RequireChallengeFences(const NetworkAdmissionProviders &providers, const AuthenticationChallenge &challenge,
                                       const PeerCertificateEvidence &certificate, const std::uint64_t tick) {
        for (unsigned field = 0; field < 5; ++field) {
            auto changed = challenge;
            switch (field) {
                case 0:
                    ++changed.contractVersion;
                    break;
                case 1:
                    changed.policy = NetworkTrustPolicyId::Create(challenge.policy.Value() + 1).Value();
                    break;
                case 2:
                    ++changed.policyRevision;
                    break;
                case 3:
                    changed.connection = {};
                    break;
                case 4:
                    changed.sessionGeneration = {};
                    break;
            }
            const auto rejected = providers.certificates->Verify({changed, certificate});
            REQUIRE(rejected.HasError());
            RequireNativeEvidenceError(rejected.ErrorValue(), NetworkErrors::AuthenticationTrustUnavailable);
        }
        std::optional<Result<PeerAuthenticationEvidence>> foreign;
        std::thread worker([&providers, &challenge, tick, &foreign] {
            foreign.emplace(providers.channel->Inspect(challenge.connection, challenge.sessionGeneration, tick));
        });
        worker.join();
        REQUIRE(foreign.has_value());
        REQUIRE(foreign->HasError());
        RequireNativeEvidenceError(foreign->ErrorValue(), NetworkErrors::AuthenticationTrustUnavailable);
        REQUIRE(providers.channel->Inspect(challenge.connection, challenge.sessionGeneration, tick).HasValue());
    }

    inline void RequireCertificateFences(const NetworkAdmissionProviders &providers, const AuthenticationChallenge &challenge,
                                         const PeerAuthenticationEvidence &evidence, const CertificateVerificationResult &certificate) {
        for (unsigned field = 0; field < 3; ++field) {
            auto changed = evidence.certificate;
            if (field == 0)
                changed.binding = CertificateBindingId::Create(changed.binding.Value() + 1).Value();
            else if (field == 1)
                ++changed.evidenceGeneration;
            else
                changed.certificateDigest[0] ^= std::byte{1};
            const auto rejected = providers.certificates->Verify({challenge, changed});
            REQUIRE(rejected.HasError());
            RequireNativeEvidenceError(rejected.ErrorValue(), NetworkErrors::AuthenticationIncompatible);
        }
        for (unsigned field = 0; field < 5; ++field) {
            PeerVerificationRequest changed{challenge, evidence, certificate};
            switch (field) {
                case 0:
                    ++changed.certificate.stamp.authorityGeneration;
                    break;
                case 1:
                    changed.certificate.binding = CertificateBindingId::Create(certificate.binding.Value() + 1).Value();
                    break;
                case 2:
                    changed.evidence.certificate.binding = CertificateBindingId::Create(certificate.binding.Value() + 1).Value();
                    break;
                case 3:
                    ++changed.evidence.certificate.evidenceGeneration;
                    break;
                case 4:
                    changed.evidence.certificate.certificateDigest[0] ^= std::byte{1};
                    break;
            }
            const auto rejected = providers.peers->Verify(changed);
            REQUIRE(rejected.HasError());
            RequireNativeEvidenceError(rejected.ErrorValue(), NetworkErrors::AuthenticationIncompatible);
        }
    }

    inline void RequireBindingFences(const NetworkAdmissionProviders &providers, const SecureChannelBindingRequest &binding) {
        for (unsigned field = 0; field < 5; ++field) {
            auto changed = binding;
            switch (field) {
                case 0:
                    changed.binding = PrivateKeyBindingId::Create(binding.binding.Value() + 1).Value();
                    break;
                case 1:
                    changed.session = {};
                    break;
                case 2:
                    changed.credentialStamp.connection = {};
                    break;
                case 3:
                    changed.credentialStamp.sessionGeneration = {};
                    break;
                case 4:
                    changed.credentialStamp.authorityGeneration = 0;
                    break;
            }
            const auto rejected = providers.privateKeys->Bind(changed);
            REQUIRE(rejected.HasError());
            RequireNativeEvidenceError(rejected.ErrorValue(), NetworkErrors::AuthenticationIncompatible);
        }
    }

    inline void RequireProtectionFences(const NetworkAdmissionProviders &providers, const PeerAuthenticationEvidence &evidence,
                                        const CertificateVerificationResult &certificate, const SecureChannelBindingRequest &binding) {
        for (unsigned field = 0; field < 8; ++field) {
            const auto changed = AlterProtection(evidence.transport, field);
            const auto peer = providers.peers->Verify({binding.challenge, {changed, evidence.certificate}, certificate});
            REQUIRE(peer.HasError());
            RequireNativeEvidenceError(peer.ErrorValue(), NetworkErrors::AuthenticationIncompatible);
            auto changedBinding = binding;
            changedBinding.transport = changed;
            const auto handoff = providers.privateKeys->Bind(changedBinding);
            REQUIRE(handoff.HasError());
            RequireNativeEvidenceError(handoff.ErrorValue(), NetworkErrors::AuthenticationIncompatible);
        }
    }

    /** @brief Each negative starts from real successful evidence and the actual recorded credential grant, never a synthesized stamp. */
    inline void VerifyNativeEvidence(const InboundMessageContext &context, const Application::Internal::NetworkProductAdmission &admission,
                                     const ObservedCredentialGrant &grant) {
        const auto &providers = admission.Providers();
        const auto &challenge = grant.challenge;
        REQUIRE(challenge.connection == context.connection);
        REQUIRE(challenge.sessionGeneration == context.generation);
        const auto *authenticated = context.session->Authentication();
        REQUIRE(authenticated != nullptr);
        REQUIRE(grant.result.principal.session == authenticated->principal.session);
        auto evidence = providers.channel->Inspect(context.connection, context.generation, context.ownerTick);
        REQUIRE(evidence.HasValue());
        const auto next = NetworkOperationGeneration::Create(context.generation.Value() + 1);
        REQUIRE(next.HasValue());
        REQUIRE(providers.channel->Inspect(context.connection, next.Value(), context.ownerTick).HasError());
        REQUIRE(providers.channel->Inspect(context.connection, context.generation, context.ownerTick).HasValue());
        const auto certificate = providers.certificates->Verify({challenge, evidence.Value().certificate});
        REQUIRE(certificate.HasValue());
        REQUIRE(providers.peers->Verify({challenge, evidence.Value(), certificate.Value()}).HasValue());
        const SecureChannelBindingRequest binding{challenge, admission.Policy().trust.privateKey, evidence.Value().transport,
                                                  grant.result.stamp, authenticated->principal.session};
        REQUIRE(providers.privateKeys->Bind(binding).HasValue());
        RequireChallengeFences(providers, challenge, evidence.Value().certificate, context.ownerTick);
        RequireCertificateFences(providers, challenge, evidence.Value(), certificate.Value());
        RequireBindingFences(providers, binding);
        RequireProtectionFences(providers, evidence.Value(), certificate.Value(), binding);
        REQUIRE(providers.privateKeys->Bind(binding).HasValue());
        REQUIRE(context.session->State() == PeerSessionState::Active);
    }
}  // namespace Horo::Network::NativeQualification
