#include "Horo/Network/AdmissionProtection.h"
#include "NetworkTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Network {
    using TestSupport::Bytes;
    using TestSupport::Connection;
    using TestSupport::Id;
    using TestSupport::RequireError;
    using TestSupport::Session;
    using TestSupport::WireIdentity;

    namespace {
        [[nodiscard]] AdmissionProtectionPolicy Policy() {
            return {{1, 2}, 2, 1, 4, 2, 4, 2, 64, 10};
        }

        [[nodiscard]] HandshakeSelection Selection(const std::uint32_t connectionGeneration = 3,
                                                   const std::uint64_t sessionGeneration = 7) {
            HandshakeSelection selection;
            selection.connection = Connection(connectionGeneration);
            selection.sessionGeneration = Session(sessionGeneration);
            selection.protocol = WireIdentity<ProtocolId>(1);
            selection.version = {1, 2};
            selection.schemaFingerprint = 42;
            return selection;
        }

        [[nodiscard]] AuthenticationChallenge Challenge(const HandshakeSelection &selection, const std::uint8_t nonceSeed) {
            AuthenticationChallenge challenge;
            challenge.connection = selection.connection;
            challenge.sessionGeneration = selection.sessionGeneration;
            challenge.policy = Id<NetworkTrustPolicyId>(10);
            challenge.policyRevision = 4;
            challenge.clientNonce = Bytes<AuthenticationNonceBytes>(nonceSeed + 32);
            challenge.serverNonce = Bytes<AuthenticationNonceBytes>(nonceSeed + 64);
            challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selection, challenge);
            return challenge;
        }
    }  // namespace

    TEST_CASE("Admission rejects downgraded, unbound and replayed challenges before reserving work", "[unit][network][admission]") {
        auto created = AdmissionProtection::Create(Policy());
        REQUIRE(created.HasValue());
        auto protection = std::move(created).Value();
        auto selection = Selection();
        auto challenge = Challenge(selection, 1);
        REQUIRE(ComputeAdmissionTranscriptDigest(selection, challenge) == challenge.transcriptDigest);
        auto changedNonce = challenge;
        changedNonce.serverNonce[0] ^= std::byte{0xff};
        REQUIRE(ComputeAdmissionTranscriptDigest(selection, changedNonce) != challenge.transcriptDigest);
        auto changedSelection = selection;
        changedSelection.version.minor = 3;
        REQUIRE(ComputeAdmissionTranscriptDigest(changedSelection, challenge) != challenge.transcriptDigest);

        challenge.transcriptDigest[0] ^= std::byte{0xff};
        RequireError(protection.Begin({1}, selection, challenge, 1), NetworkErrors::AdmissionBindingInvalid);
        REQUIRE(protection.Pending() == 0);
        challenge = Challenge(selection, 1);

        selection.version = {1, 1};
        RequireError(protection.Begin({1}, selection, challenge, 1), NetworkErrors::AdmissionBindingInvalid);
        challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selection, challenge);
        RequireError(protection.Begin({1}, selection, challenge, 1), NetworkErrors::AdmissionDowngradeRejected);
        selection.version = {1, 2};
        challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selection, challenge);
        REQUIRE(protection.Begin({1}, selection, challenge, 1).HasValue());
        REQUIRE(protection.End(selection.connection, selection.sessionGeneration).HasValue());

        auto replacement = Selection(4, 8);
        challenge.connection = replacement.connection;
        challenge.sessionGeneration = replacement.sessionGeneration;
        challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(replacement, challenge);
        RequireError(protection.Begin({1}, replacement, challenge, 2), NetworkErrors::AdmissionReplayRejected);
        REQUIRE(protection.Pending() == 0);
    }

    TEST_CASE("Admission bounds concurrent sources, work and diagnostics then fences shutdown", "[unit][network][admission]") {
        auto created = AdmissionProtection::Create(Policy());
        REQUIRE(created.HasValue());
        auto protection = std::move(created).Value();
        const auto first = Selection();
        const auto firstChallenge = Challenge(first, 1);
        REQUIRE(protection.Begin({1}, first, firstChallenge, 1).HasValue());

        const auto second = Selection(4, 8);
        const auto secondChallenge = Challenge(second, 2);
        RequireError(protection.Begin({1}, second, secondChallenge, 2), NetworkErrors::AdmissionLimitExceeded);
        REQUIRE(protection.Begin({2}, second, secondChallenge, 2).HasValue());
        const auto third = Selection(5, 9);
        const auto thirdChallenge = Challenge(third, 3);
        RequireError(protection.Begin({3}, third, thirdChallenge, 3), NetworkErrors::AdmissionLimitExceeded);

        REQUIRE(protection.Charge(first.connection, first.sessionGeneration, AdmissionWork::ParsedBytes, 64, 3).HasValue());
        RequireError(protection.Charge(second.connection, second.sessionGeneration, AdmissionWork::ParsedBytes, 1, 3),
                     NetworkErrors::AdmissionLimitExceeded);
        REQUIRE(protection.Pending() == 1);
        REQUIRE(protection.Charge(first.connection, first.sessionGeneration, AdmissionWork::Diagnostic, 1, 3).HasValue());
        REQUIRE(protection.Charge(first.connection, first.sessionGeneration, AdmissionWork::Diagnostic, 1, 3).HasValue());
        RequireError(protection.Charge(first.connection, first.sessionGeneration, AdmissionWork::Diagnostic, 1, 3),
                     NetworkErrors::AdmissionLimitExceeded);
        REQUIRE(protection.Pending() == 0);

        REQUIRE(protection.Begin({3}, third, thirdChallenge, 13).HasValue());
        protection.Shutdown();
        REQUIRE(protection.Pending() == 0);
        RequireError(protection.Charge(third.connection, third.sessionGeneration, AdmissionWork::VerifierCall, 1, 14),
                     NetworkErrors::SessionShuttingDown);
        RequireError(protection.Begin({3}, third, thirdChallenge, 14), NetworkErrors::SessionShuttingDown);
    }

    TEST_CASE("Admission policy and owner clock reject malformed or stale work", "[unit][network][admission]") {
        auto invalid = Policy();
        invalid.maximumPending = MaximumProtectedAdmissions + 1;
        RequireError(AdmissionProtection::Create(invalid), NetworkErrors::AdmissionPolicyInvalid);

        auto created = AdmissionProtection::Create(Policy());
        REQUIRE(created.HasValue());
        auto protection = std::move(created).Value();
        const auto selection = Selection();
        const auto challenge = Challenge(selection, 1);
        REQUIRE(protection.Begin({1}, selection, challenge, 5).HasValue());
        RequireError(protection.Charge(selection.connection, Session(8), AdmissionWork::VerifierCall, 1, 5),
                     NetworkErrors::NetworkLifecycleOperationStale);
        RequireError(protection.Charge(selection.connection, selection.sessionGeneration, AdmissionWork::VerifierCall, 1, 4),
                     NetworkErrors::AdmissionPolicyInvalid);
        REQUIRE(protection.Pending() == 1);
    }
}  // namespace Horo::Network
