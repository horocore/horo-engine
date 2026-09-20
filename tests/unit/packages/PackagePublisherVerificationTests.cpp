#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Packages/PackagePublisherVerification.h"
#include "Horo/Packages/PackagePublisherVerificationErrors.h"
#include "Horo/Security/SecurityErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string_view>
#include <utility>

namespace Horo::Packages::Tests {
    namespace {
        class AcceptingProvider final : public Security::SignatureProvider {
        public:
            [[nodiscard]] bool Supports(const Security::SignatureAlgorithm algorithm) const noexcept override {
                return algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256;
            }

            [[nodiscard]] Result<void> Verify(Security::SignatureAlgorithm, std::span<const std::byte>, const Sha256Digest &,
                                              std::span<const std::byte>) const override {
                return Result<void>::Success();
            }
        };

        class RejectingProvider final : public Security::SignatureProvider {
        public:
            [[nodiscard]] bool Supports(const Security::SignatureAlgorithm algorithm) const noexcept override {
                return algorithm == Security::SignatureAlgorithm::EcdsaP256Sha256;
            }

            [[nodiscard]] Result<void> Verify(Security::SignatureAlgorithm, std::span<const std::byte>, const Sha256Digest &,
                                              std::span<const std::byte>) const override {
                return Result<void>::Failure(MakeError(SecurityErrors::InvalidSignature));
            }
        };

        struct Fixture final {
            HoroPackageId package;
            PackagePublisherId publisher;
            std::vector<std::byte> artifact;
            Security::DetachedSignatureEnvelope envelope;
        };

        [[nodiscard]] Fixture MakeFixture() {
            Fixture fixture{.package = HoroPackageId::Parse("com.horo.fixture").Value(),
                            .publisher = PackagePublisherId::Parse("com.horo.publisher").Value(),
                            .artifact = {std::byte{1}, std::byte{2}, std::byte{3}}};
            fixture.envelope.publisherId = fixture.publisher.Value();
            fixture.envelope.keyId = "release-1";
            fixture.envelope.artifactDigest = ComputeSha256(fixture.artifact);
            fixture.envelope.signature = {std::byte{9}};
            return fixture;
        }

        [[nodiscard]] PackagePublisherTrustRecord Trust(const Fixture &fixture, const bool revoked = false,
                                                        const std::uint64_t expiresAt = 0) {
            return {.publisher = fixture.publisher,
                    .keyId = fixture.envelope.keyId,
                    .algorithm = Security::SignatureAlgorithm::EcdsaP256Sha256,
                    .publicKey = {std::byte{7}},
                    .expiresAtUnixMilliseconds = expiresAt,
                    .publisherRevoked = revoked};
        }

        [[nodiscard]] PackagePublisherVerificationRequest Request(const Fixture &fixture,
                                                                  const std::optional<PackagePublisherId> &expected = std::nullopt) {
            return {.package = fixture.package,
                    .artifact = fixture.artifact,
                    .signature = fixture.envelope,
                    .expectedPublisher = expected,
                    .nowUnixMilliseconds = 100};
        }

        [[nodiscard]] PackagePublisherVerificationService Create(const Fixture &,
                                                                 std::shared_ptr<const Security::SignatureProvider> provider,
                                                                 std::vector<PackagePublisherTrustRecord> publishers,
                                                                 const bool allowUnsigned = false, const std::size_t auditCapacity = 32) {
            auto service = PackagePublisherVerificationService::Create({.allowUnsigned = allowUnsigned,
                                                                        .publishers = std::move(publishers),
                                                                        .auditCapacity = auditCapacity},
                                                                       std::move(provider));
            REQUIRE(service.HasValue());
            return std::move(service).Value();
        }
    }  // namespace

    TEST_CASE("Package publisher verification records every explicit policy outcome", "[packages][publisher]") {
        const Fixture fixture = MakeFixture();
        auto service = Create(fixture, std::make_shared<AcceptingProvider>(), {Trust(fixture)});

        auto accepted = service.Verify(Request(fixture, fixture.publisher));
        REQUIRE(accepted.HasValue());
        CHECK(accepted.Value().decision.outcome == PackagePublisherVerificationOutcome::Accepted);
        CHECK(accepted.Value().decision.installPermitted);

        auto unsignedRequest = Request(fixture);
        unsignedRequest.signature.reset();
        auto unsignedResult = service.Verify(unsignedRequest);
        REQUIRE(unsignedResult.HasValue());
        CHECK(unsignedResult.Value().decision.outcome == PackagePublisherVerificationOutcome::Unsigned);
        CHECK_FALSE(unsignedResult.Value().decision.installPermitted);

        auto revoked = Create(fixture, std::make_shared<AcceptingProvider>(), {Trust(fixture, true)}).Verify(Request(fixture));
        REQUIRE(revoked.HasValue());
        CHECK(revoked.Value().decision.outcome == PackagePublisherVerificationOutcome::RevokedPublisher);
        CHECK(revoked.Value().audit.outcome == PackagePublisherVerificationOutcome::RevokedPublisher);

        auto expired = Create(fixture, std::make_shared<AcceptingProvider>(), {Trust(fixture, false, 100)}).Verify(Request(fixture));
        REQUIRE(expired.HasValue());
        CHECK(expired.Value().decision.outcome == PackagePublisherVerificationOutcome::ExpiredPublisher);
        CHECK(expired.Value().audit.outcome == PackagePublisherVerificationOutcome::ExpiredPublisher);

        auto mismatched = service.Verify(Request(fixture, PackagePublisherId::Parse("com.other.publisher").Value()));
        REQUIRE(mismatched.HasValue());
        CHECK(mismatched.Value().decision.outcome == PackagePublisherVerificationOutcome::MismatchedPublisher);

        auto unknownFixture = fixture;
        unknownFixture.envelope.publisherId = "com.unknown.publisher";
        auto unknown = service.Verify(Request(unknownFixture));
        REQUIRE(unknown.HasValue());
        CHECK(unknown.Value().decision.outcome == PackagePublisherVerificationOutcome::UnknownPublisher);

        CHECK(service.AuditRecords().size() == 4);
        CHECK(service.AuditRecords().back().outcome == PackagePublisherVerificationOutcome::UnknownPublisher);
    }

    TEST_CASE("Package publisher verification rejects malformed, oversized and unsupported inputs", "[packages][publisher]") {
        const Fixture fixture = MakeFixture();
        auto service = Create(fixture, std::make_shared<AcceptingProvider>(), {Trust(fixture)});

        auto malformed = Request(fixture);
        malformed.signature->signature.clear();
        auto malformedResult = service.Verify(malformed);
        REQUIRE(malformedResult.HasValue());
        CHECK(malformedResult.Value().decision.outcome == PackagePublisherVerificationOutcome::MalformedInput);

        auto unsupported = Request(fixture);
        unsupported.signature->algorithm = static_cast<Security::SignatureAlgorithm>(255U);
        auto unsupportedResult = service.Verify(unsupported);
        REQUIRE(unsupportedResult.HasValue());
        CHECK(unsupportedResult.Value().decision.outcome == PackagePublisherVerificationOutcome::UnsupportedAlgorithm);

        auto oversizedPolicy =
            PackagePublisherVerificationPolicy{.publishers = {Trust(fixture)}, .limits = {.maximumArtifactBytes = 2}, .auditCapacity = 32};
        auto oversizedService = PackagePublisherVerificationService::Create(oversizedPolicy, std::make_shared<AcceptingProvider>());
        REQUIRE(oversizedService.HasValue());
        auto oversizedVerificationService = std::move(oversizedService).Value();
        const auto oversized = oversizedVerificationService.Verify(Request(fixture));
        REQUIRE(oversized.HasError());
        CHECK(oversized.ErrorValue().code.Value() == PublisherVerificationErrors::ResourceLimit.code.Value());
        CHECK(oversizedVerificationService.AuditRecords().empty());

        auto invalidPolicy = PackagePublisherVerificationPolicy{.publishers = {Trust(fixture)}, .auditCapacity = 0};
        CHECK(PackagePublisherVerificationService::Create(invalidPolicy, std::make_shared<AcceptingProvider>()).ErrorValue().code.Value() ==
              PublisherVerificationErrors::InvalidInput.code.Value());
    }

    TEST_CASE("Package publisher verification recovers after provider failure and preserves no partial state", "[packages][publisher]") {
        const Fixture fixture = MakeFixture();
        auto service = Create(fixture, nullptr, {Trust(fixture)}, false, 2);

        auto unavailable = service.Verify(Request(fixture));
        REQUIRE(unavailable.HasValue());
        CHECK(unavailable.Value().decision.outcome == PackagePublisherVerificationOutcome::VerificationUnavailable);
        CHECK(service.State().lifecycle == PackagePublisherVerificationLifecycleState::RecoverableFailure);
        REQUIRE(service.SetProvider(std::make_shared<AcceptingProvider>()).HasValue());

        auto retried = service.Verify(Request(fixture));
        REQUIRE(retried.HasValue());
        CHECK(retried.Value().decision.outcome == PackagePublisherVerificationOutcome::Accepted);
        CHECK(service.State().lifecycle == PackagePublisherVerificationLifecycleState::Ready);
        CHECK(service.State().attempts == 2);
        CHECK(service.AuditRecords().size() == 2);

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto cancelledRequest = Request(fixture);
        cancelledRequest.cancellation = cancellation.Token();
        const auto beforeCancelled = service.State();
        auto cancelled = service.Verify(cancelledRequest);
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == PublisherVerificationErrors::Cancelled.code.Value());
        CHECK(service.State().revision == beforeCancelled.revision);
        CHECK(service.State().attempts == beforeCancelled.attempts);
        CHECK(service.AuditRecords().size() == 2);

        REQUIRE(service.Shutdown().HasValue());
        const auto closedState = service.State();
        auto closed = service.Verify(Request(fixture));
        REQUIRE(closed.HasError());
        CHECK(closed.ErrorValue().code.Value() == PublisherVerificationErrors::LifecycleClosed.code.Value());
        CHECK(service.State().revision == closedState.revision);

        auto invalidSignature = Create(fixture, std::make_shared<RejectingProvider>(), {Trust(fixture)}).Verify(Request(fixture));
        REQUIRE(invalidSignature.HasValue());
        CHECK(invalidSignature.Value().decision.outcome == PackagePublisherVerificationOutcome::InvalidSignature);
    }

    TEST_CASE("Package publisher audit capacity does not publish a partial transition", "[packages][publisher]") {
        const Fixture fixture = MakeFixture();
        auto service = Create(fixture, std::make_shared<AcceptingProvider>(), {Trust(fixture)}, false, 1);
        REQUIRE(service.Verify(Request(fixture)).HasValue());
        const auto before = service.State();
        const auto second = service.Verify(Request(fixture));
        REQUIRE(second.HasError());
        CHECK(second.ErrorValue().code.Value() == PublisherVerificationErrors::AuditCapacityExceeded.code.Value());
        CHECK(service.State().revision == before.revision);
        CHECK(service.State().attempts == before.attempts);
        CHECK(service.AuditRecords().size() == 1);
    }
}  // namespace Horo::Packages::Tests
