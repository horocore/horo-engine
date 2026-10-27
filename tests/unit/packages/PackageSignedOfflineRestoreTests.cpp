#include "PackageRestoreTestSupport.h"
#include "support/OpenXRProductSigningTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <fstream>

namespace Horo::Tests::PackageRestore {
    namespace {
        using OpenXRProduct::SigningKey;

        class CancellingProvider final : public Security::SignatureProvider {
        public:
            explicit CancellingProvider(CancellationSource &source) : source_(source) {}

            bool Supports(Security::SignatureAlgorithm algorithm) const noexcept override {
                return provider_->Supports(algorithm);
            }

            Result<void> Verify(Security::SignatureAlgorithm algorithm, std::span<const std::byte> key, const Sha256Digest &digest,
                                std::span<const std::byte> signature) const override {
                auto verified = provider_->Verify(algorithm, key, digest, signature);
                source_.RequestCancellation();
                return verified;
            }

        private:
            CancellationSource &source_;
            std::shared_ptr<const Security::SignatureProvider> provider_{Security::CreateMbedTlsSignatureProvider()};
        };

        PackagePublisherVerificationService Publishers(const SigningKey &key, bool revoked = false, std::uint64_t expires = 0,
                                                       std::string keyId = {}, bool allowUnsigned = false) {
            auto publisher = PackagePublisherId::Parse(key.root.publisherId);
            REQUIRE(publisher.HasValue());
            auto value = PackagePublisherVerificationService::Create({.allowUnsigned = allowUnsigned,
                                                                      .publishers = {{.publisher = publisher.Value(),
                                                                                      .keyId = keyId.empty() ? key.root.keyId : keyId,
                                                                                      .algorithm = key.root.algorithm,
                                                                                      .publicKey = key.root.publicKey,
                                                                                      .expiresAtUnixMilliseconds = expires,
                                                                                      .publisherRevoked = revoked}}},
                                                                     Security::CreateMbedTlsSignatureProvider());
            REQUIRE(value.HasValue());
            return std::move(value).Value();
        }

        PackageRestoreRequest SignedRequest(const RestoreFixture &fixture, PackageRestoreMode mode) {
            auto request = fixture.Request(mode);
            request.requirePublisherVerification = true;
            request.nowUnixMilliseconds = 100;
            return request;
        }

        PackageRestoreProgressSnapshot FreshOffline(ServiceFixture &fixture, PackagePublisherVerificationService &publisher,
                                                    PackageRestoreRequest request) {
            auto created = PackageRestoreService::Create(fixture.jobs, fixture.files, fixture.cache, nullptr, &publisher);
            REQUIRE(created.HasValue());
            auto service = std::move(created).Value();
            auto started = service.Start(request);
            REQUIRE(started.HasValue());
            return WaitForTerminal(service, started.Value().Id());
        }
    }  // namespace

    TEST_CASE("Signed offline restore reverifies persisted detached evidence with a fresh current authority",
              "[packages][restore][offline][publisher]") {
        RestoreFixture fixture;
        SigningKey key;
        auto publisher = Publishers(key);
        ServiceFixture service(fixture.bytes, &publisher);
        service.source.signature = key.Sign(fixture.bytes);
        REQUIRE(RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online)).downloadedPackages == 1);
        auto current = Publishers(key);
        const auto restored = FreshOffline(service, current, SignedRequest(fixture, PackageRestoreMode::Offline));
        REQUIRE(restored.outcome == PackageRestoreOutcome::Ready);
        CHECK(restored.cacheHits == 1);
        CHECK(service.source.calls == 1);
        REQUIRE(current.AuditRecords().size() == 1);
        CHECK(current.AuditRecords().front().outcome == PackagePublisherVerificationOutcome::Accepted);
    }

    TEST_CASE("Signed offline restore rejects revoked expired and rotated current trust without a source fallback",
              "[packages][restore][offline][security]") {
        RestoreFixture fixture;
        SigningKey key;
        auto publisher = Publishers(key);
        ServiceFixture service(fixture.bytes, &publisher);
        service.source.signature = key.Sign(fixture.bytes);
        RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online));
        const auto prior = service.service->ActiveGraph();
        const auto variant = GENERATE(0, 1, 2);
        auto current = Publishers(key, variant == 0, variant == 1 ? 200 : 0, variant == 2 ? "rotated-key" : "");
        auto request = SignedRequest(fixture, PackageRestoreMode::Offline);
        request.nowUnixMilliseconds = variant == 1 ? 200 : 100;
        const auto restored = FreshOffline(service, current, request);
        REQUIRE(restored.outcome == PackageRestoreOutcome::Failed);
        REQUIRE(restored.diagnostic);
        CHECK(restored.diagnostic->code.Value() == PackageRestoreErrors::PublisherRejected.code.Value());
        CHECK(service.service->ActiveGraph() == prior);
        CHECK(service.source.calls == 1);
        REQUIRE(current.AuditRecords().size() == 1);
        CHECK_FALSE(current.AuditRecords().front().installPermitted);
    }

    TEST_CASE("Signed offline legacy cache misses fail closed while online restore obtains fresh publisher evidence",
              "[packages][restore][offline][publisher]") {
        RestoreFixture fixture;
        SigningKey key;
        auto publisher = Publishers(key);
        ServiceFixture service(fixture.bytes, &publisher);
        REQUIRE(service.cache.Publish(fixture.archive).HasValue());
        const auto failed = FreshOffline(service, publisher, SignedRequest(fixture, PackageRestoreMode::Offline));
        REQUIRE(failed.outcome == PackageRestoreOutcome::Failed);
        REQUIRE(failed.diagnostic);
        CHECK(failed.diagnostic->code.Value() == PackageRestoreErrors::OfflineArtifactUnavailable.code.Value());
        CHECK(service.source.calls == 0);
        service.source.signature = key.Sign(fixture.bytes);
        RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online));
        CHECK(service.source.calls == 1);
        auto current = Publishers(key);
        CHECK(FreshOffline(service, current, SignedRequest(fixture, PackageRestoreMode::Offline)).outcome == PackageRestoreOutcome::Ready);
    }

    TEST_CASE("Signed offline restore rejects mutated cached detached signatures under the real provider",
              "[packages][restore][offline][security]") {
        RestoreFixture fixture;
        SigningKey key;
        auto publisher = Publishers(key);
        ServiceFixture service(fixture.bytes, &publisher);
        service.source.signature = key.Sign(fixture.bytes);
        RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online));
        auto cached = service.cache.LoadWithPublisherEvidence(fixture.archive.Digest());
        REQUIRE(cached.HasValue());
        REQUIRE(cached.Value());
        REQUIRE(cached.Value()->signature);
        auto signature = *cached.Value()->signature;
        const auto tamper = GENERATE(0, 1, 2);
        if (tamper == 0)
            signature.signature.back() ^= std::byte{1};
        else if (tamper == 1)
            signature.publisherId = "com.horo.unapproved";
        else
            signature.keyId = "unapproved-key";
        REQUIRE(service.cache.Publish(fixture.archive, signature).HasValue());  // Storage does not issue verification authority.
        auto current = Publishers(key);
        const auto restored = FreshOffline(service, current, SignedRequest(fixture, PackageRestoreMode::Offline));
        REQUIRE(restored.outcome == PackageRestoreOutcome::Failed);
        REQUIRE(restored.diagnostic);
        CHECK(restored.diagnostic->code.Value() == PackageRestoreErrors::PublisherRejected.code.Value());
        CHECK(service.source.calls == 1);
    }

    TEST_CASE("Signed restore cancellation after actual verification retains the prior graph without publishing evidence",
              "[packages][restore][cancellation]") {
        RestoreFixture fixture;
        SigningKey key;
        CancellationSource cancellation;
        auto publisher = Publishers(key);
        ServiceFixture service(fixture.bytes, &publisher);
        service.source.signature = key.Sign(fixture.bytes);
        RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online));
        const auto prior = service.service->ActiveGraph();
        REQUIRE(service.cache.Remove(fixture.archive.Digest()).HasValue());
        REQUIRE(publisher.SetProvider(std::make_shared<CancellingProvider>(cancellation)).HasValue());
        auto request = SignedRequest(fixture, PackageRestoreMode::Online);
        request.cancellation = cancellation.Token();
        const auto cancelled = StartAndWait(service, request);
        CHECK(cancelled.outcome == PackageRestoreOutcome::Cancelled);
        CHECK(service.service->ActiveGraph() == prior);
        auto cached = service.cache.LoadWithPublisherEvidence(fixture.archive.Digest());
        REQUIRE(cached.HasValue());
        CHECK_FALSE(cached.Value());
        REQUIRE(publisher.SetProvider(Security::CreateMbedTlsSignatureProvider()).HasValue());
        CHECK(RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online)).outcome == PackageRestoreOutcome::Ready);
    }

    TEST_CASE("Legacy unsigned restore without a verifier ignores malformed optional publisher sidecars", "[packages][restore][offline]") {
        RestoreFixture fixture;
        ServiceFixture service(fixture.bytes);
        REQUIRE(service.cache.Publish(fixture.archive).HasValue());
        const auto path = service.temporary.Path() / "cache" / "by-hash" / "sha256" / FormatSha256(fixture.archive.Digest()).substr(7) /
                          "publisher-envelope.bin";
        {
            std::ofstream output{path, std::ios::binary};
            output << "invalid optional evidence";
        }
        const auto restored = RestoreReady(service, fixture.Request(PackageRestoreMode::Offline));
        CHECK(restored.cacheHits == 1);
        CHECK(service.source.calls == 0);
        CHECK(service.cache.LoadWithPublisherEvidence(fixture.archive.Digest()).HasError());
    }

    TEST_CASE("Configured publisher policy still decides unsigned cache hits without detached evidence",
              "[packages][restore][offline][publisher]") {
        RestoreFixture fixture;
        SigningKey key;
        const bool permitted = GENERATE(false, true);
        auto publisher = Publishers(key, false, 0, {}, permitted);
        ServiceFixture service(fixture.bytes, &publisher);
        REQUIRE(service.cache.Publish(fixture.archive).HasValue());
        const auto restored = StartAndWait(service, fixture.Request(PackageRestoreMode::Offline));
        REQUIRE(publisher.AuditRecords().size() == 1);
        CHECK(publisher.AuditRecords().front().outcome == PackagePublisherVerificationOutcome::Unsigned);
        CHECK(publisher.AuditRecords().front().installPermitted == permitted);
        CHECK(service.source.calls == 0);
        if (permitted) {
            REQUIRE(restored.outcome == PackageRestoreOutcome::Ready);
            REQUIRE(service.service->ActiveGraph());
            REQUIRE(service.service->ActiveGraph()->packages.front().publisher);
            CHECK(service.service->ActiveGraph()->packages.front().publisher->outcome == PackagePublisherVerificationOutcome::Unsigned);
        } else {
            CHECK(restored.outcome == PackageRestoreOutcome::Failed);
            CHECK_FALSE(service.service->ActiveGraph());
        }
    }

    TEST_CASE("Cached signed verification cancellation preserves the prior graph and never invokes an online source",
              "[packages][restore][offline][cancellation]") {
        RestoreFixture fixture;
        SigningKey key;
        CancellationSource cancellation;
        auto publisher = Publishers(key);
        ServiceFixture service(fixture.bytes, &publisher);
        service.source.signature = key.Sign(fixture.bytes);
        RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Online));
        const auto prior = service.service->ActiveGraph();
        REQUIRE(publisher.SetProvider(std::make_shared<CancellingProvider>(cancellation)).HasValue());
        auto request = SignedRequest(fixture, PackageRestoreMode::Offline);
        request.cancellation = cancellation.Token();
        const auto cancelled = StartAndWait(service, request);
        CHECK(cancelled.outcome == PackageRestoreOutcome::Cancelled);
        CHECK(service.service->ActiveGraph() == prior);
        CHECK(service.source.calls == 1);
        auto cached = service.cache.LoadWithPublisherEvidence(fixture.archive.Digest());
        REQUIRE(cached.HasValue());
        REQUIRE(cached.Value());
        REQUIRE(cached.Value()->signature);
        CHECK(cached.Value()->signature->signature == service.source.signature->signature);
        REQUIRE(publisher.SetProvider(Security::CreateMbedTlsSignatureProvider()).HasValue());
        CHECK(RestoreReady(service, SignedRequest(fixture, PackageRestoreMode::Offline)).cacheHits == 1);
        CHECK(service.source.calls == 1);
    }
}  // namespace Horo::Tests::PackageRestore
