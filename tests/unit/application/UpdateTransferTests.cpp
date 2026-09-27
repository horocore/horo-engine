#include "Horo/Release/UpdateTransfer.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <span>
#include <utility>
#include <vector>

using namespace Horo::Release;

namespace {
    class AcceptingProvider final : public Horo::Security::SignatureProvider {
    public:
        [[nodiscard]] bool Supports(Horo::Security::SignatureAlgorithm) const noexcept override {
            return true;
        }

        [[nodiscard]] Horo::Result<void> Verify(Horo::Security::SignatureAlgorithm, std::span<const std::byte>, const Horo::Sha256Digest &,
                                                std::span<const std::byte>) const override {
            return Horo::Result<void>::Success();
        }
    };

    [[nodiscard]] UpdatePackageRecord Package() {
        UpdatePackageRecord package;
        package.url = "https://updates.example.test/editor.zip";
        package.size = 100U;
        package.digest.bytes[0] = 42U;
        return package;
    }

    [[nodiscard]] UpdateTransferResponse FreshResponse(const UpdatePackageRecord &package) {
        return {.status = 200U,
                .requestedUrl = package.url,
                .effectiveUrl = package.url,
                .strongEtag = "\"release-42\"",
                .contentLength = package.size};
    }

    [[nodiscard]] UpdateTransferCheckpoint PartialCheckpoint(const UpdatePackageRecord &package) {
        auto first = PlanUpdateTransfer(package, FreshResponse(package), std::nullopt);
        REQUIRE(first.HasValue());
        auto checkpoint = AdvanceUpdateTransfer(first.Value(), 40U);
        REQUIRE(checkpoint.HasValue());
        return std::move(checkpoint).Value();
    }

    [[nodiscard]] UpdateTransferResponse RemainingResponse(const UpdatePackageRecord &package) {
        auto response = FreshResponse(package);
        response.status = 206U;
        response.contentLength = 60U;
        response.rangeStart = 40U;
        response.rangeEnd = 99U;
        response.rangeTotal = 100U;
        return response;
    }
}  // namespace

TEST_CASE("Update download capacity preflight preserves a recovery reserve", "[release][update]") {
    const auto package = Package();
    CHECK(CheckUpdateTransferSpace(package, 200U, 100U, 100U).HasValue());
    CHECK(CheckUpdateTransferSpace(package, 199U, 100U, 100U).HasError());
    CHECK(CheckUpdateTransferSpace(package, 200U, 99U, 0U).HasError());
    CHECK(CheckUpdateTransferSpace(package, 50U, 100U, 100U).HasError());
}

TEST_CASE("Update transfer admits an exact fresh body and durable progress", "[release][update]") {
    const auto package = Package();
    auto response = FreshResponse(package);
    auto plan = PlanUpdateTransfer(package, response, std::nullopt);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().writeOffset == 0U);
    CHECK(plan.Value().responseBytes == package.size);
    CHECK(plan.Value().resumable);
    auto partial = AdvanceUpdateTransfer(plan.Value(), 40U);
    REQUIRE(partial.HasValue());
    CHECK(partial.Value().durableBytes == 40U);
    CHECK(AdvanceUpdateTransfer(plan.Value(), 101U).HasError());
    response.effectiveUrl = "https://mirror.example.test/editor.zip";
    CHECK(PlanUpdateTransfer(package, response, std::nullopt).HasError());
    response = FreshResponse(package);
    response.strongEtag = "W/\"release-42\"";
    CHECK(PlanUpdateTransfer(package, response, std::nullopt).HasError());
}

TEST_CASE("Update resume accepts only an exact strong validator and byte range", "[release][update]") {
    const auto package = Package();
    const auto checkpoint = PartialCheckpoint(package);
    auto response = RemainingResponse(package);
    auto plan = PlanUpdateTransfer(package, response, checkpoint);
    REQUIRE(plan.HasValue());
    CHECK(plan.Value().writeOffset == 40U);
    CHECK(plan.Value().responseBytes == 60U);
    auto complete = AdvanceUpdateTransfer(plan.Value(), 60U);
    REQUIRE(complete.HasValue());
    CHECK(complete.Value().durableBytes == 100U);
    response.strongEtag = "\"changed\"";
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    response = RemainingResponse(package);
    response.rangeStart = 39U;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    response = RemainingResponse(package);
    response.status = 200U;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
}

TEST_CASE("Update resume rejects stale package and checkpoint identities", "[release][update]") {
    auto package = Package();
    auto checkpoint = PartialCheckpoint(package);
    const auto response = RemainingResponse(package);
    package.digest.bytes[0] = 43U;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    package = Package();
    checkpoint.effectiveUrl = "https://mirror.example.test/editor.zip";
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
    checkpoint = PartialCheckpoint(package);
    checkpoint.durableBytes = package.size;
    CHECK(PlanUpdateTransfer(package, response, checkpoint).HasError());
}

TEST_CASE("Completed update bytes require the signed package hash and publisher", "[release][update]") {
    std::array<std::byte, 100U> bytes{};
    auto package = Package();
    package.digest = Horo::ComputeSha256(bytes);
    package.signature = {.publisherId = "com.horo.updates",
                         .keyId = "key-1",
                         .artifactDigest = package.digest,
                         .signature = std::vector<std::byte>(64U, std::byte{1})};
    auto roots = std::make_shared<Horo::Security::TrustedRootStore>();
    std::vector<std::byte> key(65U, std::byte{1});
    key.front() = std::byte{0x04};
    REQUIRE(roots->Add({.publisherId = "com.horo.updates", .keyId = "key-1", .publicKey = std::move(key)}).HasValue());
    const Horo::Security::ArtifactVerifier verifier{std::make_shared<AcceptingProvider>(), roots};
    auto checkpoint = PartialCheckpoint(package);
    checkpoint.durableBytes = package.size;
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, bytes, verifier).HasValue());
    bytes[0] = std::byte{1};
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, bytes, verifier).HasError());
    bytes[0] = std::byte{};
    checkpoint.durableBytes = package.size - 1U;
    CHECK(VerifyCompletedUpdateTransfer(package, checkpoint, bytes, verifier).HasError());
}
